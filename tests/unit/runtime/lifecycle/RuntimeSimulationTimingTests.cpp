#include "Horo/Runtime/RuntimeHost.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;

    static_assert(!std::is_default_constructible_v<RuntimeSimulationControl>);
    static_assert(!std::is_copy_constructible_v<RuntimeSimulationControl>);
    static_assert(!std::is_move_constructible_v<RuntimeSimulationControl>);
    static_assert(!std::is_default_constructible_v<RuntimeSimulationPolicyRead>);
    static_assert(!std::is_copy_constructible_v<RuntimeSimulationPauseLease>);
    static_assert(!std::is_copy_constructible_v<RuntimeSingleStepReceipt>);

    class TimingReader final : public RuntimeLifecycleParticipant {
    public:
        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const FixedStepContext &context) override {
            if (failFixed)
                return Result<void>::Failure(
                    {ErrorCode{"runtime.test.fixed"}, ErrorDomainId{"runtime.test"}, ErrorSeverity::Error, "Injected fixed failure.", {}});
            ++fixedCount;
            fixed = context.fixedDelta;
            attempt = context.attemptNumber;
            frame = context.frameNumber;
            return Result<void>::Success();
        }

        Result<void> OnPhase(const RuntimePhase phase, const FrameContext &context) override {
            if (phase == changeAt && changeRate) {
                auto read = control->ReadPolicy();
                if (read.HasError())
                    return Result<void>::Failure(read.ErrorValue());
                auto changed = control->SetRate({2, 1}, read.Value());
                if (changed.HasError())
                    return Result<void>::Failure(changed.ErrorValue());
                changeRate = false;
            }
            if (phase == RuntimePhase::VariableUpdate)
                observed = context.simulationPolicy;
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            ++shutdowns;
        }

        RuntimeSimulationControl *control{};
        RuntimePhase changeAt{RuntimePhase::NetworkPoll};
        RuntimeSimulationPolicy observed;
        Duration fixed{};
        std::uint64_t attempt{};
        std::uint64_t frame{};
        unsigned fixedCount{};
        unsigned shutdowns{};
        bool failFixed{};
        bool changeRate{};
    };
}  // namespace

TEST_CASE("Actual host rate changes retain exact fractional duration without scaling fixed quanta", "[runtime][timing]") {
    DeterministicClock clock;
    auto made = RuntimeHost::Create(clock, {.fixedStep = Duration::FromNanoseconds(2)});
    REQUIRE(made.HasValue());
    auto host = std::move(made).Value();
    auto participant = std::make_unique<TimingReader>();
    auto *reader = participant.get();
    REQUIRE(host->AddParticipant(std::move(participant)).HasValue());
    REQUIRE(host->Startup().HasValue());
    auto &control = host->SimulationControl();
    auto first = control.ReadPolicy();
    REQUIRE(first.HasValue());
    REQUIRE(control.SetRate({1, 2}, first.Value()).HasValue());
    REQUIRE(host->RunFrame().HasValue());
    clock.Advance(Duration::FromNanoseconds(1));
    REQUIRE(host->RunFrame().HasValue());
    REQUIRE(control.Policy().remainder == RuntimeSimulationRemainder{1, 2});
    auto second = control.ReadPolicy();
    REQUIRE(second.HasValue());
    REQUIRE(control.SetRate({1, 3}, second.Value()).HasValue());
    REQUIRE(control.SetRate({1, 1}, first.Value()).HasError());
    clock.Advance(Duration::FromNanoseconds(1));
    REQUIRE(host->RunFrame().HasValue());
    REQUIRE(control.Policy().remainder == RuntimeSimulationRemainder{5, 6});
    auto third = control.ReadPolicy();
    REQUIRE(third.HasValue());
    REQUIRE(control.SetRate({1, 1}, third.Value()).HasValue());
    clock.Advance(Duration::FromNanoseconds(2));
    REQUIRE(host->RunFrame().HasValue());
    REQUIRE(reader->fixedCount == 1);
    REQUIRE(reader->fixed == Duration::FromNanoseconds(2));
    REQUIRE(control.Policy().remainder == RuntimeSimulationRemainder{5, 6});
}

