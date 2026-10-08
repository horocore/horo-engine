#include "AllocationProbe.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "RuntimeDispatchOrdinal.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;

    class DispatchReader final : public RuntimeLifecycleParticipant {
    public:
        explicit DispatchReader(RuntimeDispatchSource expected) : expected(std::move(expected)) {}

        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {}

        Result<void> OnFixedUpdate(const FixedStepContext &context) override {
            fixedEvidence = context.dispatchEvidence;
            fixedStatus = fixedEvidence.Read(expected, RuntimePhase::FixedUpdate, fixedFacts);
            return Result<void>::Success();
        }

        Result<void> OnPhase(const RuntimePhase phase, const FrameContext &context) override {
            if (phase == RuntimePhase::VariableUpdate)
                ObserveVariable(context);
            if (phase == RuntimePhase::RenderExtraction) {
                ++extractions;
                extractionStatus = context.dispatchEvidence.Read(expected, phase, extractionFacts);
                RuntimeDispatchFacts ignored;
                replayStatus = variableEvidence.Read(expected, RuntimePhase::VariableUpdate, ignored);
            }
            return Result<void>::Success();
        }

        void ObserveVariable(const FrameContext &context) {
            variableEvidence = context.dispatchEvidence;
            variableStatus = variableEvidence.Read(expected, RuntimePhase::VariableUpdate, variableFacts);
            auto fabricated = context;
            fabricated.frameNumber = 999;
            fabricated.completedSimulationTick = 999;
            forgedStatus = fabricated.dispatchEvidence.Read(expected, RuntimePhase::VariableUpdate, forgedFacts);
            wrongPhaseStatus = variableEvidence.Read(expected, RuntimePhase::FixedUpdate, untouchedFacts);
            foreignStatus = variableEvidence.Read(foreign, RuntimePhase::VariableUpdate, untouchedFacts);
            if (crossThread) {
                std::thread reader([this] {
                    crossThreadStatus = variableEvidence.Read(expected, RuntimePhase::VariableUpdate, untouchedFacts);
                });
                reader.join();
            }
            if (reenter)
                reentrantError = scheduler->RunFrame(*lifecycle, context.cancellation, false).HasError();
            if (cancel)
                cancel->RequestCancellation();
            if (throwVariable)
                throw std::runtime_error("Injected dispatch failure.");
        }

        RuntimeDispatchSource expected;
        RuntimeDispatchSource foreign;
        RuntimeDispatchEvidence variableEvidence;
        RuntimeDispatchEvidence fixedEvidence;
        RuntimeDispatchFacts variableFacts;
        RuntimeDispatchFacts fixedFacts;
        RuntimeDispatchFacts forgedFacts;
        RuntimeDispatchFacts extractionFacts;
        RuntimeDispatchFacts untouchedFacts{.frame = 77};
        RuntimeDispatchStatus variableStatus{};
        RuntimeDispatchStatus fixedStatus{};
        RuntimeDispatchStatus forgedStatus{};
        RuntimeDispatchStatus wrongPhaseStatus{};
        RuntimeDispatchStatus foreignStatus{};
        RuntimeDispatchStatus crossThreadStatus{};
        RuntimeDispatchStatus extractionStatus{};
        RuntimeDispatchStatus replayStatus{};
        FrameScheduler *scheduler{};
        RuntimeLifecycle *lifecycle{};
        CancellationSource *cancel{};
        unsigned extractions{};
        bool crossThread{};
        bool reenter{};
        bool reentrantError{};
        bool throwVariable{};
    };

    struct DispatchFixture final {
        DispatchFixture() {
            auto made = FrameScheduler::Create(clock, {.fixedStep = Duration::FromMilliseconds(10)});
            REQUIRE(made.HasValue());
            scheduler = std::move(made).Value();
            auto participant = std::make_unique<DispatchReader>(scheduler->DispatchSource());
            reader = participant.get();
            reader->scheduler = scheduler.get();
            reader->lifecycle = &lifecycle;
            REQUIRE(lifecycle.AddParticipant(std::move(participant)).HasValue());
            REQUIRE(lifecycle.Startup(token).HasValue());
        }

        DeterministicClock clock;
        CancellationSource cancellation;
        CancellationToken token{cancellation.Token()};
        std::unique_ptr<FrameScheduler> scheduler;
        RuntimeLifecycle lifecycle;
        DispatchReader *reader{};
    };
}  // namespace

TEST_CASE("Actual dispatch evidence rejects forged observations foreign issuers phases and stale copies", "[runtime][dispatch]") {
    DispatchFixture fixture;
    DeterministicClock otherClock;
    auto other = FrameScheduler::Create(otherClock);
    REQUIRE(other.HasValue());
    fixture.reader->foreign = other.Value()->DispatchSource();
    REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
    auto &reader = *fixture.reader;
    REQUIRE(reader.variableStatus == RuntimeDispatchStatus::Valid);
    REQUIRE(reader.forgedStatus == RuntimeDispatchStatus::Valid);
    REQUIRE(reader.forgedFacts.frame == 1);
    REQUIRE(reader.forgedFacts.committedTick == 0);
    REQUIRE(reader.wrongPhaseStatus == RuntimeDispatchStatus::WrongPhase);
    REQUIRE(reader.foreignStatus == RuntimeDispatchStatus::ForeignSource);
    REQUIRE(reader.untouchedFacts.frame == 77);
    REQUIRE(reader.extractionStatus == RuntimeDispatchStatus::Valid);
    REQUIRE(reader.extractionFacts.completedVariableUpdateFrame == 1);
    REQUIRE(reader.replayStatus == RuntimeDispatchStatus::Stale);
    RuntimeDispatchFacts output{.frame = 99};
    REQUIRE(reader.variableEvidence.Read(reader.expected, RuntimePhase::VariableUpdate, output) == RuntimeDispatchStatus::Stale);
    REQUIRE(RuntimeDispatchEvidence{}.Read(reader.expected, RuntimePhase::VariableUpdate, output) == RuntimeDispatchStatus::Invalid);
    REQUIRE(output.frame == 99);
}

