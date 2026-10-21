#include "../../support/OwnedTestDirectory.h"
#include "../editor/PartitionedWorldResidencyTestSupport.h"
#include "Horo/Application/WorldBakeApplication.h"
#include "Horo/Platform/NativeProjectReadFiles.h"

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <mutex>

namespace Horo::Application {
    namespace {
        namespace T = Editor::WorldResidencyTests;
        namespace W = WorldStreaming;

        /** @brief Captures two actual page documents with equal local IDs and distinct immutable page identities. */
        WorldBakeRequest Request(Editor::PartitionedWorldWorkspace &workspace) {
            REQUIRE(workspace.Open(T::Document()).HasValue());
            for (const auto &page : T::Document().pages)
                REQUIRE(workspace.LoadPage(page.head, T::Source(), T::Current(workspace)->fence).HasValue());
            WorldBakeRequest request{.document = T::Current(workspace), .title = "Bake streaming cells"};
            request.project.fence = {T::Asset(12), 1, {}};
            request.project.fence.schemas.bytes.front() = 1;
            request.project.settings.bytes.front() = 2;
            for (const auto &page : request.document->document.pages)
                request.sources.push_back(
                    {page.head, {(std::filesystem::current_path() / (page.head.sourceAsset.ToString() + ".scene")).string(), 1, 1}});
            return request;
        }

        /** @brief Single-worker FIFO marker joins all earlier queued callbacks under an explicit finite policy. */
        void Drain(JobSystem &jobs) {
            auto marker = jobs.SubmitResult({}, [](const CancellationToken &) {
                return Result<void>::Success();
            });
            REQUIRE(marker.HasValue());
            REQUIRE(marker.Value().Wait({WaitPolicy::OwnerThreadBlockAllowed, std::chrono::seconds{5}}).HasValue());
        }

        /** @brief Prevents worker execution until cancellation/replacement/source edits have been deterministically installed. */
        struct Gate final {
            std::mutex mutex;
            std::condition_variable changed;
            bool open{};
            JobHandle job;

            explicit Gate(JobSystem &jobs) {
                auto accepted = jobs.SubmitResult({}, [this](const CancellationToken &) {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] {
                        return open;
                    });
                    return Result<void>::Success();
                });
                REQUIRE(accepted.HasValue());
                job = std::move(accepted).Value();
            }

            ~Gate() {
                Release();
                (void)job.Wait({WaitPolicy::OwnerThreadBlockAllowed, std::chrono::seconds{5}});
            }

            void Release() {
                {
                    std::lock_guard lock(mutex);
                    open = true;
                }
                changed.notify_all();
            }
        };

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

namespace Horo::Application {
    TEST_CASE("World bake reads actual unloaded source pages through the native root authority and fences byte changes",
              "[world_bake][native]") {
        Editor::PartitionedWorldWorkspace workspace{T::Asset(9)};
        auto request = Request(workspace);
        Tests::OwnedTestDirectory directory{"world bake ü space"};
        for (std::size_t index = 0; index < request.sources.size(); ++index) {
            const auto path = "page ü " + std::to_string(index) + ".scene";
            request.sources[index].relativePath = path;
            request.sources[index].source.absolutePath = (directory.Path() / path).string();
            std::ofstream stream{directory.Path() / path};
            stream << T::Source();
            REQUIRE(stream.good());
            REQUIRE(workspace.UnloadPage(request.sources[index].page.sourceAsset, T::Current(workspace)->fence).HasValue());
        }
        request.document = T::Current(workspace);
        const auto reader = Platform::CreateNativeProjectReadFiles(directory.Path(), request.project.fence.project.ToString(),
                                                                   request.project.fence.revision);
        REQUIRE(reader.HasValue());
        request.files = reader.Value();
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
                    std::ofstream stream{request.sources.front().source.absolutePath};
                    stream << T::Source("Externally changed");
                    REQUIRE(stream.good());
                }
            });
            T::ErrorIs(result, "world.bake.stale");
        }
        SECTION("aggregate source capacity includes unloaded sources") {
            request.limits.maximumSourceBytes = 1;
            T::ErrorIs(CookWorldPages(request), "world.bake.capacity");
        }
    }
}  // namespace Horo::Application
