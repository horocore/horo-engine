#include "AllocationProbe.h"
#include "UiAnimationCommittedTicks.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <optional>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Ui;
    using IntegrationInternal::CommittedTickLedger;

    const CancellationToken uncancelled;

    FixedStepContext Attempt(const std::uint64_t tick, const std::uint64_t attempt, const std::uint64_t frame,
                             const std::int64_t duration) {
        return {tick, Duration::FromNanoseconds(duration), uncancelled, attempt, frame};
    }

    FrameContext Commitment(const FixedStepContext &attempt, const std::uint64_t frame) {
        return {frame,
                {},
                0.0,
                attempt.simulationTick,
                {},
                false,
                uncancelled,
                1,
                PresentationClockContinuity::Continuous,
                {attempt.simulationTick, attempt.attemptNumber, attempt.frameNumber, attempt.fixedDelta}};
    }

    FrameContext NoCommitment(const std::uint64_t frame) {
        return {frame, {}, 0.0, 0, {}, false, uncancelled};
    }

    void CheckError(const Result<void> &result, const ErrorCodeDescriptor &expected) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == expected.code.Value());
    }
}  // namespace

TEST_CASE("UI fixed ledger replaces failed same-tick duration with the exact successful attempt", "[runtime][ui][clock]") {
    auto created = CommittedTickLedger::Create(2);
    REQUIRE(created.HasValue());
    auto ledger = std::move(created).Value();
    const auto failed = Attempt(1, 1, 1, 17);
    const auto retry = Attempt(1, 2, 2, 31);
    REQUIRE(ledger.Stage(failed).HasValue());
    auto before = ledger.Prepare(NoCommitment(1));
    REQUIRE(before.HasValue());
    CHECK(before.Value().Delta().nanoseconds == 0);
    REQUIRE(ledger.Commit(before.Value()).HasValue());
    REQUIRE(ledger.Stage(retry).HasValue());
    auto stale = ledger.Prepare(Commitment(failed, 2));
    REQUIRE(stale.HasError());
    CHECK(stale.ErrorValue().code.Value() == UiErrors::ClockSourceStale.code.Value());
    auto current = ledger.Prepare(Commitment(retry, 2));
    REQUIRE(current.HasValue());
    CHECK(current.Value().Delta().nanoseconds == 31);
    REQUIRE(ledger.Commit(current.Value()).HasValue());
    CheckError(ledger.Commit(current.Value()), UiErrors::ClockSourceStale);
    auto duplicate = ledger.Prepare(Commitment(retry, 3));
    REQUIRE(duplicate.HasValue());
    CHECK(duplicate.Value().Delta().nanoseconds == 0);
}

TEST_CASE("UI fixed ledger retains prior successes across a later failed catchup attempt", "[runtime][ui][clock]") {
    auto created = CommittedTickLedger::Create(3);
    REQUIRE(created.HasValue());
    auto ledger = std::move(created).Value();
    const auto first = Attempt(1, 1, 1, 10);
    const auto second = Attempt(2, 2, 1, 20);
    const auto failed = Attempt(3, 3, 1, 30);
    const auto retry = Attempt(3, 4, 2, 40);
    REQUIRE(ledger.Stage(first).HasValue());
    REQUIRE(ledger.Stage(second).HasValue());
    REQUIRE(ledger.Stage(failed).HasValue());
    auto previous = ledger.Prepare(Commitment(second, 1));
    REQUIRE(previous.HasValue());
    CHECK(previous.Value().Delta().nanoseconds == 30);
    REQUIRE(ledger.Stage(retry).HasValue());
    CHECK_FALSE(ledger.CanConsume(previous.Value()));
    auto combined = ledger.Prepare(Commitment(retry, 2));
    REQUIRE(combined.HasValue());
    CHECK(combined.Value().Delta().nanoseconds == 70);
    REQUIRE(ledger.Commit(combined.Value()).HasValue());
}

