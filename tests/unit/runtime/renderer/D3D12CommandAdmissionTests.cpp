#include "../../../support/renderer/RenderGraphSubmissionFixture.h"
#include "D3D12CommandRuntime.h"
#include "RenderGraphTestUtils.h"

#include <array>
#include <thread>

namespace Horo::Render::D3D12AdmissionTest {
    using namespace Horo;
    using SubmissionTest::Take;

    struct Lease final : IRenderGraphResourceLease {
        unsigned releases{};

        void Release() noexcept override {
            ++releases;
        }
    };

    /** @brief Deliberate fake: only admission/ownership behavior, never native execution evidence. */
    struct Runtime final : ID3D12CommandRuntime {
        bool supportedEnhanced{}, failPrepare{}, failBeforeExecute{}, failAfterExecute{}, completed{}, stopped{};
        unsigned prepares{}, submits{}, aborts{}, polls{}, observations{}, executions{};
        std::array<IRenderGraphResourceLease *, 32> pending{};
        std::size_t pendingCount{};
        CancellationSource *cancelDuringPrepare{};
        ID3D12Commands *reenter{};
        bool busyRejected{};

        Result<void> Initialize(const D3D12CommandConfiguration &config) override {
            if (config.barrierMode == D3D12BarrierMode::Enhanced && !supportedEnhanced)
                return Result<void>::Failure(
                    D3D12CommandError("render.d3d12.enhanced_unsupported", "Enhanced mode is not supported by this deliberate fake."));
            return Result<void>::Success();
        }

        Result<void> PrepareGraph(const RenderGraphExecutionRequest &, const CancellationToken &) override {
            ++prepares;
            if (cancelDuringPrepare)
                cancelDuringPrepare->RequestCancellation();
            if (reenter) {
                auto nested = reenter->Poll();
                busyRejected = nested.HasError() && nested.ErrorValue().code.Value() == "render.d3d12.command_busy";
            }
            if (failPrepare)
                return Result<void>::Failure(D3D12CommandError("render.d3d12.test_prepare", "Deliberate unsent preparation failure."));
            return Result<void>::Success();
        }

        Result<void> SubmitPrepared(IRenderGraphResourceLease *lease, RenderGraphLeaseTransfer *transfer) override {
            ++submits;
            if (failBeforeExecute)
                return Result<void>::Failure(D3D12CommandError("render.d3d12.test_submit", "Deliberate failure before queue execution."));
            if (transfer)
                transfer->authority = RenderGraphLeaseAuthority::BackendCompletion;
            pending[pendingCount++] = lease;
            ++executions;
            if (failAfterExecute)
                return Result<void>::Failure(D3D12CommandError("render.d3d12.test_signal", "Deliberate accepted-prefix signal failure."));
            return Result<void>::Success();
        }

        void AbortPrepared() noexcept override {
            ++aborts;
        }

        Result<bool> Observe(const RenderCpuWait &) override {
            ++observations;
            return Result<bool>::Success(completed);
        }

        Result<void> Poll() override {
            ++polls;
            if (completed) {
                for (std::size_t index = 0; index < pendingCount; ++index)
                    if (pending[index])
                        pending[index]->Release();
                pendingCount = 0;
            }
            return Result<void>::Success();
        }

        void StopAdmission() noexcept override {
            stopped = true;
        }

        Result<RenderBackendDrainProgress> PollDrain() override {
            auto polled = Poll();
            if (polled.HasError())
                return Result<RenderBackendDrainProgress>::Failure(polled.ErrorValue());
            return Result<RenderBackendDrainProgress>::Success(pendingCount ? RenderBackendDrainProgress::Pending
                                                                            : RenderBackendDrainProgress::Drained);
        }
    };

    struct Fixture {
        SubmissionTest::BufferGraph packet =
            SubmissionTest::CopyGraph({RenderBufferHandle{{41}, 1, 1}, RenderBufferHandle{{41}, 2, 1}, RenderBufferHandle{{41}, 3, 1}}, {1},
                                      {1});
        std::array<RenderGraphQueueSubmission, 2> submissions = packet.Submissions();
        Lease lease;
        RenderGraphLeaseTransfer transfer;
        Runtime *runtime{};
        std::unique_ptr<ID3D12Commands> commands;

        explicit Fixture(const D3D12CommandLimits &limits = {}) {
            auto native = std::make_unique<Runtime>();
            runtime = native.get();
            const std::array queues{D3D12CommandQueueBinding{{1}, D3D12QueueKind::Direct}};
            commands = Take(CreateD3D12CommandService(std::move(native), {17, queues, D3D12BarrierMode::Legacy, limits}));
        }

        ~Fixture() {
            if (!commands)
                return;
            runtime->completed = true;
            static_cast<void>(commands->BeginShutdown());
            static_cast<void>(commands->PollDrain());
        }

        RenderGraphExecutionRequest Request() {
            return {.frame = {1},
                    .graph = packet.graph,
                    .workloads = packet.workloads,
                    .resources = packet.instances,
                    .lease = &lease,
                    .transfer = &transfer,
                    .submissions = submissions};
        }

