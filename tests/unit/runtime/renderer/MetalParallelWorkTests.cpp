#include "MetalParallelWorkTestSupport.h"

namespace Horo::Render::MetalBackendTests {
    TEST_CASE("Real host workers record independent native slots and owner presents compiled order after source destruction",
              "[unit][renderer][parallel][native]") {
        JobSystem jobs{{.workerCount = 2}};
        ParallelHostFixture fixture;
        auto frame = BeginParallelTestFrame(*fixture.frontend);
        {
            auto input = MakeParallelPrimaryGraph(
                {PrimaryOutputAttachment{.clearColor = {1, 0, 0, 1}}, PrimaryOutputAttachment{.clearColor = {0, 1, 0, 1}}});
            REQUIRE(frame.PrepareParallelGraphExecution(jobs, input.graph, input.workloads).HasValue());
            input.workloads[0].workload = std::monostate{};
            input.workloads[1].workload = std::monostate{};
        }
        auto moved = std::move(frame);
        CHECK(frame.PollParallelExecution().HasError());
        AwaitWorker([&] {
            return fixture.observation->entered[0].load() && fixture.observation->finished[1].load();
        });
        REQUIRE(moved.PollParallelExecution().Value() == RenderParallelExecutionProgress::Pending);
        CHECK_FALSE(fixture.observation->accepted);
        CHECK(fixture.observation->submitted.empty());
        CHECK(fixture.observation->workerThreads[0] != std::this_thread::get_id());
        CHECK(fixture.observation->workerThreads[1] != std::this_thread::get_id());
        CHECK(fixture.observation->workerThreads[0] != fixture.observation->workerThreads[1]);
        fixture.observation->releaseFirst.store(true);
        REQUIRE(AwaitOwner(moved).Value() == RenderParallelExecutionProgress::Executed);
        CHECK(fixture.observation->accepted);
        CHECK(fixture.observation->submitted.empty());
        REQUIRE(moved.Present().HasValue());
        REQUIRE(fixture.observation->submitted.size() == 2);
        CHECK(std::get<PrimaryOutputAttachment>(fixture.observation->submitted[0]).clearColor.red == 1);
        CHECK(std::get<PrimaryOutputAttachment>(fixture.observation->submitted[1]).clearColor.green == 1);
        CHECK(fixture.observation->submitThread == std::this_thread::get_id());
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Native worker cancellation failure and owner shutdown never publish partial records",
              "[unit][renderer][parallel][native][lifecycle]") {
        JobSystem jobs{{.workerCount = 2}};
        ParallelHostFixture fixture;
        const auto input = MakeParallelPrimaryGraph();
        CancellationSource cancellation;
        SECTION("native error identity remains intact") {
            fixture.observation->failIndex = 1;
        }
        auto frame = BeginParallelTestFrame(*fixture.frontend);
        REQUIRE(frame.PrepareParallelGraphExecution(jobs, input.graph, input.workloads, cancellation.Token()).HasValue());
        AwaitWorker([&] {
            return fixture.observation->entered[0].load() && fixture.observation->entered[1].load();
        });
        SECTION("cancel while encoder is active") {
            cancellation.RequestCancellation();
            REQUIRE(frame.PollParallelExecution().HasError());
        }
        SECTION("destroy owner while encoder is active") {
            fixture.frontend.reset();
            REQUIRE(frame.PollParallelExecution().HasError());
        }
        if (fixture.observation->failIndex == 1) {
            fixture.observation->releaseFirst.store(true);
            Test::RequireError(AwaitOwner(frame), "render.test.native_record_failed");
        }
        CHECK(fixture.observation->submitted.empty());
        CHECK_FALSE(fixture.observation->accepted);
        frame.Cancel();
        jobs.Shutdown(ShutdownPolicy::Cancel);
    }

    TEST_CASE("Native graph admission rejects mismatched bindings and unsupported backend routes before job scheduling",
              "[unit][renderer][parallel][native][admission]") {
        JobSystem jobs;
        ParallelHostFixture fixture;
        auto frame = BeginParallelTestFrame(*fixture.frontend);
        const auto input = MakeParallelPrimaryGraph();
        const std::array reversed{input.workloads[1], input.workloads[0]};
        REQUIRE(frame.PrepareParallelGraphExecution(jobs, input.graph, reversed).HasError());
        CHECK_FALSE(fixture.observation->entered[0].load());
        CHECK_FALSE(fixture.observation->entered[1].load());
        REQUIRE(frame.Execute({}).HasValue());
        REQUIRE(frame.Present().HasValue());
        fixture.frontend.reset();
        // A non-native fake runtime uses the same production backend's explicit opt-out,
        // not a frontend fallback to synchronous encoding.
        auto backend = CreateInitializedBackend(fixture.port, fixture.state, fixture.bridge);
        const auto active = backend->BeginFrame({2, {64, 64}});
        REQUIRE(active.HasValue());
        const std::array ordered{reversed[1], reversed[0]};
        REQUIRE(backend->ParallelRecordingCapabilities().maximumPasses == 0);
        Test::RequireError(backend->PrepareParallelGraph({active.Value(), input.graph, ordered, {}}),
                           "render.metal.unsupported_graph_execution");
    }

    TEST_CASE("Partial native job admission cancels its admitted prefix without owner wait or submission",
              "[unit][renderer][parallel][native][admission][cancellation]") {
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 1}};
        auto occupied = std::make_shared<std::atomic<bool>>(false);
        const auto holder = jobs.SubmitResult({}, [occupied](const CancellationToken &token) {
            occupied->store(true);
            while (!token.IsCancellationRequested())
                std::this_thread::yield();
            return JobCancelled();
        });
        REQUIRE(holder.HasValue());
        AwaitWorker([&] {
            return occupied->load();
        });
        ParallelHostFixture fixture;
        const auto input = MakeParallelPrimaryGraph();
        auto frame = BeginParallelTestFrame(*fixture.frontend);
        REQUIRE(frame.PrepareParallelGraphExecution(jobs, input.graph, input.workloads).HasError());
        CHECK_FALSE(fixture.observation->entered[0].load());
        CHECK_FALSE(fixture.observation->entered[1].load());
        CHECK(fixture.observation->submitted.empty());
        CHECK(fixture.state.abortCount == 1);
        REQUIRE(frame.Present().HasError());
        REQUIRE(fixture.frontend->SubmitFrame({2, {64, 64}}, {}).HasValue());
        jobs.Shutdown(ShutdownPolicy::Cancel);
    }
}  // namespace Horo::Render::MetalBackendTests
