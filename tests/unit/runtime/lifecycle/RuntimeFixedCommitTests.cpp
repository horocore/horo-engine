#include "AllocationProbe.h"
#include "Horo/Runtime/RuntimeHost.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;

    enum class Failure {
        None,
        Update,
        UpdateException,
        Prepare,
        PrepareException,
        PrepareCancellation,
        Suspend,
        PublishCancellation,
        PublishRetirement,
        PrepareRetirement
    };

    struct Trace final {
        void Append(const unsigned event) noexcept {
            if (count < entries.size())
                entries[count++] = event;
        }

        std::array<unsigned, 64> entries{};
        std::size_t count{};
    };

    class StagedParticipant final : public RuntimeLifecycleParticipant {
    public:
        StagedParticipant(const unsigned identity, Trace &trace, RuntimeDispatchSource source)
            : identity(identity), trace(trace), source(std::move(source)) {}

        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            staged = false;
        }

        Result<void> OnPhase(RuntimePhase, const FrameContext &) override {
            return Result<void>::Success();
        }

        Result<void> InjectedFailure() const {
            return Result<void>::Failure({ErrorCode{"runtime.test.fixed_commit"},
                                          ErrorDomainId{"runtime.test"},
                                          ErrorSeverity::Error,
                                          "Injected fixed transaction failure.",
                                          {}});
        }

        Result<void> OnFixedUpdate(const FixedStepContext &context) override {
            staged = true;
            trace.Append(identity * 10 + 1);
            evidence = context.dispatchEvidence;
            updateStatus = evidence.Read(source, RuntimePhase::FixedUpdate, updateFacts);
            if (failure == Failure::UpdateException)
                throw std::runtime_error("Injected fixed update exception.");
            return failure == Failure::Update ? InjectedFailure() : Result<void>::Success();
        }

        Result<void> PrepareFixedCommit(const FixedStepContext &context) override {
            trace.Append(identity * 10 + 2);
            prepareStatus = context.dispatchEvidence.Read(source, RuntimePhase::FixedUpdate, prepareFacts);
            if (failure == Failure::PrepareException)
                throw std::runtime_error("Injected fixed preparation exception.");
            if (failure == Failure::PrepareCancellation)
                cancellation->RequestCancellation();
            if (failure == Failure::Suspend)
                return lifecycle->Suspend();
            if (failure == Failure::PrepareRetirement)
                scheduler->RetireDispatch();
            return failure == Failure::Prepare ? InjectedFailure() : Result<void>::Success();
        }

        void CommitFixedUpdate(const FixedStepContext &context) noexcept override {
            trace.Append(identity * 10 + 3);
            commitStatus = context.dispatchEvidence.Read(source, RuntimePhase::FixedUpdate, commitFacts);
            observedSchedulerTick = scheduler->Statistics().completedSimulationTick;
            ++published;
            staged = false;
            if (failure == Failure::PublishCancellation)
                cancellation->RequestCancellation();
            if (failure == Failure::PublishRetirement)
                scheduler->RetireDispatch();
        }

        void AbortFixedUpdate(const FixedStepContext &context) noexcept override {
            trace.Append(identity * 10 + 4);
            abortStatus = context.dispatchEvidence.Read(source, RuntimePhase::FixedUpdate, abortFacts);
            abortIdentityMatches = evidence.RefersToSameDispatch(context.dispatchEvidence);
            staged = false;
            ++aborted;
        }

        unsigned identity{};
        Trace &trace;
        RuntimeDispatchSource source;
        RuntimeDispatchEvidence evidence;
        RuntimeDispatchFacts updateFacts, prepareFacts, commitFacts, abortFacts;
        RuntimeDispatchStatus updateStatus{}, prepareStatus{}, commitStatus{}, abortStatus{};
        FrameScheduler *scheduler{};
        RuntimeLifecycle *lifecycle{};
        CancellationSource *cancellation{};
        Failure failure{};
        unsigned published{}, aborted{};
        std::uint64_t observedSchedulerTick{};
        bool staged{};
        bool abortIdentityMatches{};
    };

    struct FixedFixture final {
        FixedFixture() {
            auto made = FrameScheduler::Create(clock, {.fixedStep = Duration::FromMilliseconds(10)});
            REQUIRE(made.HasValue());
            scheduler = std::move(made).Value();
            for (unsigned index = 0; index < owners.size(); ++index) {
                auto owner = std::make_unique<StagedParticipant>(index + 1, trace, scheduler->DispatchSource());
                owners[index] = owner.get();
                owner->scheduler = scheduler.get();
                owner->lifecycle = &lifecycle;
                owner->cancellation = &cancellation;
                REQUIRE(lifecycle.AddParticipant(std::move(owner)).HasValue());
            }
            REQUIRE(lifecycle.Startup(token).HasValue());
            REQUIRE(scheduler->RunFrame(lifecycle, token, false).HasValue());
        }

        Result<void> Attempt() {
            clock.Advance(Duration::FromMilliseconds(10));
            return scheduler->RunFrame(lifecycle, token, false);
        }

        DeterministicClock clock;
        CancellationSource cancellation;
        CancellationToken token{cancellation.Token()};
        Trace trace;
        std::unique_ptr<FrameScheduler> scheduler;
        RuntimeLifecycle lifecycle;
        std::array<StagedParticipant *, 2> owners{};
    };
}  // namespace