        void Next(const std::uint64_t firstOrder, const std::uint64_t firstSignal) {
            packet.producerWait[0] = {{1}, firstSignal};
            submissions[0].submission = {RenderQueueId{1}, {firstOrder}, {}, {{1}, firstSignal}};
            submissions[1].submission = {RenderQueueId{1}, {firstOrder + 1}, packet.producerWait, {{1}, firstSignal + 1}};
            transfer = {};
        }
    };
}  // namespace Horo::Render::D3D12AdmissionTest

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::D3D12AdmissionTest;
    using Horo::Render::Test::RequireError;
}  // namespace

TEST_CASE("D3D12 portable command policy preserves exact queues and required barrier mode", "[renderer][d3d12][command-admission]") {
    std::array queues{D3D12CommandQueueBinding{{1}, D3D12QueueKind::Direct}};
    auto config = D3D12CommandConfiguration{17, queues};
    std::string_view expected = "render.d3d12.command_configuration";
    SECTION("invalid capacity") {
        config.limits.maximumTimelineRanges = 0;
    }
    SECTION("invalid device") {
        config.deviceIncarnation = 0;
    }
    SECTION("unknown negative queue class") {
        queues[0].kind = static_cast<D3D12QueueKind>(-1);
        expected = "render.d3d12.queue_invalid";
    }
    SECTION("required enhanced never falls back") {
        config.barrierMode = D3D12BarrierMode::Enhanced;
        expected = "render.d3d12.enhanced_unsupported";
    }
    auto created = CreateD3D12CommandService(std::make_unique<Runtime>(), config);
    RequireError(created, expected);
    CHECK(created.ErrorValue().domain.Value() == "horo.render.d3d12");
}

TEST_CASE("D3D12 portable admission rejects frame receipt and workload failures before native preparation",
          "[renderer][d3d12][command-admission]") {
    Fixture fixture;
    auto request = fixture.Request();
    std::string_view expected = "render.d3d12.command_frame";
    SECTION("invalid frame") {
        request.frame = {};
    }
    SECTION("unsupported UI role") {
        request.role = RenderGraphExecutionRole::UiComposition;
    }
    SECTION("reused transfer receipt") {
        fixture.transfer.authority = RenderGraphLeaseAuthority::BackendCompletion;
        expected = "render.d3d12.lease_authority";
    }
    SECTION("missing lease") {
        request.lease = nullptr;
        expected = "render.d3d12.lease_missing";
    }
    SECTION("missing explicit submission") {
        request.submissions = {};
        expected = "render.d3d12.submission_required";
    }
    SECTION("later batch order invalid") {
        fixture.submissions[1].submission.order = {1};
        expected = "render.graph.execution.synchronization_invalid";
    }
    RequireError(fixture.commands->ExecuteGraph(request), expected);
    CHECK(fixture.runtime->prepares == 0);
    CHECK(fixture.runtime->executions == 0);
    CHECK(fixture.lease.releases == 0);
}

TEST_CASE("D3D12 portable cancellation and preparation failure keep lease authority with frontend",
          "[renderer][d3d12][command-admission]") {
    Fixture fixture;
    CancellationSource cancellation;
    std::string_view expected = "render.d3d12.command_cancelled";
    SECTION("cancel before prepare") {
        cancellation.RequestCancellation();
    }
    SECTION("cancel after prepare") {
        fixture.runtime->cancelDuringPrepare = &cancellation;
    }
    SECTION("preparation failure") {
        fixture.runtime->failPrepare = true;
        expected = "render.d3d12.test_prepare";
    }
    RequireError(fixture.commands->ExecuteGraph(fixture.Request(), cancellation.Token()), expected);
    CHECK(fixture.runtime->submits == 0);
    CHECK(fixture.transfer.authority == RenderGraphLeaseAuthority::Frontend);
    CHECK(fixture.lease.releases == 0);
}

TEST_CASE("D3D12 portable failed submit retains only actual completion ownership and closes admission",
          "[renderer][d3d12][command-admission]") {
    Fixture fixture;
    bool accepted = false;
    std::string_view expected = "render.d3d12.test_submit";
    SECTION("before first execute") {
        fixture.runtime->failBeforeExecute = true;
    }
    SECTION("accepted prefix signal failure") {
        fixture.runtime->failAfterExecute = true;
        accepted = true;
        expected = "render.d3d12.test_signal";
    }
    RequireError(fixture.commands->ExecuteGraph(fixture.Request()), expected);
    CHECK(fixture.runtime->stopped);
    CHECK((fixture.transfer.authority == RenderGraphLeaseAuthority::BackendCompletion) == accepted);
    CHECK(fixture.lease.releases == 0);
    RequireError(fixture.commands->ExecuteGraph(fixture.Request()), "render.d3d12.command_closed");
    RequireError(fixture.commands->Observe({{{1}, 1}, RenderCpuWaitPurpose::Poll, {}, 17}), "render.d3d12.timeline_unissued");
    auto pending = fixture.commands->PollDrain();
    REQUIRE(pending.HasValue());
    CHECK((pending.Value() == RenderBackendDrainProgress::Pending) == accepted);
    fixture.runtime->completed = true;
    REQUIRE(fixture.commands->PollDrain().HasValue());
    CHECK(fixture.lease.releases == static_cast<unsigned>(accepted));
    REQUIRE(fixture.commands->PollDrain().HasValue());
    CHECK(fixture.lease.releases == static_cast<unsigned>(accepted));
}

