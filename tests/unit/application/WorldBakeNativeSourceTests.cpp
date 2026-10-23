#include "../../support/OwnedTestDirectory.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "WorldBakeApplicationTestSupport.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

namespace Horo::Application {
    namespace {
        namespace T = Editor::WorldResidencyTests;
        using WorldBakeTestSupport::Drain;
        using WorldBakeTestSupport::Gate;
        using WorldBakeTestSupport::Request;

        /** @brief Owns actual native source files, root authority and real source-page metadata in destruction-safe order. */
        struct NativeSources final {
            Editor::PartitionedWorldWorkspace workspace{T::Asset(9)};
            Tests::OwnedTestDirectory directory{"world bake ü space"};
            WorldBakeRequest request;

            NativeSources() : request(Request(workspace)) {
                for (std::size_t index = 0; index < request.sources.size(); ++index) {
                    const auto path = "page ü " + std::to_string(index) + ".scene";
                    request.sources[index].relativePath = path;
                    const auto nativePath = directory.Path() / std::filesystem::u8path(path);
                    const auto absolutePath = nativePath.generic_u8string();
                    request.sources[index].source.absolutePath.assign(absolutePath.begin(), absolutePath.end());
                    std::ofstream stream{nativePath, std::ios::binary};
                    stream << T::Source();
                    REQUIRE(stream.good());
                    REQUIRE(workspace.UnloadPage(request.sources[index].page.sourceAsset, T::Current(workspace)->fence).HasValue());
                }
                request.document = T::Current(workspace);
                request.project.fence.revision = 7;
                request.sourceAuthority.owner = request.project.fence.project;
                request.sourceAuthority.generation = 1;
                const auto reader = Platform::CreateNativeProjectReadFiles(directory.Path(), request.sourceAuthority.owner.ToString(),
                                                                           request.sourceAuthority.generation);
                REQUIRE(reader.HasValue());
                request.sourceAuthority.files = reader.Value();
            }
        };