TEST_CASE("UI fixed ledger can consume successful prefix without dropping the unread failed tail", "[runtime][ui][clock]") {
    auto created = CommittedTickLedger::Create(2);
    REQUIRE(created.HasValue());
    auto ledger = std::move(created).Value();
    const auto first = Attempt(1, 1, 1, 10);
    const auto failed = Attempt(2, 2, 1, 99);
    const auto retry = Attempt(2, 3, 2, 7);
    REQUIRE(ledger.Stage(first).HasValue());
    REQUIRE(ledger.Stage(failed).HasValue());
    auto prefix = ledger.Prepare(Commitment(first, 1));
    REQUIRE(prefix.HasValue());
    REQUIRE(ledger.Commit(prefix.Value()).HasValue());
    REQUIRE(ledger.Stage(retry).HasValue());
    auto remaining = ledger.Prepare(Commitment(retry, 2));
    REQUIRE(remaining.HasValue());
    CHECK(remaining.Value().Delta().nanoseconds == 7);
}

TEST_CASE("UI fixed ledger preserves admission on duplicate attempts and rejects malformed evidence", "[runtime][ui][clock]") {
    auto created = CommittedTickLedger::Create(2);
    REQUIRE(created.HasValue());
    auto ledger = std::move(created).Value();
    const auto first = Attempt(1, 1, 1, 8);
    REQUIRE(ledger.Stage(first).HasValue());
    auto candidate = ledger.Prepare(Commitment(first, 1));
    REQUIRE(candidate.HasValue());
    REQUIRE(ledger.Stage(first).HasValue());
    CHECK(ledger.CanConsume(candidate.Value()));
    CheckError(ledger.Stage(Attempt(1, 1, 1, 9)), UiErrors::ClockSourceStale);
    CheckError(ledger.Stage(Attempt(1, 2, 1, 9)), UiErrors::ClockSourceStale);
    CheckError(ledger.Stage(Attempt(3, 2, 2, 9)), UiErrors::ClockSourceStale);
    CheckError(ledger.Stage(Attempt(2, 0, 2, 9)), UiErrors::ClockInputInvalid);
    CheckError(ledger.Stage(Attempt(2, 2, 0, 9)), UiErrors::ClockInputInvalid);
    CheckError(ledger.Stage(Attempt(2, 2, 2, 0)), UiErrors::ClockInputInvalid);
    CHECK(ledger.CanConsume(candidate.Value()));
    REQUIRE(ledger.Commit(candidate.Value()).HasValue());
}

TEST_CASE("UI fixed ledger capacity failure preserves the last admitted consumption", "[runtime][ui][clock]") {
    auto created = CommittedTickLedger::Create(1);
    REQUIRE(created.HasValue());
    auto ledger = std::move(created).Value();
    const auto first = Attempt(1, 1, 1, 3);
    const auto second = Attempt(2, 2, 2, 5);
    REQUIRE(ledger.Stage(first).HasValue());
    auto candidate = ledger.Prepare(Commitment(first, 1));
    REQUIRE(candidate.HasValue());
    CheckError(ledger.Stage(second), UiErrors::AnimationStorageExhausted);
    CHECK(ledger.CanConsume(candidate.Value()));
    REQUIRE(ledger.Commit(candidate.Value()).HasValue());
    REQUIRE(ledger.Stage(second).HasValue());
    auto next = ledger.Prepare(Commitment(second, 2));
    REQUIRE(next.HasValue());
    CHECK(next.Value().Delta().nanoseconds == 5);
}