TEST_CASE("D3D12 portable timeline observation rejects foreign future and skipped values", "[renderer][d3d12][command-admission]") {
    Fixture fixture;
    fixture.Next(1, 3);
    REQUIRE(fixture.commands->ExecuteGraph(fixture.Request()).HasValue());
    CHECK(fixture.transfer.authority == RenderGraphLeaseAuthority::BackendCompletion);
    RequireError(fixture.commands->Observe({{{1}, 3}, RenderCpuWaitPurpose::Poll, {}, 18}), "render.d3d12.timeline_stale");
    RequireError(fixture.commands->Observe({{{1}, 5}, RenderCpuWaitPurpose::Poll, {}, 17}), "render.d3d12.timeline_unissued");
    RequireError(fixture.commands->Observe({{{1}, 2}, RenderCpuWaitPurpose::Poll, {}, 17}), "render.d3d12.timeline_unissued");
    auto pending = fixture.commands->Observe({{{1}, 3}, RenderCpuWaitPurpose::Poll, {}, 17});
    REQUIRE(pending.HasValue());
    CHECK_FALSE(pending.Value());
}

TEST_CASE("D3D12 completed receipts survive polling and remain valid dependencies while capacity remains",
          "[renderer][d3d12][command-admission]") {
    Fixture fixture;
    REQUIRE(fixture.commands->ExecuteGraph(fixture.Request()).HasValue());
    fixture.runtime->completed = true;
    REQUIRE(fixture.commands->Poll().HasValue());
    auto observed = fixture.commands->Observe({{{1}, 1}, RenderCpuWaitPurpose::Poll, {}, 17});
    REQUIRE(observed.HasValue());
    CHECK(observed.Value());
    fixture.Next(3, 5);
    const std::array previous{RenderTimelinePoint{{1}, 2}};
    fixture.submissions[0].submission.waits = previous;
    REQUIRE(fixture.commands->ExecuteGraph(fixture.Request()).HasValue());
    CHECK(fixture.runtime->submits == 2);
}

TEST_CASE("D3D12 receipt pressure retires only completed ranges and supports long sparse streams", "[renderer][d3d12][command-admission]") {
    D3D12CommandLimits limits;
    limits.maximumTimelineRanges = 2;
    Fixture fixture{limits};
    fixture.runtime->completed = true;
    for (std::uint64_t index = 0; index < 16; ++index) {
        fixture.Next(index * 2 + 1, index * 4 + 1);
        REQUIRE(fixture.commands->ExecuteGraph(fixture.Request()).HasValue());
    }
    CHECK(fixture.runtime->submits == 16);
    RequireError(fixture.commands->Observe({{{1}, 1}, RenderCpuWaitPurpose::Poll, {}, 17}), "render.d3d12.receipt_expired");
    auto current = fixture.commands->Observe({{{1}, 61}, RenderCpuWaitPurpose::Poll, {}, 17});
    REQUIRE(current.HasValue());
    CHECK(current.Value());
}

TEST_CASE("D3D12 pending receipt pressure fails before prepare without losing native pins", "[renderer][d3d12][command-admission]") {
    D3D12CommandLimits limits;
    limits.maximumTimelineRanges = 1;
    Fixture fixture{limits};
    REQUIRE(fixture.commands->ExecuteGraph(fixture.Request()).HasValue());
    fixture.Next(3, 5);
    RequireError(fixture.commands->ExecuteGraph(fixture.Request()), "render.d3d12.receipt_capacity");
    CHECK(fixture.runtime->prepares == 1);
    CHECK(fixture.runtime->pendingCount == 1);
    CHECK(fixture.lease.releases == 0);
}

TEST_CASE("D3D12 portable owner and reentrant shutdown rules remain explicit", "[renderer][d3d12][command-admission]") {
    Fixture fixture;
    fixture.runtime->reenter = fixture.commands.get();
    REQUIRE(fixture.commands->ExecuteGraph(fixture.Request()).HasValue());
    CHECK(fixture.runtime->busyRejected);
    bool rejected = false;
    std::thread worker{[&] {
        auto result = fixture.commands->Poll();
        rejected = result.HasError() && result.ErrorValue().code.Value() == "render.d3d12.command_thread";
    }};
    worker.join();
    CHECK(rejected);
    RequireError(fixture.commands->PollDrain(), "render.d3d12.command_shutdown_required");
    REQUIRE(fixture.commands->BeginShutdown().HasValue());
    REQUIRE(fixture.commands->BeginShutdown().HasValue());
    RequireError(fixture.commands->ExecuteGraph(fixture.Request()), "render.d3d12.command_closed");
}