TEST_CASE("Actual host composed pause leases and successful steps retain independent authority", "[runtime][timing]") {
    DeterministicClock clock;
    auto made = RuntimeHost::Create(clock, {.fixedStep = Duration::FromMilliseconds(10)});
    REQUIRE(made.HasValue());
    auto host = std::move(made).Value();
    auto &control = host->SimulationControl();
    auto read = control.ReadPolicy();
    REQUIRE(read.HasValue());
    auto gameplay = control.AcquirePause(RuntimeSimulationPauseReason::Gameplay, read.Value());
    REQUIRE(gameplay.HasValue());
    auto gameplayLease = std::move(gameplay).Value();
    read = control.ReadPolicy();
    REQUIRE(read.HasValue());
    auto cinematic = control.AcquirePause(RuntimeSimulationPauseReason::Cinematic, read.Value());
    REQUIRE(cinematic.HasValue());
    auto cinematicLease = std::move(cinematic).Value();
    read = control.ReadPolicy();
    REQUIRE(read.HasValue());
    auto step = control.RequestStep(read.Value());
    REQUIRE(step.HasValue());
    REQUIRE(host->Startup().HasValue());
    REQUIRE(host->RunFrame().HasValue());
    auto result = step.Value().ResultValue();
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().state == RuntimeSingleStepState::Committed);
    REQUIRE(result.Value().simulationTick == 1);
    REQUIRE(result.Value().attemptNumber == 1);
    REQUIRE(result.Value().frameNumber == 1);
    REQUIRE(result.Value().duration == Duration::FromMilliseconds(10));
    gameplayLease.Release();
    clock.Advance(Duration::FromMilliseconds(50));
    REQUIRE(host->RunFrame().HasValue());
    REQUIRE(control.Policy().pauseCount == 1);
    REQUIRE(host->Statistics().completedSimulationTick == 1);
    cinematicLease.Release();
    REQUIRE(host->RunFrame().HasValue());
    REQUIRE_FALSE(control.Policy().paused);
    clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(host->RunFrame().HasValue());
    REQUIRE(host->Statistics().completedSimulationTick == 2);
}

TEST_CASE("Actual host fatal fixed failure cancels step receipts without claiming commitment", "[runtime][timing]") {
    DeterministicClock clock;
    auto made = RuntimeHost::Create(clock);
    REQUIRE(made.HasValue());
    auto host = std::move(made).Value();
    auto participant = std::make_unique<TimingReader>();
    auto *reader = participant.get();
    reader->failFixed = true;
    REQUIRE(host->AddParticipant(std::move(participant)).HasValue());
    auto &control = host->SimulationControl();
    auto read = control.ReadPolicy();
    REQUIRE(read.HasValue());
    auto pause = control.AcquirePause(RuntimeSimulationPauseReason::Debugger, read.Value());
    REQUIRE(pause.HasValue());
    auto pauseLease = std::move(pause).Value();
    read = control.ReadPolicy();
    REQUIRE(read.HasValue());
    auto receipt = control.RequestStep(read.Value());
    REQUIRE(receipt.HasValue());
    REQUIRE(host->Startup().HasValue());
    REQUIRE(host->RunFrame().HasError());
    REQUIRE(host->Statistics().completedSimulationTick == 0);
    REQUIRE(reader->shutdowns == 1);
    REQUIRE(control.ReadPolicy().HasError());
    auto terminal = receipt.Value().ResultValue();
    REQUIRE(terminal.HasValue());
    REQUIRE(terminal.Value().state == RuntimeSingleStepState::Cancelled);
    REQUIRE(terminal.Value().cancellation == RuntimeSingleStepCancellation::HostRetired);
    REQUIRE(terminal.Value().simulationTick == 0);
    REQUIRE(terminal.Value().duration == Duration{});
    host.reset();
    REQUIRE(receipt.Value().ResultValue().HasValue());
    pauseLease.Release();
}

TEST_CASE("Actual owner command cutoff defers late timing commands to the following frame", "[runtime][timing]") {
    DeterministicClock clock;
    auto made = RuntimeHost::Create(clock, {.fixedStep = Duration::FromMilliseconds(10)});
    REQUIRE(made.HasValue());
    auto host = std::move(made).Value();
    auto participant = std::make_unique<TimingReader>();
    auto *reader = participant.get();
    reader->control = &host->SimulationControl();
    REQUIRE(host->AddParticipant(std::move(participant)).HasValue());
    REQUIRE(host->Startup().HasValue());
    REQUIRE(host->RunFrame().HasValue());
    reader->changeRate = true;
    SECTION("command at the owner cutoff") {
        reader->changeAt = RuntimePhase::ApplyQueuedOwnerThreadCommands;
    }
    SECTION("command after the owner cutoff") {
        reader->changeAt = RuntimePhase::NetworkPoll;
    }
    clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(host->RunFrame().HasValue());
    const bool early = reader->changeAt == RuntimePhase::ApplyQueuedOwnerThreadCommands;
    REQUIRE(reader->fixedCount == (early ? 2 : 1));
    REQUIRE(reader->observed.rate == RuntimeSimulationRate{early ? 2U : 1U, 1});
    clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(host->RunFrame().HasValue());
    REQUIRE(reader->fixedCount == (early ? 4 : 3));
}

