#include "AllocationProbe.h"
#include "FixedAttemptEvidence.h"
#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/Runtime/RuntimeLifecycle.h"
#include "PresentationClockGeneration.h"
#include "RuntimeLifecycleTestFixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Runtime;

    using namespace Horo::Runtime::LifecycleTests;

    TEST_CASE("Scheduler success fence distinguishes a failed tick from its retry", "[unit][runtime][clock]") {
        DeterministicClock clock;
        RuntimeLifecycle lifecycle;
        CancellationSource cancellation;
        auto created = FrameScheduler::Create(clock, {.fixedStep = Duration::FromMilliseconds(10)});
        Check(created.HasValue());
        auto scheduler = std::move(created).Value();
        auto observer = std::make_unique<RecordingParticipant>();
        auto *observed = observer.get();
        auto failing = std::make_unique<RecordingParticipant>();
        auto *failure = failing.get();
        failure->throwFixedTick = 1;
        bool failBeforeObserver = false;
        SECTION("failure after observation") {}
        SECTION("failure before observation") {
            failBeforeObserver = true;
        }
        if (failBeforeObserver)
            Check(lifecycle.AddParticipant(std::move(failing)).HasValue());
        Check(lifecycle.AddParticipant(std::move(observer)).HasValue());
        if (!failBeforeObserver)
            Check(lifecycle.AddParticipant(std::move(failing)).HasValue());
        Check(lifecycle.Startup(cancellation.Token()).HasValue());
        Check(scheduler->RunFrame(lifecycle, cancellation.Token(), false).HasValue());
        clock.Advance(Duration::FromMilliseconds(10));
        Check(scheduler->RunFrame(lifecycle, cancellation.Token(), false).HasError());
        Check(scheduler->Statistics().completedSimulationTick == 0);
        Check(observed->lastCommittedFixedStep.simulationTick == 0);
        Check(observed->fixedEvidence.size() == (failBeforeObserver ? 0U : 1U));
        Check(lifecycle.State() == RuntimeLifecycleState::Running);
        failure->throwFixedTick = 0;
        Check(scheduler->RunFrame(lifecycle, cancellation.Token(), false).HasValue());
        const auto &committed = observed->lastCommittedFixedStep;
        Check(committed.simulationTick == 1);
        Check(committed.attemptNumber == 2);
        Check(committed.frameNumber == 3);
        Check(committed.duration == Duration::FromMilliseconds(10));
        Check(observed->fixedEvidence.back().attemptNumber == committed.attemptNumber);
        Check(observed->fixedEvidence.back().duration == committed.duration);
        Check(observed->lastCompletedSimulationTick == 1);
    }

    TEST_CASE("Scheduler retains earlier successful catch-up ticks across a later failed attempt", "[unit][runtime][clock]") {
        DeterministicClock clock;
        RuntimeLifecycle lifecycle;
        CancellationSource cancellation;
        auto created = FrameScheduler::Create(clock, {.fixedStep = Duration::FromMilliseconds(10)});
        Check(created.HasValue());
        auto scheduler = std::move(created).Value();
        auto observer = std::make_unique<RecordingParticipant>();
        auto *observed = observer.get();
        auto failing = std::make_unique<RecordingParticipant>();
        auto *failure = failing.get();
        failure->failFixedTick = 3;
        Check(lifecycle.AddParticipant(std::move(observer)).HasValue());
        Check(lifecycle.AddParticipant(std::move(failing)).HasValue());
        Check(lifecycle.Startup(cancellation.Token()).HasValue());
        Check(scheduler->RunFrame(lifecycle, cancellation.Token(), false).HasValue());
        clock.Advance(Duration::FromMilliseconds(30));
        Check(scheduler->RunFrame(lifecycle, cancellation.Token(), false).HasError());
        Check(scheduler->Statistics().completedSimulationTick == 2);
        Check(observed->variableUpdateCount == 1);
        Check(observed->fixedTicks == std::vector<std::uint64_t>{1, 2, 3});
        Check(lifecycle.State() == RuntimeLifecycleState::Running);
        failure->failFixedTick = 0;
        Check(scheduler->RunFrame(lifecycle, cancellation.Token(), false).HasValue());
        Check(scheduler->Statistics().completedSimulationTick == 3);
        Check(observed->fixedTicks == std::vector<std::uint64_t>{1, 2, 3, 3});
        Check(observed->lastCommittedFixedStep.simulationTick == 3);
        Check(observed->lastCommittedFixedStep.attemptNumber == 4);
        Check(observed->lastCommittedFixedStep.frameNumber == 3);
        Check(observed->lastCommittedFixedStep.duration == Duration::FromMilliseconds(10));
    }

    TEST_CASE("Runtime host failure retires attempted fixed evidence without retry", "[unit][runtime][clock]") {
        DeterministicClock clock;
        auto host = MakeHost(clock, {.fixedStep = Duration::FromMilliseconds(10)});
        auto participant = std::make_unique<RecordingParticipant>();
        auto *observed = participant.get();
        observed->failFixedTick = 3;
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        clock.Advance(Duration::FromMilliseconds(30));
        const auto failed = host->RunFrame();
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "runtime.test.fixed");
        Check(host->Statistics().completedSimulationTick == 2);
        Check(host->State() == RuntimeLifecycleState::Stopped);
        Check(observed->shutdownCount == 1);
        Check(observed->fixedTicks == std::vector<std::uint64_t>{1, 2, 3});
        Check(observed->variableUpdateCount == 1);
        Check(observed->lastCommittedFixedStep.simulationTick == 0);
        Check(host->RunFrame().HasError());
        Check(host->Statistics().completedSimulationTick == 2);
        Check(observed->fixedTicks == std::vector<std::uint64_t>{1, 2, 3});
        Check(observed->shutdownCount == 1);
    }

    TEST_CASE("Cancellation after fixed dispatch never publishes its attempted success fence", "[unit][runtime][clock]") {
        DeterministicClock clock;
        auto host = MakeHost(clock, {.fixedStep = Duration::FromMilliseconds(10)});
        auto participant = std::make_unique<RecordingParticipant>();
        auto *observed = participant.get();
        observed->host = host.get();
        observed->cancelFixedTick = 1;
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        clock.Advance(Duration::FromMilliseconds(10));
        Check(host->RunFrame().HasError());
        Check(host->Statistics().completedSimulationTick == 0);
        Check(observed->fixedEvidence.size() == 1);
        Check(observed->fixedEvidence.front().attemptNumber == 1);
        Check(observed->lastCommittedFixedStep.simulationTick == 0);
        Check(observed->variableUpdateCount == 1);
    }

    TEST_CASE("Actual fixed attempt reservation exhausts without wrapping or changing its last identity", "[unit][runtime][clock]") {
        auto attempt = std::numeric_limits<std::uint64_t>::max() - 1;
        const auto last = Internal::ReserveFixedAttempt(attempt);
        REQUIRE(last.HasValue());
        REQUIRE(last.Value() == std::numeric_limits<std::uint64_t>::max());
        const auto exhausted = Internal::ReserveFixedAttempt(attempt);
        REQUIRE(exhausted.HasError());
        REQUIRE(exhausted.ErrorValue().code.Value() == RuntimeErrors::FixedAttemptIdentityExhausted.code.Value());
        REQUIRE(attempt == std::numeric_limits<std::uint64_t>::max());
        REQUIRE(Internal::ReserveFixedAttempt(attempt).HasError());
    }

    TEST_CASE("Actual presentation frame reservation rejects wrap before sampling or dispatch", "[unit][runtime][clock]") {
        auto frame = std::numeric_limits<std::uint64_t>::max() - 1;
        const auto last = Internal::ReservePresentationFrame(frame);
        REQUIRE(last.HasValue());
        REQUIRE(last.Value() == std::numeric_limits<std::uint64_t>::max());
        const auto exhausted = Internal::ReservePresentationFrame(frame);
        REQUIRE(exhausted.HasError());
        REQUIRE(exhausted.ErrorValue().code.Value() == RuntimeErrors::FrameIdentityExhausted.code.Value());
        REQUIRE(frame == std::numeric_limits<std::uint64_t>::max());
        REQUIRE(Internal::ReservePresentationFrame(frame).HasError());
    }

    TEST_CASE("Phase Order And Tick Contexts Are Canonical", "[unit][runtime]") {
        DeterministicClock clock;
        std::vector<RuntimePhase> phases;
        auto participant = std::make_unique<RecordingParticipant>(&phases);
        RecordingParticipant *observed = participant.get();
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        clock.Advance(Duration::FromNanoseconds(16'666'667));
        Check(host->RunFrame().HasValue());

        constexpr std::array expected{RuntimePhase::BeginFrame,
                                      RuntimePhase::PollPlatformEvents,
                                      RuntimePhase::BuildInputSnapshot,
                                      RuntimePhase::ApplyQueuedOwnerThreadCommands,
                                      RuntimePhase::NetworkPoll,
                                      RuntimePhase::NetworkFlush,
                                      RuntimePhase::VariableUpdate,
                                      RuntimePhase::RenderExtraction,
                                      RuntimePhase::RenderExecution,
                                      RuntimePhase::RenderGui,
                                      RuntimePhase::Presentation,
                                      RuntimePhase::CommitDeferredLifecycleChanges,
                                      RuntimePhase::EndFrame};
        Check(phases.size() == expected.size() * 2);
        for (std::size_t index = 0; index < expected.size(); ++index) {
            Check(phases[index] == expected[index]);
            Check(phases[index + expected.size()] == expected[index]);
        }
        Check(observed->fixedTicks == std::vector<std::uint64_t>{1});
        Check(observed->lastCompletedSimulationTick == 1);
        Check(observed->lastInterpolationAlpha == 0.0);
    }

    TEST_CASE("Catch Up Is Bounded And Observable", "[unit][runtime]") {
        DeterministicClock clock;
        auto participant = std::make_unique<RecordingParticipant>();
        RecordingParticipant *observed = participant.get();
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        clock.Advance(Duration::FromMilliseconds(250));
        Check(host->RunFrame().HasValue());
        Check(observed->fixedTicks.size() == 5);
        Check(observed->lastCompletedSimulationTick == 5);
        Check(observed->lastInterpolationAlpha >= 0.0 && observed->lastInterpolationAlpha < 1.0);
        Check(observed->lastRealDeltaWasClamped == false);
        const FrameSchedulerStatistics statistics = host->Statistics();
        Check(statistics.catchUpLimitedFrameCount == 1);
        Check(statistics.totalDroppedFixedSteps == 9);
        Check(statistics.totalDroppedSimulationTime.ToNanoseconds() == 9 * 16'666'667LL);

        clock.Advance(Duration::FromMilliseconds(500));
        Check(host->RunFrame().HasValue());
        Check(host->Statistics().maximumDeltaClampCount == 1);
    }

    TEST_CASE("Negative Delta Is Normalized", "[unit][runtime]") {
        DeterministicClock clock(Duration::FromMilliseconds(100));
        auto participant = std::make_unique<RecordingParticipant>();
        RecordingParticipant *observed = participant.get();
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        clock.Advance(Duration::FromMilliseconds(-10));
        Check(host->RunFrame().HasValue());
        Check(observed->lastVariableDelta == Duration{});
        Check(host->Statistics().negativeDeltaNormalizationCount == 1);
    }

    TEST_CASE("Startup Failure Rolls Back And Shutdown Is Reverse Ordered", "[unit][runtime]") {
        DeterministicClock clock;
        std::vector<int> order;
        auto host = MakeHost(clock);
        auto first = std::make_unique<RecordingParticipant>();
        first->id = 1;
        first->shutdownOrder = &order;
        auto second = std::make_unique<RecordingParticipant>();
        RecordingParticipant *failed = second.get();
        second->id = 2;
        second->shutdownOrder = &order;
        second->failStartup = true;
        auto third = std::make_unique<RecordingParticipant>();
        RecordingParticipant *skipped = third.get();
        third->id = 3;
        third->shutdownOrder = &order;
        Check(host->AddParticipant(std::move(first)).HasValue());
        Check(host->AddParticipant(std::move(second)).HasValue());
        Check(host->AddParticipant(std::move(third)).HasValue());
        Check(host->Startup().HasError());
        Check(order == std::vector<int>{1});
        Check(failed->shutdownCount == 0);
        Check(skipped->startupCount == 0);

        order.clear();
        auto successful = MakeHost(clock);
        for (int id = 1; id <= 3; ++id) {
            auto service = std::make_unique<RecordingParticipant>();
            service->id = id;
            service->shutdownOrder = &order;
            Check(successful->AddParticipant(std::move(service)).HasValue());
        }
        Check(successful->Startup().HasValue());
        successful->Shutdown();
        successful->Shutdown();
        Check(order == std::vector<int>({3, 2, 1}));
    }

    TEST_CASE("Failures And Exceptions Gate Presentation", "[unit][runtime]") {
        constexpr std::array gatedPhases{RuntimePhase::RenderExtraction, RuntimePhase::RenderExecution, RuntimePhase::RenderGui};
        for (const RuntimePhase failedPhase : gatedPhases) {
            DeterministicClock clock;
            std::vector<RuntimePhase> phases;
            auto participant = std::make_unique<RecordingParticipant>(&phases);
            participant->failPhase = failedPhase;
            auto host = MakeHost(clock);
            Check(host->AddParticipant(std::move(participant)).HasValue());
            Check(host->Startup().HasValue());
            const Result<void> failed = host->RunFrame();
            Check(failed.HasError());
            Check(failed.ErrorValue().code.Value() == "runtime.test.failure");
            Check(std::find(phases.begin(), phases.end(), RuntimePhase::Presentation) == phases.end());
            Check(std::find(phases.begin(), phases.end(), RuntimePhase::EndFrame) == phases.end());
            Check(host->State() == RuntimeLifecycleState::Stopped);
        }

        DeterministicClock clock;
        auto participant = std::make_unique<RecordingParticipant>();
        participant->throwPhase = RuntimePhase::VariableUpdate;
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        const Result<void> failed = host->RunFrame();
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "runtime.lifecycle.unexpected_exception");
    }

    TEST_CASE("Failed Fixed Tick Does Not Commit", "[unit][runtime]") {
        DeterministicClock clock;
        auto participant = std::make_unique<RecordingParticipant>();
        participant->failFixedTick = 1;
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        clock.Advance(Duration::FromNanoseconds(16'666'667));
        Check(host->RunFrame().HasError());
        Check(host->Statistics().completedSimulationTick == 0);
        Check(host->Statistics().totalDroppedFixedSteps == 0);
    }

    TEST_CASE("Suspend Pumps Only Safe Phases And Resume Drops Wall Time", "[unit][runtime]") {
        DeterministicClock clock;
        std::vector<RuntimePhase> phases;
        auto participant = std::make_unique<RecordingParticipant>(&phases);
        RecordingParticipant *observed = participant.get();
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        phases.clear();
        Check(host->Suspend().HasValue());
        clock.Advance(Duration::FromMilliseconds(500));
        Check(host->RunFrame().HasValue());
        constexpr std::array suspended{RuntimePhase::BeginFrame, RuntimePhase::PollPlatformEvents,
                                       RuntimePhase::ApplyQueuedOwnerThreadCommands, RuntimePhase::NetworkPoll, RuntimePhase::EndFrame};
        Check(phases.size() == suspended.size());
        for (std::size_t index = 0; index < suspended.size(); ++index)
            Check(phases[index] == suspended[index]);
        Check(host->Resume().HasValue());
        phases.clear();
        Check(host->RunFrame().HasValue());
        Check(observed->fixedTicks.empty());
    }

    TEST_CASE("Presentation baseline reset is explicit and preserves simulation commitment", "[unit][runtime]") {
        DeterministicClock clock;
        auto participant = std::make_unique<RecordingParticipant>();
        auto *observed = participant.get();
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        Check(observed->presentationGeneration == 1);
        Check(observed->presentationContinuity == PresentationClockContinuity::Initial);
        clock.Advance(Duration::FromNanoseconds(16'666'667));
        Check(host->RunFrame().HasValue());
        Check(observed->presentationContinuity == PresentationClockContinuity::Continuous);
        Check(host->Statistics().completedSimulationTick == 1);
        Check(host->Suspend().HasValue());
        clock.Advance(Duration::FromMilliseconds(20'000));
        Check(host->RunFrame().HasValue());
        Check(observed->variableUpdateCount == 2);
        Check(host->Resume().HasValue());
        Check(host->RunFrame().HasValue());
        Check(observed->presentationGeneration == 3);
        Check(observed->presentationContinuity == PresentationClockContinuity::BaselineReset);
        Check(observed->lastVariableDelta == Duration{});
        Check(host->Statistics().completedSimulationTick == 1);
        Check(host->RunFrame().HasValue());
        Check(observed->presentationContinuity == PresentationClockContinuity::Continuous);
        Check(observed->presentationGeneration == 3);
    }

    TEST_CASE("Actual checked presentation baseline operations latch exhaustion before dispatch", "[unit][runtime]") {
        std::uint64_t generation = std::numeric_limits<std::uint64_t>::max() - 1;
        bool exhausted = false;
        Internal::AdvancePresentationBaseline(generation, exhausted);
        Check(generation == std::numeric_limits<std::uint64_t>::max());
        Check(Internal::AdmitPresentationBaseline(exhausted).HasValue());
        Internal::AdvancePresentationBaseline(generation, exhausted);
        Check(generation == std::numeric_limits<std::uint64_t>::max());
        const auto failed = Internal::AdmitPresentationBaseline(exhausted);
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "runtime.scheduler.presentation_clock_generation_exhausted");
        Internal::AdvancePresentationBaseline(generation, exhausted);
        Check(generation == std::numeric_limits<std::uint64_t>::max());
        Check(Internal::AdmitPresentationBaseline(exhausted).HasError());
    }

    TEST_CASE("Mid Frame Cancellation Stops Later Phases", "[unit][runtime]") {
        DeterministicClock clock;
        std::vector<RuntimePhase> phases;
        auto host = MakeHost(clock);
        auto participant = std::make_unique<RecordingParticipant>(&phases);
        participant->host = host.get();
        participant->cancelPhase = RuntimePhase::VariableUpdate;
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        const Result<void> cancelled = host->RunFrame();
        Check(cancelled.HasError());
        Check(cancelled.ErrorValue().code.Value() == "runtime.host.cancelled");
        Check(std::find(phases.begin(), phases.end(), RuntimePhase::RenderExtraction) == phases.end());
        Check(host->State() == RuntimeLifecycleState::Stopped);
    }

    std::uint64_t RunDeterministicCadence(const int frameCount) {
        DeterministicClock clock;
        auto participant = std::make_unique<RecordingParticipant>();
        RecordingParticipant *observed = participant.get();
        auto host = MakeHost(clock, FrameSchedulerConfig{.fixedStep = Duration::FromMilliseconds(10),
                                                         .maximumFrameDelta = Duration::FromMilliseconds(250),
                                                         .maximumCatchUpSteps = 25});
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        for (int frame = 0; frame < frameCount; ++frame) {
            const std::int64_t baseDelta = 1'000'000'000LL / frameCount;
            const std::int64_t remainder = frame == frameCount - 1 ? 1'000'000'000LL % frameCount : 0;
            clock.Advance(Duration::FromNanoseconds(baseDelta + remainder));
            Check(host->RunFrame().HasValue());
        }
        return observed->lastCompletedSimulationTick;
    }

    TEST_CASE("Fixed Simulation Is Independent Of Presentation Cadence", "[unit][runtime]") {
        const std::uint64_t thirtyHz = RunDeterministicCadence(30);
        const std::uint64_t sixtyHz = RunDeterministicCadence(60);
        const std::uint64_t highHz = RunDeterministicCadence(144);
        Check(thirtyHz == 100);
        Check(sixtyHz == thirtyHz);
        Check(highHz == thirtyHz);
    }

    TEST_CASE("Headless Null Renderer Uses Canonical Contract", "[unit][runtime]") {
        DeterministicClock clock;
        auto participant = std::make_unique<NullRenderParticipant>();
        NullRenderParticipant *observed = participant.get();
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        Check(observed->presentCount == 1);
    }

    TEST_CASE("Successful Steady State Scheduler Does Not Allocate", "[unit][runtime]") {
        DeterministicClock clock;
        auto participant = std::make_unique<AllocationFreeParticipant>();
        auto host = MakeHost(clock);
        Check(host->AddParticipant(std::move(participant)).HasValue());
        Check(host->Startup().HasValue());
        Check(host->RunFrame().HasValue());
        clock.Advance(Duration::FromNanoseconds(16'666'667));
        const auto allocations = Horo::Tests::AllocationProbe::Count();
        const auto frame = host->RunFrame();
        const auto after = Horo::Tests::AllocationProbe::Count();
        Check(frame.HasValue());
        Check(after == allocations);
    }
}  // namespace
