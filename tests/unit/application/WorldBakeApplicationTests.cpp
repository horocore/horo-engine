#include "WorldBakeApplicationTestSupport.h"
#include "assets/AssetCookOutputFixture.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Horo::Application {
    namespace {
        namespace T = Editor::WorldResidencyTests;
        using WorldBakeTestSupport::Drain;
        using WorldBakeTestSupport::Gate;
        using WorldBakeTestSupport::Request;

        /** @brief Verifies typed navigation content and its complete resource closure in both cooked representations. */
        void CheckRepresentedNavigation(const Result<WorldBakeOutput> &output, const WorldBakeRequest &request,
                                        const Assets::AssetId resource, const Assets::AssetTypeId type,
                                        const Result<Assets::AssetCookGeneration> &published) {
            REQUIRE(output.HasValue());
            REQUIRE(output.Value().dependencyGeneration);
            CHECK(output.Value().dependencyGeneration->manifestDigest == published.Value().manifestDigest);
            REQUIRE(output.Value().cells.size() == 1);
            REQUIRE(output.Value().cells.front()->Definition().AssetDependencies().size() == 1);
            CHECK(output.Value().cells.front()->Definition().AssetDependencies().front().id == resource);
            CHECK(output.Value().cells.front()->Definition().AssetDependencies().front().expectedType == type);
            const auto entities = output.Value().cells.front()->Definition().Entities();
            REQUIRE(entities.size() == 2);
            REQUIRE(output.Value().sourceScenes.size() == entities.size());
            for (std::size_t index = 0; index < entities.size(); ++index) {
                const auto authored = request.document->loadedPages[index].content.Objects().front().components.navigationSurface;
                REQUIRE(authored);
                REQUIRE(entities[index].components.navigationSurface);
                CHECK(*entities[index].components.navigationSurface == *authored);
                CHECK(entities[index].components.navigationSurface->definition == resource);
                REQUIRE(output.Value().sourceScenes[index].Entities().front().components.navigationSurface);
                CHECK(*output.Value().sourceScenes[index].Entities().front().components.navigationSurface == *authored);
            }
            CHECK_FALSE(output.Value().artifacts.front().bytes.empty());
        }

        /** @brief Locates one accepted operation in the authoritative store, retaining its complete terminal evidence. */
        OperationRecord Operation(OperationStore &store, const OperationId id) {
            const auto snapshot = store.SnapshotIfChanged(0);
            REQUIRE(snapshot);
            for (const auto &record : snapshot->operations)
                if (record.id == id)
                    return record;
            FAIL("Accepted operation disappeared");
            return {};
        }
    }  // namespace

    TEST_CASE("World bake cooks populated real page content and encodes complete canonical cells", "[world_bake]") {
        Editor::PartitionedWorldWorkspace workspace{T::Asset(9)};
        auto request = Request(workspace);
        std::vector<float> progress;
        auto cooked = CookWorldPages(request, {}, [&](const float value) {
            progress.push_back(value);
        });
        REQUIRE(cooked.HasValue());
        const auto &output = cooked.Value();
        CHECK(output.document == request.document->fence);
        CHECK(output.project == request.project.fence);
        REQUIRE(output.validation.CanPublish());
        REQUIRE(output.cells.size() == 1);
        REQUIRE(output.artifacts.size() == 1);
        CHECK_FALSE(output.artifacts.front().bytes.empty());
        CHECK(output.sourceScenes.size() == 2);
        const auto entities = output.cells.front()->Definition().Entities();
        REQUIRE(entities.size() == 2);
        CHECK(entities.front().object != entities.back().object);
        CHECK(entities.front().localTransform == request.document->loadedPages.front().content.Objects().front().localTransform);
        CHECK(request.document->loadedPages.front().content.Objects().front().id.value == 1);
        CHECK(std::ranges::is_sorted(progress));
        CHECK(progress.back() == 1.0F);
        auto repeated = CookWorldPages(request);
        REQUIRE(repeated.HasValue());
        CHECK(repeated.Value().artifacts.front().bytes == output.artifacts.front().bytes);
    }

    TEST_CASE("World bake consumes actual selected committed resource bytes and rejects generation drift", "[world_bake][native]") {
        namespace A = Assets::OutputTestSupport;
        A::TempDir directory;
        const auto target = A::Target("headless-null");
        const auto resource = T::Asset(24);
        const auto type = A::Type("core.navmesh");
        const std::vector<std::uint8_t> bytes(32, 0x42);
        auto envelope = Assets::EncodeCookedArtifact(
            {.id = resource, .type = type, .target = target, .payloadDigest = A::DigestOf(bytes), .payload = bytes});
        REQUIRE(envelope.HasValue());
        const std::vector<std::vector<std::uint8_t>> payloads{std::move(envelope).Value()};
        const std::vector entries{Assets::AssetCookManifestEntry{.assetId = resource,
                                                                 .assetType = type,
                                                                 .artifactFile = resource.ToString() + ".cooked",
                                                                 .artifactHash = A::DigestOf(payloads.front())}};
        auto published = A::PublishFixture(directory.path, target, entries, payloads);
        REQUIRE(published.HasValue());
        Editor::PartitionedWorldWorkspace workspace{T::Asset(9)};
        auto request = Request(workspace, {}, resource);
        request.project.dependencySource = WorldBakeDependencySource{directory.path, target, std::make_shared<NativeDurableFileSystem>()};
        SECTION("complete manifest and real envelope evidence admit a represented navigation resource") {
            const auto output = CookWorldPages(request);
            CheckRepresentedNavigation(output, request, resource, type, published);
        }
        SECTION("missing selected dependency remains an error") {
            REQUIRE(A::PublishFixture(directory.path, target, {}, {}).HasValue());
            T::ErrorIs(CookWorldPages(request), "world.bake.dependency");
        }
        SECTION("a real pointer replacement after conversion rejects the whole candidate") {
            const auto output = CookWorldPages(request, {}, [&](const float progress) {
                if (progress == 0.25F)
                    REQUIRE(A::PublishFixture(directory.path, target, {}, {}).HasValue());
            });
            T::ErrorIs(output, "world.bake.stale");
        }
        SECTION("selected target cannot be inferred from published data") {
            request.project.dependencySource->target = A::Target("different-target");
            T::ErrorIs(CookWorldPages(request), "world.bake.dependency");
        }
        SECTION("mixed dependency authorities are rejected") {
            auto evidence = CaptureWorldBakeDependencies(*request.project.dependencySource->files, published.Value(), 1, 1024 * 1024);
            REQUIRE(evidence.HasValue());
            request.project.dependencies = std::move(evidence).Value();
            T::ErrorIs(CookWorldPages(request), "world.bake.dependency");
        }
        SECTION("stale registry capture is rejected before reading") {
            request.project.fence.registryRevision = request.registry.Revision().value + 1;
            T::ErrorIs(CookWorldPages(request), "world.bake.stale");
        }
    }

    TEST_CASE("World bake rejects imported mesh content that has no runtime entity representation", "[world_bake]") {
        Editor::PartitionedWorldWorkspace workspace{T::Asset(9)};
        auto request = Request(workspace, T::Asset(24));
        REQUIRE(request.document->loadedPages.front().content.Objects().front().meshAsset == T::Asset(24));
        SECTION("otherwise valid content fails closed with its exact page origin") {
            const auto result = CookWorldPages(request);
            T::ErrorIs(result, "world.bake.unsupported");
            REQUIRE_FALSE(result.ErrorValue().diagnostics.empty());
            CHECK(result.ErrorValue().diagnostics.front().location.source == request.sources.front().source.absolutePath);
        }
        SECTION("project admission errors retain precedence over unsupported content") {
            request.project.fence.schemas = {};
            T::ErrorIs(CookWorldPages(request), "world.bake.invalid");
        }
        SECTION("cancellation retains precedence over unsupported content") {
            CancellationSource cancellation;
            cancellation.RequestCancellation();
            T::ErrorIs(CookWorldPages(request, cancellation.Token()), "world.bake.cancelled");
        }
        SECTION("source capacity retains precedence over unsupported content") {
            request.limits.maximumSourceBytes = 1;
            T::ErrorIs(CookWorldPages(request), "world.bake.capacity");
        }
        CHECK(T::Current(workspace) == request.document);
    }

    TEST_CASE("World bake preserves canonical errors and returns no partial outputs on boundary failures", "[world_bake]") {
        Editor::PartitionedWorldWorkspace workspace{T::Asset(9)};
        auto request = Request(workspace);
        SECTION("missing actual page cannot be replaced by empty payload") {
            REQUIRE(workspace.UnloadPage(T::Asset(2), T::Current(workspace)->fence).HasValue());
            request.document = T::Current(workspace);
            T::ErrorIs(CookWorldPages(request), "world.bake.missing-page");
        }
        SECTION("zero capacity") {
            request.limits.maximumCells = 0;
            T::ErrorIs(CookWorldPages(request), "world.bake.capacity");
        }
        SECTION("aggregate entity capacity") {
            request.limits.maximumObjects = 1;
            T::ErrorIs(CookWorldPages(request), "world.bake.capacity");
        }
        SECTION("malformed captured schema is rejected even when source content does not require it") {
            const auto component = Gameplay::ComponentTypeId::Parse("game.world_bake.component");
            REQUIRE(component.HasValue());
            request.project.components.push_back({component.Value(), 0});
            T::ErrorIs(CookWorldPages(request), "world.bake.invalid");
        }
        SECTION("missing exact schema authority") {
            request.project.fence.schemas = {};
            T::ErrorIs(CookWorldPages(request), "world.bake.invalid");
        }
        SECTION("invalid mode") {
            request.mode = static_cast<WorldBakeMode>(99);
            T::ErrorIs(CookWorldPages(request), "world.bake.invalid");
        }
        SECTION("pre-cancelled") {
            CancellationSource cancellation;
            cancellation.RequestCancellation();
            T::ErrorIs(CookWorldPages(request, cancellation.Token()), "world.bake.cancelled");
        }
        SECTION("relative diagnostic origin") {
            request.sources.front().source.absolutePath = "relative";
            T::ErrorIs(CookWorldPages(request), "world.bake.invalid");
        }
        SECTION("encoded output capacity") {
            request.limits.maximumOutputBytes = 1;
            CHECK(CookWorldPages(request).HasError());
        }
        CHECK(T::Current(workspace) == request.document);
    }

    TEST_CASE("World build validation emits navigable real budget errors and prevents artifact publication", "[world_bake]") {
        Editor::PartitionedWorldWorkspace workspace{T::Asset(9)};
        auto request = Request(workspace);
        request.limits.validation.maximumCellBytes = 1;
        auto result = CookWorldPages(request);
        REQUIRE(result.HasValue());
        CHECK_FALSE(result.Value().validation.CanPublish());
        REQUIRE_FALSE(result.Value().validation.diagnostics.empty());
        CHECK(result.Value().validation.diagnostics.front().source.has_value());
        CHECK(result.Value().artifacts.empty());
    }

    TEST_CASE("World bake operation publishes only a fresh successful bake and validation retains prior output", "[world_bake]") {
        Editor::PartitionedWorldWorkspace workspace{T::Asset(9)};
        auto request = Request(workspace);
        JobSystem jobs{{.workerCount = 1}};
        OperationStore operations{8, 16};
        BuildOutputStore diagnostics{32};
        WorldBakeApplication application{jobs, operations, diagnostics};
        const auto first = application.Submit(request);
        REQUIRE(first.HasValue());
        Drain(jobs);
        application.Pump(request.document, request.project.fence);
        REQUIRE(application.Published());
        const auto published = application.Published();
        CHECK(Operation(operations, first.Value()).state == OperationState::Succeeded);
        const auto output = diagnostics.SnapshotIfChanged(0);
        REQUIRE(output);
        CHECK(output->records.back().operationId == first.Value());
        REQUIRE(output->records.back().sessionId);
        request.mode = WorldBakeMode::Validate;
        const auto validation = application.Submit(request);
        REQUIRE(validation.HasValue());
        CHECK(validation.Value() != first.Value());
        Drain(jobs);
        application.Pump(request.document, request.project.fence);
        CHECK(Operation(operations, validation.Value()).state == OperationState::Succeeded);
        CHECK(application.Published() == published);
    }

    TEST_CASE("Cancelled stale replaced and shutdown world bakes cannot publish stale diagnostics", "[world_bake]") {
        Editor::PartitionedWorldWorkspace workspace{T::Asset(9)};
        auto request = Request(workspace);
        JobSystem jobs{{.workerCount = 1}};
        OperationStore operations{8, 16};
        BuildOutputStore diagnostics{32};
        WorldBakeApplication application{jobs, operations, diagnostics};
        Gate gate{jobs};
        const auto accepted = application.Submit(request);
        REQUIRE(accepted.HasValue());
        bool replacement{};
        auto current = request.document;
        auto project = request.project.fence;
        SECTION("cancel queued work") {
            REQUIRE(operations.RequestCancel(accepted.Value()));
        }
        SECTION("document replaced") {
            REQUIRE(workspace.Replace(T::Document(), current->fence).HasValue());
            current = T::Current(workspace);
        }
        SECTION("project publication changed") {
            ++project.revision;
        }
        SECTION("schema publication changed") {
            ++project.schemas.bytes.front();
        }
        SECTION("replacement has a fresh ID and old publisher is fenced") {
            auto retry = application.Submit(request);
            REQUIRE(retry.HasValue());
            CHECK(retry.Value() != accepted.Value());
            replacement = true;
        }
        SECTION("shutdown") {
            application.Close();
            T::ErrorIs(application.Submit(request), "world.bake.closed");
        }
        gate.Release();
        Drain(jobs);
        application.Pump(current, project);
        CHECK(Operation(operations, accepted.Value()).state == OperationState::Cancelled);
        CHECK_FALSE(application.Published());
        CHECK_FALSE(diagnostics.SnapshotIfChanged(0));
        if (replacement) {
            Drain(jobs);
            application.Pump(current, project);
            CHECK(application.Published());
        }
    }

    TEST_CASE("Rejected operation admission preserves active work and query leases survive owner teardown", "[world_bake]") {
        Editor::PartitionedWorldWorkspace workspace{T::Asset(9)};
        auto request = Request(workspace);
        JobSystem jobs{{.workerCount = 1}};
        OperationStore operations{1, 16};
        BuildOutputStore diagnostics{32};
        WorldBakeApplication application{jobs, operations, diagnostics};
        Gate gate{jobs};
        const auto accepted = application.Submit(request);
        REQUIRE(accepted.HasValue());
        T::ErrorIs(application.Submit(request), "world.bake.capacity");
        gate.Release();
        Drain(jobs);
        application.Pump(request.document, request.project.fence);
        REQUIRE(application.Published());
        const auto retained = application.Published();
        application.Close();
        REQUIRE(workspace.Shutdown().HasValue());
        CHECK(retained->cells.front()->Definition().Entities().size() == 2);
        CHECK(retained->sourceScenes.front().Entities().size() == 1);
    }
}  // namespace Horo::Application