TEST_CASE("UI fixed ledger rejects duration overflow without consuming earlier successful ticks", "[runtime][ui][clock]") {
    auto created = CommittedTickLedger::Create(2);
    REQUIRE(created.HasValue());
    auto ledger = std::move(created).Value();
    const auto first = Attempt(1, 1, 1, std::numeric_limits<std::int64_t>::max());
    const auto second = Attempt(2, 2, 1, 1);
    REQUIRE(ledger.Stage(first).HasValue());
    REQUIRE(ledger.Stage(second).HasValue());
    auto overflow = ledger.Prepare(Commitment(second, 1));
    REQUIRE(overflow.HasError());
    CHECK(overflow.ErrorValue().code.Value() == UiErrors::ClockOverflow.code.Value());
    auto prefix = ledger.Prepare(Commitment(first, 1));
    REQUIRE(prefix.HasValue());
    CHECK(prefix.Value().Delta().nanoseconds == std::numeric_limits<std::int64_t>::max());
    REQUIRE(ledger.Commit(prefix.Value()).HasValue());
    auto remaining = ledger.Prepare(Commitment(second, 2));
    REQUIRE(remaining.HasValue());
    CHECK(remaining.Value().Delta().nanoseconds == 1);
}

TEST_CASE("UI fixed ledger rejects foreign and moved source candidates", "[runtime][ui][clock]") {
    auto first = CommittedTickLedger::Create(1);
    auto second = CommittedTickLedger::Create(1);
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());
    const auto tick = Attempt(1, 1, 1, 6);
    auto firstLedger = std::move(first).Value();
    auto secondLedger = std::move(second).Value();
    REQUIRE(firstLedger.Stage(tick).HasValue());
    REQUIRE(secondLedger.Stage(tick).HasValue());
    auto candidate = firstLedger.Prepare(Commitment(tick, 1));
    REQUIRE(candidate.HasValue());
    CheckError(secondLedger.Commit(candidate.Value()), UiErrors::ClockSourceStale);
    auto moved = std::move(firstLedger);
    CHECK_FALSE(firstLedger.CanConsume(candidate.Value()));
    CHECK_FALSE(moved.CanConsume(candidate.Value()));
    auto current = moved.Prepare(Commitment(tick, 1));
    REQUIRE(current.HasValue());
    CHECK(current.Value().Delta().nanoseconds == 6);
}

TEST_CASE("UI fixed ledger requires exact successful evidence and one-based host frames", "[runtime][ui][clock]") {
    auto created = CommittedTickLedger::Create(1);
    REQUIRE(created.HasValue());
    auto ledger = std::move(created).Value();
    CHECK(ledger.Prepare(NoCommitment(0)).HasError());
    const auto tick = Attempt(1, 1, 3, 11);
    REQUIRE(ledger.Stage(tick).HasValue());
    auto early = ledger.Prepare(Commitment(tick, 2));
    REQUIRE(early.HasError());
    auto wrongDuration = Commitment(tick, 3);
    wrongDuration.committedFixedStep.duration = Duration::FromNanoseconds(12);
    CHECK(ledger.Prepare(wrongDuration).HasError());
    auto missing = Commitment(tick, 3);
    missing.completedSimulationTick = 2;
    CHECK(ledger.Prepare(missing).HasError());
    auto valid = ledger.Prepare(Commitment(tick, 3));
    REQUIRE(valid.HasValue());
    CHECK(valid.Value().Delta().nanoseconds == 11);
}

TEST_CASE("UI fixed ledger transfers and releases all record storage without allocating during destruction", "[runtime][ui][clock]") {
    const auto allocationsBefore = Horo::Tests::AllocationProbe::Count();
    const auto freesBefore = Horo::Tests::AllocationProbe::FreeCount();
    std::size_t cleanupAllocations{};
    {
        std::optional<CommittedTickLedger> source{std::move(CommittedTickLedger::Create(2)).Value()};
        std::optional<CommittedTickLedger> target{std::move(CommittedTickLedger::Create(3)).Value()};
        *target = std::move(*source);
        const auto cleanupStart = Horo::Tests::AllocationProbe::Count();
        source.reset();
        target.reset();
        cleanupAllocations = Horo::Tests::AllocationProbe::Count() - cleanupStart;
    }
    const auto allocations = Horo::Tests::AllocationProbe::Count() - allocationsBefore;
    const auto frees = Horo::Tests::AllocationProbe::FreeCount() - freesBefore;
    CHECK(cleanupAllocations == 0);
    CHECK(frees == allocations);
}