TEST_CASE("Actual scheduler retains a failed step attempt and commits only its successful same-tick retry", "[runtime][timing]") {
    DeterministicClock clock;
    RuntimeLifecycle lifecycle;
    CancellationSource cancellation;
    auto made = FrameScheduler::Create(clock);
    REQUIRE(made.HasValue());
    auto scheduler = std::move(made).Value();
    auto participant = std::make_unique<TimingReader>();
    auto *reader = participant.get();
    reader->failFixed = true;
    REQUIRE(lifecycle.AddParticipant(std::move(participant)).HasValue());
    REQUIRE(lifecycle.Startup(cancellation.Token()).HasValue());
    auto &control = scheduler->SimulationControl();
    auto read = control.ReadPolicy();
    REQUIRE(read.HasValue());
    auto pause = control.AcquirePause(RuntimeSimulationPauseReason::Debugger, read.Value());
    REQUIRE(pause.HasValue());
    auto pauseLease = std::move(pause).Value();
    read = control.ReadPolicy();
    REQUIRE(read.HasValue());
    auto receipt = control.RequestStep(read.Value());
    REQUIRE(receipt.HasValue());
    REQUIRE(scheduler->RunFrame(lifecycle, cancellation.Token(), false).HasError());
    REQUIRE(receipt.Value().ResultValue().Value().state == RuntimeSingleStepState::Pending);
    REQUIRE(scheduler->Statistics().completedSimulationTick == 0);
    reader->failFixed = false;
    clock.Advance(Duration::FromMilliseconds(50));
    REQUIRE(scheduler->RunFrame(lifecycle, cancellation.Token(), false).HasValue());
    const auto result = receipt.Value().ResultValue();
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().simulationTick == 1);
    REQUIRE(result.Value().attemptNumber == 2);
    REQUIRE(result.Value().frameNumber == 2);
    REQUIRE(result.Value().duration == Duration::FromNanoseconds(16'666'667));
    REQUIRE(reader->fixedCount == 1);
}

TEST_CASE("Actual host suspension retains queued steps and composed pause release cancels stale work", "[runtime][timing]") {
    DeterministicClock clock;
    auto made = RuntimeHost::Create(clock);
    REQUIRE(made.HasValue());
    auto host = std::move(made).Value();
    auto &control = host->SimulationControl();
    auto read = control.ReadPolicy();
    REQUIRE(read.HasValue());
    auto pause = control.AcquirePause(RuntimeSimulationPauseReason::Menu, read.Value());
    REQUIRE(pause.HasValue());
    auto pauseLease = std::move(pause).Value();
    read = control.ReadPolicy();
    REQUIRE(read.HasValue());
    auto receipt = control.RequestStep(read.Value());
    REQUIRE(receipt.HasValue());
    REQUIRE(host->Startup().HasValue());
    REQUIRE(host->Suspend().HasValue());
    clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(host->RunFrame().HasValue());
    REQUIRE(receipt.Value().ResultValue().Value().state == RuntimeSingleStepState::Pending);
    REQUIRE(host->Statistics().completedSimulationTick == 0);
    SECTION("resume commits the retained request") {
        REQUIRE(host->Resume().HasValue());
        REQUIRE(host->RunFrame().HasValue());
        REQUIRE(receipt.Value().ResultValue().Value().state == RuntimeSingleStepState::Committed);
        REQUIRE(host->Statistics().completedSimulationTick == 1);
    }
    SECTION("release invalidates old pause-bound request") {
        pauseLease.Release();
        REQUIRE(host->RunFrame().HasValue());
        const auto result = receipt.Value().ResultValue();
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().state == RuntimeSingleStepState::Cancelled);
        REQUIRE(result.Value().cancellation == RuntimeSingleStepCancellation::PauseChanged);
        REQUIRE(result.Value().duration == Duration{});
    }
}

TEST_CASE("Actual host rejects fractional denominator exhaustion without losing the admitted remainder", "[runtime][timing]") {
    DeterministicClock clock;
    auto made = RuntimeHost::Create(clock);
    REQUIRE(made.HasValue());
    auto host = std::move(made).Value();
    auto &control = host->SimulationControl();
    auto read = control.ReadPolicy();
    REQUIRE(read.HasValue());
    REQUIRE(control.SetRate({1, 4'294'967'291U}, read.Value()).HasValue());
    REQUIRE(host->Startup().HasValue());
    REQUIRE(host->RunFrame().HasValue());
    clock.Advance(Duration::FromNanoseconds(1));
    REQUIRE(host->RunFrame().HasValue());
    REQUIRE(control.Policy().remainder == RuntimeSimulationRemainder{1, 4'294'967'291U});
    read = control.ReadPolicy();
    REQUIRE(read.HasValue());
    REQUIRE(control.SetRate({1, 4'294'967'279U}, read.Value()).HasValue());
    clock.Advance(Duration::FromNanoseconds(1));
    const auto failed = host->RunFrame();
    REQUIRE(failed.HasError());
    REQUIRE(failed.ErrorValue().code.Value() == "runtime.simulation_timing.overflow");
    REQUIRE(control.Policy().remainder == RuntimeSimulationRemainder{1, 4'294'967'291U});
    REQUIRE(host->Statistics().completedSimulationTick == 0);
}