TEST_CASE("Dispatch reads reject foreign threads before facts and recursive frames before counter mutation", "[runtime][dispatch]") {
    DispatchFixture fixture;
    fixture.reader->crossThread = true;
    fixture.reader->reenter = true;
    REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
    REQUIRE(fixture.reader->crossThreadStatus == RuntimeDispatchStatus::WrongThread);
    REQUIRE(fixture.reader->untouchedFacts.frame == 77);
    REQUIRE(fixture.reader->reentrantError);
    REQUIRE(fixture.reader->extractionFacts.frame == 1);
    bool rejected = false;
    std::thread wrongOwner([&] {
        rejected = fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasError();
    });
    wrongOwner.join();
    REQUIRE(rejected);
    fixture.reader->crossThread = false;
    fixture.reader->reenter = false;
    REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
    REQUIRE(fixture.reader->extractionFacts.frame == 2);
}

TEST_CASE("Actual callback cancellation or exceptions revoke proof without a variable completion fence", "[runtime][dispatch]") {
    DispatchFixture fixture;
    SECTION("contained exception") {
        fixture.reader->throwVariable = true;
    }
    SECTION("post callback cancellation") {
        fixture.reader->cancel = &fixture.cancellation;
    }
    REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasError());
    REQUIRE(fixture.reader->extractions == 0);
    REQUIRE(fixture.reader->variableFacts.completedVariableUpdateFrame == 0);
    RuntimeDispatchFacts output;
    REQUIRE(fixture.reader->variableEvidence.Read(fixture.reader->expected, RuntimePhase::VariableUpdate, output) ==
            RuntimeDispatchStatus::Stale);
}

TEST_CASE("Fixed dispatch admission distinguishes the pending attempt from the later successful fence", "[runtime][dispatch]") {
    DispatchFixture fixture;
    REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
    REQUIRE(fixture.reader->fixedStatus == RuntimeDispatchStatus::Valid);
    REQUIRE(fixture.reader->fixedFacts.fixedTick == 1);
    REQUIRE(fixture.reader->fixedFacts.fixedAttempt == 1);
    REQUIRE(fixture.reader->fixedFacts.committedTick == 0);
    REQUIRE(fixture.reader->variableFacts.committedTick == 1);
    REQUIRE(fixture.reader->variableFacts.committedAttempt == 1);
    REQUIRE(fixture.reader->variableFacts.committedDuration == Duration::FromMilliseconds(10));
    RuntimeDispatchFacts ignored;
    REQUIRE(fixture.reader->fixedEvidence.Read(fixture.reader->expected, RuntimePhase::FixedUpdate, ignored) ==
            RuntimeDispatchStatus::Stale);
}

TEST_CASE("Actual host fatal shutdown retires retained dispatch pins without granting another callback", "[runtime][dispatch]") {
    DeterministicClock clock;
    auto made = RuntimeHost::Create(clock);
    REQUIRE(made.HasValue());
    auto host = std::move(made).Value();
    auto participant = std::make_unique<DispatchReader>(host->DispatchSource());
    auto *reader = participant.get();
    reader->throwVariable = true;
    REQUIRE(host->AddParticipant(std::move(participant)).HasValue());
    REQUIRE(host->Startup().HasValue());
    REQUIRE(host->RunFrame().HasError());
    auto proof = reader->variableEvidence;
    auto expected = reader->expected;
    host.reset();
    RuntimeDispatchFacts output{.frame = 19};
    REQUIRE(proof.Read(expected, RuntimePhase::VariableUpdate, output) == RuntimeDispatchStatus::Retired);
    REQUIRE(output.frame == 19);
}

TEST_CASE("Success and suspended pump dispatches allocate and free no producer storage", "[runtime][dispatch]") {
    DispatchFixture fixture;
    REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
    const auto allocations = Horo::Tests::AllocationProbe::Count();
    const auto frees = Horo::Tests::AllocationProbe::FreeCount();
    const auto running = fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false);
    const auto suspended = fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, true);
    const auto afterAllocations = Horo::Tests::AllocationProbe::Count();
    const auto afterFrees = Horo::Tests::AllocationProbe::FreeCount();
    REQUIRE(running.HasValue());
    REQUIRE(suspended.HasValue());
    REQUIRE(afterAllocations == allocations);
    REQUIRE(afterFrees == frees);
    REQUIRE(fixture.reader->extractions == 2);
}

TEST_CASE("Actual dispatch identity primitive never wraps or mutates the exhausted namespace", "[runtime][dispatch]") {
    auto ordinal = std::numeric_limits<std::uint64_t>::max() - 1;
    REQUIRE(Internal::AdvanceDispatchOrdinal(ordinal));
    REQUIRE(ordinal == std::numeric_limits<std::uint64_t>::max());
    REQUIRE_FALSE(Internal::AdvanceDispatchOrdinal(ordinal));
    REQUIRE(ordinal == std::numeric_limits<std::uint64_t>::max());
}