TEST_CASE("Fixed publication follows all preparations and actual scheduler commitment", "[runtime][fixed-commit]") {
    FixedFixture fixture;
    REQUIRE(fixture.Attempt().HasValue());
    REQUIRE(fixture.trace.count == 6);
    const std::array<unsigned, 6> expected{11, 21, 12, 22, 13, 23};
    for (std::size_t index = 0; index < expected.size(); ++index)
        REQUIRE(fixture.trace.entries[index] == expected[index]);
    for (auto *owner : fixture.owners) {
        REQUIRE(owner->prepareStatus == RuntimeDispatchStatus::Valid);
        REQUIRE(owner->prepareFacts.committedTick == 0);
        REQUIRE(owner->commitStatus == RuntimeDispatchStatus::Valid);
        REQUIRE(owner->commitFacts.committedTick == owner->updateFacts.fixedTick);
        REQUIRE(owner->commitFacts.committedAttempt == owner->updateFacts.fixedAttempt);
        REQUIRE(owner->commitFacts.committedFrame == owner->updateFacts.frame);
        REQUIRE(owner->commitFacts.committedDuration == Duration::FromMilliseconds(10));
        REQUIRE(owner->observedSchedulerTick == 1);
        REQUIRE(owner->published == 1);
        REQUIRE(owner->aborted == 0);
        RuntimeDispatchFacts ignored;
        REQUIRE(owner->evidence.Read(owner->source, RuntimePhase::FixedUpdate, ignored) == RuntimeDispatchStatus::Stale);
    }
}

TEST_CASE("Later update or preparation failures abort every staged owner in reverse order", "[runtime][fixed-commit]") {
    FixedFixture fixture;
    SECTION("update failure") {
        fixture.owners[1]->failure = Failure::Update;
    }
    SECTION("update exception") {
        fixture.owners[1]->failure = Failure::UpdateException;
    }
    SECTION("preparation failure") {
        fixture.owners[1]->failure = Failure::Prepare;
    }
    SECTION("preparation exception") {
        fixture.owners[1]->failure = Failure::PrepareException;
    }
    SECTION("last cancellation fence") {
        fixture.owners[1]->failure = Failure::PrepareCancellation;
    }
    SECTION("lifecycle suspension") {
        fixture.owners[1]->failure = Failure::Suspend;
    }
    REQUIRE(fixture.Attempt().HasError());
    REQUIRE(fixture.scheduler->Statistics().completedSimulationTick == 0);
    REQUIRE(fixture.trace.entries[fixture.trace.count - 2] == 24);
    REQUIRE(fixture.trace.entries[fixture.trace.count - 1] == 14);
    for (auto *owner : fixture.owners) {
        REQUIRE(owner->published == 0);
        REQUIRE(owner->aborted == 1);
        REQUIRE_FALSE(owner->staged);
        REQUIRE(owner->abortStatus == RuntimeDispatchStatus::Valid);
        REQUIRE(owner->abortFacts.committedTick == 0);
    }
}

TEST_CASE("An early update failure aborts even owners that did not stage", "[runtime][fixed-commit]") {
    FixedFixture fixture;
    fixture.owners[0]->failure = Failure::Update;
    REQUIRE(fixture.Attempt().HasError());
    REQUIRE(fixture.trace.count == 3);
    REQUIRE(fixture.owners[1]->published == 0);
    REQUIRE(fixture.owners[1]->aborted == 1);
    REQUIRE_FALSE(fixture.owners[1]->staged);
}