        /** @brief Confirms canonical conversion rejects malformed mesh content before unsupported-content admission. */
        void CheckConverterErrorPrecedence(const WorldBakeRequest &request, Editor::PartitionedWorldWorkspace &workspace) {
            auto decoded = SceneSource::DecodeSceneSource(T::Source());
            REQUIRE(decoded.HasValue());
            auto document = std::move(decoded).Value();
            document.objects.front().meshAsset = T::Asset(24);
            document.objects.front().parent = document.objects.front().id;
            const auto bytes = SceneSource::EncodeSceneSource({document.objects, document.prefabInstances});
            const auto reparsed = SceneSource::DecodeSceneSource(bytes);
            REQUIRE(reparsed.HasValue());
            const auto expected =
                SceneSource::ConvertSceneSourceToRuntime({reparsed.Value().objects, reparsed.Value().prefabInstances},
                                                         Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1});
            REQUIRE(expected.HasError());
            {
                std::ofstream stream{std::filesystem::u8path(request.sources.front().source.absolutePath), std::ios::binary};
                stream << bytes;
                REQUIRE(stream.good());
            }
            const auto result = CookWorldPages(request);
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.ErrorValue().code.Value());
            REQUIRE_FALSE(result.ErrorValue().diagnostics.empty());
            CHECK(result.ErrorValue().diagnostics.front().location.source == request.sources.front().source.absolutePath);
            CHECK(T::Current(workspace)->loadedPages.empty());
        }
    }  // namespace

    TEST_CASE("World bake reads actual unloaded source pages through the native root authority and fences byte changes",
              "[world_bake][native]") {
        NativeSources fixture;
        auto &request = fixture.request;
        auto &workspace = fixture.workspace;
        SECTION("real canonical source bytes cook without mutating manual residency") {
            const auto result = CookWorldPages(request);
            REQUIRE(result.HasValue());
            CHECK(result.Value().sourceRevisions.size() == 2);
            REQUIRE(result.Value().cells.size() == 1);
            CHECK(result.Value().cells.front()->Definition().Entities().size() == 2);
            CHECK(T::Current(workspace)->loadedPages.empty());
        }
        SECTION("source bytes changed after capture never escape as publishable output") {
            const auto result = CookWorldPages(request, {}, [&](const float value) {
                if (value == 0.25F) {
                    std::ofstream stream{std::filesystem::u8path(request.sources.front().source.absolutePath), std::ios::binary};
                    stream << T::Source("Externally changed");
                    REQUIRE(stream.good());
                }
            });
            T::ErrorIs(result, "world.bake.stale");
        }

        SECTION("imported mesh sources cannot silently publish geometry-free cells") {
            auto source = SceneSource::DecodeSceneSource(T::Source());
            REQUIRE(source.HasValue());
            auto document = std::move(source).Value();
            document.objects.front().meshAsset = T::Asset(24);
            {
                std::ofstream stream{std::filesystem::u8path(request.sources.front().source.absolutePath), std::ios::binary};
                stream << SceneSource::EncodeSceneSource({document.objects, document.prefabInstances});
                REQUIRE(stream.good());
            }
            T::ErrorIs(CookWorldPages(request), "world.bake.unsupported");
            CHECK(T::Current(workspace)->loadedPages.empty());
        }
        SECTION("converter errors retain precedence over unsupported mesh content") {
            CheckConverterErrorPrecedence(request, workspace);
        }
        SECTION("invalid source authority generation is rejected before reading") {
            request.sourceAuthority.generation = 0;
            T::ErrorIs(CookWorldPages(request), "world.bake.invalid");
        }
        SECTION("aggregate source capacity includes unloaded sources") {
            request.limits.maximumSourceBytes = 1;
            T::ErrorIs(CookWorldPages(request), "world.bake.capacity");
        }
    }

    TEST_CASE("World bake publication and close retire source reservations without retaining writer authority in output",
              "[world_bake][native]") {
        NativeSources fixture;
        auto &request = fixture.request;
        auto &directory = fixture.directory;

        std::filesystem::create_directories(directory.Path() / ".horo/local");
        NativeDurableFileSystem files;
        Editor::ProjectMutationCoordinator mutations{files};
        auto reserved = mutations.TryAcquire({directory.Path(), Editor::ProjectMutationOwner::Asset, "world.bake.test"});
        REQUIRE(reserved.HasValue());
        request.sourceAuthority.reservation = std::make_shared<const Editor::ProjectMutationLease>(std::move(reserved).Value());
        JobSystem jobs{{.workerCount = 1, .ioWorkerCount = 1}};
        OperationStore operations{4, 8};
        BuildOutputStore output{16};
        WorldBakeApplication application{jobs, operations, output};
        Gate gate{jobs, JobResource::Io};
        REQUIRE(application.Submit(request).HasValue());
        request.sourceAuthority.reservation.reset();
        CHECK(mutations.TryAcquire({directory.Path(), Editor::ProjectMutationOwner::Save, "blocked"}).HasError());
        SECTION("successful real I/O bake releases the writer reservation while output remains leased") {
            gate.Release();
            Drain(jobs, JobResource::Io);
            application.Pump(request.document, request.project.fence);
            REQUIRE(application.Published());
            CHECK(application.Published()->cells.front()->Definition().Entities().size() == 2);
            CHECK(mutations.TryAcquire({directory.Path(), Editor::ProjectMutationOwner::Save, "published"}).HasValue());
        }
        SECTION("close cancels queued work and releases its writer reservation") {
            application.Close();
            gate.Release();
            Drain(jobs, JobResource::Io);
            CHECK(mutations.TryAcquire({directory.Path(), Editor::ProjectMutationOwner::Save, "released"}).HasValue());
            CHECK_FALSE(output.SnapshotIfChanged(0));
        }
    }
}  // namespace Horo::Application
