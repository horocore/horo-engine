#include "D3D12CommandTestSupport.h"

#include <limits>
#include <string>

TEST_CASE("D3D12 command slot bounds are finite before native initialization", "[unit][d3d12][commands]") {
    Horo::Render::D3D12ResourceLimits limits;
    CHECK(limits.IsValid());
    limits.maximumCommandBatches = 0;
    CHECK_FALSE(limits.IsValid());
    limits.maximumCommandBatches = 1;
    CHECK(limits.IsValid());
    limits.maximumCommandBatches = 128;
    CHECK(limits.IsValid());
    limits.maximumCommandBatches = 129;
    CHECK_FALSE(limits.IsValid());
}

#if defined(_WIN32) && defined(_M_X64)
namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::D3D12Tests;
    using namespace Horo::Render::D3D12CommandTests;

    TEST_CASE("D3D12 commands execute a real timestamp recording before completion lease release", "[warp][d3d12][commands]") {
        Fixture fixture;
        auto &commands = Commands(fixture);
        CompletionLease lease;
        TimestampProbe timestamp(*fixture.device.Get());
        DrainGuard drain{fixture};
        const auto recording = commands.Begin();
        REQUIRE(recording.HasValue());
        auto borrowed = BorrowD3D12CommandList(*fixture.resources, recording.Value());
        REQUIRE(borrowed.HasValue());

        timestamp.Record(*borrowed.Value());
        REQUIRE(commands.Close(recording.Value()).HasValue());
        RenderGraphLeaseTransfer receipt;
        REQUIRE(commands.Submit(recording.Value(), {{1}, {1}, {}, {{1}, 1}}, &lease, receipt).HasValue());
        CHECK(receipt.authority == RenderGraphLeaseAuthority::BackendCompletion);
        Await(commands, recording.Value());
        CHECK(lease.releases == 1);
        CHECK(lease.releasedOn == std::this_thread::get_id());
        CHECK(timestamp.Value() != 0);
        REQUIRE(commands.Cancel(recording.Value()).HasValue());
    }

    TEST_CASE("D3D12 cancelled open recordings close before allocator reuse", "[warp][d3d12][commands][cancel-reuse]") {
        Fixture fixture({.maximumCommandBatches = 1});
        auto &commands = Commands(fixture);
        const auto first = commands.Begin();
        REQUIRE(first.HasValue());
        REQUIRE(commands.Begin().HasError());
        REQUIRE(commands.Cancel(first.Value()).HasValue());
        CHECK(commands.Close(first.Value()).HasError());
        const auto second = commands.Begin();
        REQUIRE(second.HasValue());
        CHECK(second.Value().deviceIncarnation == fixture.resources->Snapshot().deviceIncarnation);
        CHECK(second.Value().value != first.Value().value);
        REQUIRE(commands.Cancel(second.Value()).HasValue());
    }

    TEST_CASE("D3D12 submitted cancellation retains the slot and lease behind a real queue fence",
              "[warp][d3d12][commands][pending-drain]") {
        Fixture fixture({.maximumCommandBatches = 1});
        auto &commands = Commands(fixture);
        CompletionLease lease;
        DrainGuard drain{fixture};
        QueueGate gate{fixture};
        const auto recording = Closed(commands);
        RenderGraphLeaseTransfer receipt;
        REQUIRE(commands.Submit(recording, {{1}, {1}, {}, {{1}, 1}}, &lease, receipt).HasValue());
        const auto pending = commands.Poll(recording);
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value());
        CHECK(lease.releases == 0);
        REQUIRE(commands.Cancel(recording).HasValue());
        CHECK(commands.Poll(recording).HasError());
        CHECK(commands.Begin().HasError());
        fixture.resources->StopAdmission();
        const auto closing = fixture.resources->PollDrain();
        REQUIRE(closing.HasValue());
        CHECK_FALSE(closing.Value());
        CHECK(lease.releases == 0);
        gate.Open();
        AwaitDrain(fixture);
        CHECK(lease.releases == 1);
        CHECK(lease.releasedOn == std::this_thread::get_id());
        CHECK(fixture.resources->PollDrain().Value());
        CHECK(lease.releases == 1);
    }

    TEST_CASE("D3D12 command fence and ticket counters reject exhaustion without wraparound", "[warp][d3d12][commands][exhaustion]") {
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        SECTION("shared native fence") {
            Fixture fixture({}, {.initialFenceValue = maximum - 1});
            auto &commands = Commands(fixture);
            CompletionLease lease;
            DrainGuard drain{fixture};
            const auto first = Closed(commands);
            RenderGraphLeaseTransfer accepted;
            REQUIRE(commands.Submit(first, {{1}, {1}, {}, {{1}, 1}}, &lease, accepted).HasValue());
            Await(commands, first);
            REQUIRE(commands.Cancel(first).HasValue());
            const auto second = Closed(commands);
            RenderGraphLeaseTransfer rejected;
            const auto exhausted = commands.Submit(second, {{1}, {2}, {}, {{1}, 2}}, nullptr, rejected);
            REQUIRE(exhausted.HasError());
            CHECK(exhausted.ErrorValue().code.Value() == "render.d3d12.command_capacity");
            CHECK(rejected.authority == RenderGraphLeaseAuthority::Frontend);
        }
        SECTION("recording identity") {
            Fixture fixture({}, {.initialCommandId = maximum - 1});
            auto &commands = Commands(fixture);
            const auto first = commands.Begin();
            REQUIRE(first.HasValue());
            CHECK(first.Value().value == maximum - 1);
            REQUIRE(commands.Cancel(first.Value()).HasValue());
            const auto exhausted = commands.Begin();
            REQUIRE(exhausted.HasError());
            CHECK(exhausted.ErrorValue().code.Value() == "render.d3d12.command_capacity");
        }
    }

    TEST_CASE("D3D12 command admission rejects invalid queues states waits and cancellation before transfer",
              "[warp][d3d12][commands][admission]") {
        Fixture fixture;
        auto *commands = fixture.resources->Commands();
        REQUIRE(commands != nullptr);
        CHECK(commands->Begin().HasError());
        CHECK(commands->Configure({0}).HasError());
        REQUIRE(commands->Configure({1}).HasValue());
        CHECK(commands->Configure({2}).HasError());
        const auto recording = Closed(*commands);
        CompletionLease lease;
        DrainGuard drain{fixture};
        RenderGraphLeaseTransfer receipt;
        const RenderTimelinePoint wait{{2}, 1};
        const RenderQueueSubmission waiting{{1}, {1}, {&wait, 1}, {{1}, 1}};
        const auto unsupported = commands->Submit(recording, waiting, &lease, receipt);
        REQUIRE(unsupported.HasError());
        CHECK(unsupported.ErrorValue().code.Value() == "render.d3d12.command_wait_unsupported");
        CHECK(commands->Submit(recording, {{2}, {1}, {}, {{2}, 1}}, &lease, receipt).HasError());
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        const auto cancelled = commands->Submit(recording, {{1}, {1}, {}, {{1}, 1}}, &lease, receipt, cancellation.Token());
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code.Value() == "render.d3d12.cancelled");
        CHECK(receipt.authority == RenderGraphLeaseAuthority::Frontend);
        CHECK(lease.releases == 0);
        REQUIRE(commands->Submit(recording, {{1}, {1}, {}, {{1}, 1}}, &lease, receipt).HasValue());
        Await(*commands, recording);
        CHECK(lease.releases == 1);
        CHECK(commands->Submit(recording, {{1}, {2}, {}, {{1}, 2}}, nullptr, receipt).HasError());
        const auto second = Closed(*commands);
        RenderGraphLeaseTransfer secondReceipt;
        CHECK(commands->Submit(second, {{1}, {1}, {}, {{1}, 2}}, nullptr, secondReceipt).HasError());
        CHECK(commands->Submit(second, {{1}, {2}, {}, {{1}, 1}}, nullptr, secondReceipt).HasError());
        CHECK(BorrowD3D12CommandList(*fixture.resources, recording).HasError());
    }

    TEST_CASE("D3D12 recording errors quarantine their slot without accepting a lease", "[warp][d3d12][commands][recording-error]") {
        Fixture fixture({.maximumCommandBatches = 1});
        auto &commands = Commands(fixture);
        const auto recording = commands.Begin();
        REQUIRE(recording.HasValue());
        auto borrowed = BorrowD3D12CommandList(*fixture.resources, recording.Value());
        REQUIRE(borrowed.HasValue());
        REQUIRE(SUCCEEDED(borrowed.Value()->Close()));
        const auto failed = commands.Close(recording.Value());
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == "render.d3d12.command_close_failed");
        CompletionLease lease;
        RenderGraphLeaseTransfer receipt;
        CHECK(commands.Submit(recording.Value(), {{1}, {1}, {}, {{1}, 1}}, &lease, receipt).HasError());
        CHECK(commands.Cancel(recording.Value()).HasError());
        CHECK(commands.Begin().HasError());
        CHECK(lease.releases == 0);
        CHECK(receipt.authority == RenderGraphLeaseAuthority::Frontend);
    }

    TEST_CASE("D3D12 command identities reject foreign contexts and non-owner threads", "[warp][d3d12][commands][identity]") {
        Fixture first;
        Fixture second;
        auto &commands = Commands(first);
        auto &otherCommands = Commands(second);
        const auto recording = commands.Begin();
        const auto otherRecording = otherCommands.Begin();
        REQUIRE(recording.HasValue());
        REQUIRE(otherRecording.HasValue());
        CHECK(recording.Value().value == otherRecording.Value().value);
        CHECK(recording.Value().deviceIncarnation != otherRecording.Value().deviceIncarnation);
        CHECK(commands.Close(otherRecording.Value()).HasError());
        CHECK(commands.Poll({0, recording.Value().value}).HasError());
        CHECK(BorrowD3D12CommandList(*second.resources, recording.Value()).HasError());
        std::string code;
        std::thread worker([&] {
            const auto rejected = commands.Begin();
            if (rejected.HasError())
                code = rejected.ErrorValue().code.Value();
        });
        worker.join();
        CHECK(code == "render.d3d12.wrong_thread");
        REQUIRE(commands.Cancel(recording.Value()).HasValue());
        REQUIRE(otherCommands.Cancel(otherRecording.Value()).HasValue());
    }

    TEST_CASE("D3D12 command device removal preserves completion ownership until native disposal", "[warp][d3d12][commands][device-loss]") {
        Fixture fixture({}, {.afterExecute = RemoveAfterExecute});
        auto &commands = Commands(fixture);
        CompletionLease lease;
        DrainGuard drain{fixture};
        const auto recording = Closed(commands);
        RenderGraphLeaseTransfer receipt;
        const auto failed = commands.Submit(recording, {{1}, {1}, {}, {{1}, 1}}, &lease, receipt);
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == "render.d3d12.device_lost");
        CHECK(receipt.authority == RenderGraphLeaseAuthority::BackendCompletion);
        CHECK(lease.releases == 0);
        CHECK(commands.Poll(recording).HasError());
        const auto foreign = commands.Poll({recording.deviceIncarnation + 1, recording.value});
        REQUIRE(foreign.HasError());
        CHECK(foreign.ErrorValue().code.Value() == "render.d3d12.command_invalid");
        fixture.resources.reset();
        CHECK(lease.releases == 1);
        CHECK(lease.releasedOn == std::this_thread::get_id());
    }

    TEST_CASE("D3D12 rejects a second completion owner for one outstanding frontend lease", "[warp][d3d12][commands][lease-authority]") {
        Fixture fixture;
        auto &commands = Commands(fixture);
        CompletionLease lease;
        DrainGuard drain{fixture};
        QueueGate gate{fixture};
        const auto first = Closed(commands);
        const auto second = Closed(commands);
        RenderGraphLeaseTransfer firstReceipt;
        RenderGraphLeaseTransfer secondReceipt;
        REQUIRE(commands.Submit(first, {{1}, {1}, {}, {{1}, 1}}, &lease, firstReceipt).HasValue());
        const auto duplicate = commands.Submit(second, {{1}, {2}, {}, {{1}, 2}}, &lease, secondReceipt);
        REQUIRE(duplicate.HasError());
        CHECK(duplicate.ErrorValue().code.Value() == "render.d3d12.command_lease_invalid");
        CHECK(secondReceipt.authority == RenderGraphLeaseAuthority::Frontend);
        CHECK(lease.releases == 0);
        gate.Open();
        Await(commands, first);
        CHECK(lease.releases == 1);
    }

    TEST_CASE("D3D12 failed completion signaling retains actual accepted commands until retry and drain",
              "[warp][d3d12][commands][signal-retry]") {
        Fixture fixture({}, {.commandSignalFailures = 2});
        auto &commands = Commands(fixture);
        CompletionLease lease;
        DrainGuard drain{fixture};
        QueueGate gate{fixture};
        const auto recording = Closed(commands);
        RenderGraphLeaseTransfer receipt;
        const auto submitted = commands.Submit(recording, {{1}, {1}, {}, {{1}, 1}}, &lease, receipt);
        REQUIRE(submitted.HasError());
        CHECK(submitted.ErrorValue().code.Value() == "render.d3d12.signal_failed");
        CHECK(receipt.authority == RenderGraphLeaseAuthority::BackendCompletion);
        CHECK(lease.releases == 0);
        const auto retried = commands.Poll(recording);
        REQUIRE(retried.HasError());
        CHECK(retried.ErrorValue().code.Value() == "render.d3d12.signal_failed");
        CHECK(lease.releases == 0);
        fixture.resources->StopAdmission();
        const auto pending = fixture.resources->PollDrain();
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value());
        CHECK(lease.releases == 0);
        gate.Open();
        AwaitDrain(fixture);
        CHECK(lease.releases == 1);
    }
}  // namespace
#endif