TEST_CASE("A failed tick retries the same simulation tick with a fresh attempt", "[runtime][fixed-commit]") {
    FixedFixture fixture;
    fixture.owners[1]->failure = Failure::Prepare;
    REQUIRE(fixture.Attempt().HasError());
    const auto failedAttempt = fixture.owners[0]->updateFacts.fixedAttempt;
    fixture.owners[1]->failure = Failure::None;
    REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
    REQUIRE(fixture.owners[0]->commitFacts.committedTick == 1);
    REQUIRE(fixture.owners[0]->commitFacts.committedAttempt > failedAttempt);
    REQUIRE(fixture.owners[0]->published == 1);
}

TEST_CASE("Cancellation after the fixed commitment fence cannot partially publish its owners", "[runtime][fixed-commit]") {
    FixedFixture fixture;
    fixture.owners[0]->failure = Failure::PublishCancellation;
    REQUIRE(fixture.Attempt().HasError());
    REQUIRE(fixture.scheduler->Statistics().completedSimulationTick == 1);
    for (auto *owner : fixture.owners) {
        REQUIRE(owner->published == 1);
        REQUIRE(owner->aborted == 0);
        REQUIRE(owner->commitFacts.committedTick == 1);
    }
}

TEST_CASE("Fixed preparation and publication remain allocation free across catch-up ticks", "[runtime][fixed-commit]") {
    FixedFixture fixture;
    fixture.clock.Advance(Duration::FromMilliseconds(30));
    const auto allocations = Tests::AllocationProbe::Count();
    const auto frees = Tests::AllocationProbe::FreeCount();
    auto result = fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false);
    const auto afterAllocations = Tests::AllocationProbe::Count();
    const auto afterFrees = Tests::AllocationProbe::FreeCount();
    REQUIRE(result.HasValue());
    REQUIRE(afterAllocations == allocations);
    REQUIRE(afterFrees == frees);
    REQUIRE(fixture.scheduler->Statistics().completedSimulationTick == 3);
    REQUIRE(fixture.owners[0]->published == 3);
    REQUIRE(fixture.owners[1]->published == 3);
}

TEST_CASE("Retirement after the fixed fence preserves same-attempt commitment evidence until publication ends", "[runtime][fixed-commit]") {
    FixedFixture fixture;
    fixture.owners[0]->failure = Failure::PublishRetirement;
    REQUIRE(fixture.Attempt().HasError());
    REQUIRE(fixture.scheduler->Statistics().completedSimulationTick == 1);
    for (auto *owner : fixture.owners) {
        REQUIRE(owner->published == 1);
        REQUIRE(owner->aborted == 0);
        REQUIRE(owner->commitStatus == RuntimeDispatchStatus::Valid);
        REQUIRE(owner->commitFacts.committedTick == 1);
        RuntimeDispatchFacts untouched{.frame = 99};
        REQUIRE(owner->evidence.Read(owner->source, RuntimePhase::FixedUpdate, untouched) == RuntimeDispatchStatus::Retired);
        REQUIRE(untouched.frame == 99);
    }
    REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasError());
    REQUIRE(fixture.scheduler->Statistics().completedSimulationTick == 1);
}

TEST_CASE("Retirement before the fixed fence preserves exact rollback identity without fact admission", "[runtime][fixed-commit]") {
    FixedFixture fixture;
    fixture.owners[1]->failure = Failure::PrepareRetirement;
    REQUIRE(fixture.Attempt().HasError());
    REQUIRE(fixture.scheduler->Statistics().completedSimulationTick == 0);
    REQUIRE(fixture.trace.entries[fixture.trace.count - 2] == 24);
    REQUIRE(fixture.trace.entries[fixture.trace.count - 1] == 14);
    for (auto *owner : fixture.owners) {
        REQUIRE(owner->aborted == 1);
        REQUIRE(owner->published == 0);
        REQUIRE_FALSE(owner->staged);
        REQUIRE(owner->abortStatus == RuntimeDispatchStatus::Retired);
        REQUIRE(owner->abortIdentityMatches);
        REQUIRE_FALSE(owner->evidence.RefersToSameDispatch(RuntimeDispatchEvidence{}));
    }
}
