#include "AllocationProbe.h"
#include "Horo/PlatformServices/PlatformOfflineQueue.h"
#include "Horo/PlatformServices/PlatformOfflineQueueErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace PlatformOfflineQueueTests {
    using namespace Horo;
    using namespace Horo::PlatformServices;
    using namespace std::chrono_literals;

    [[nodiscard]] PlatformOfflineSubjectPartition Subject(const std::uint8_t value) {
        PlatformOfflineSubjectPartition subject;
        subject.bytes.back() = static_cast<std::byte>(value);
        return subject;
    }

    [[nodiscard]] PlatformOfflineIntentId IntentId(const std::uint8_t value) {
        PlatformOfflineIntentId id;
        id.bytes.back() = static_cast<std::byte>(value);
        return id;
    }

    [[nodiscard]] PlatformOfflineQueue::TimePoint At(const std::chrono::seconds offset) {
        return PlatformOfflineQueue::TimePoint{} + offset;
    }

    [[nodiscard]] PlatformOfflineQueueConfig Config(const std::size_t active = 16, const std::size_t retained = 32,
                                                    const std::size_t perSubjectActive = 16) {
        return PlatformOfflineQueueConfig{.activeCapacity = active,
                                          .perSubjectActiveCapacity = perSubjectActive,
                                          .retainedCapacity = retained,
                                          .progressionMaximumAge = 10s,
                                          .presenceMaximumAge = 3s,
                                          .terminalRetention = 5s,
                                          .producerRedeliveryHorizon = 4s,
                                          .maximumPresenceDetailBytes = 32,
                                          .generation = {7}};
    }

    [[nodiscard]] PlatformOfflineProgressionLane StatLane(const std::uint8_t subject, const std::uint64_t stat,
                                                          const std::uint64_t policy = 1) {
        return PlatformOfflineProgressionLane{.subject = Subject(subject), .target = StatId{stat}, .policy = {policy}, .accessPolicy = {1}};
    }

    [[nodiscard]] PlatformOfflineProgressionLane ScoreLane(const std::uint8_t subject, const std::uint64_t leaderboard,
                                                           const std::uint64_t policy = 1) {
        return PlatformOfflineProgressionLane{.subject = Subject(subject),
                                              .target = LeaderboardId{leaderboard},
                                              .policy = {policy},
                                              .accessPolicy = {1}};
    }

    [[nodiscard]] PlatformOfflineProgressionLane AchievementLane(const std::uint8_t subject, const std::uint64_t achievement,
                                                                 const std::uint64_t policy = 1) {
        return PlatformOfflineProgressionLane{.subject = Subject(subject),
                                              .target = AchievementId{achievement},
                                              .policy = {policy},
                                              .accessPolicy = {1}};
    }

    [[nodiscard]] PlatformOfflinePresenceLane PresenceLane(const std::uint8_t subject, const std::uint64_t policy = 1) {
        return PlatformOfflinePresenceLane{.subject = Subject(subject),
                                           .purpose = PlatformOfflinePresencePurpose::PresencePublish,
                                           .policy = {policy},
                                           .accessPolicy = {1}};
    }

    PlatformOfflineAdmission Admit(PlatformOfflineQueue &queue, PlatformOfflineIntent intent, const std::chrono::seconds at) {
        auto admitted = queue.Admit(std::move(intent), At(at));
        REQUIRE(admitted.HasValue());
        return std::move(admitted).Value();
    }

    [[nodiscard]] PlatformOfflineIntent StatMaximum(const std::uint8_t id, const std::uint8_t subject, const std::uint64_t stat,
                                                    const std::int64_t value, const std::uint64_t policy = 1) {
        const auto lane = StatLane(subject, stat, policy);
        return PlatformOfflineIntent{.id = IntentId(id),
                                     .lane = lane,
                                     .operation = PlatformOfflineSetStatMaximum{.stat = StatId{stat}, .value = value}};
    }

}  // namespace PlatformOfflineQueueTests

using Horo::PlatformServices::AchievementId;
using Horo::PlatformServices::LeaderboardId;
using Horo::PlatformServices::PlatformOfflineAddStatOnce;
using Horo::PlatformServices::PlatformOfflineAdmission;
using Horo::PlatformServices::PlatformOfflineAdmissionDisposition;
using Horo::PlatformServices::PlatformOfflineIntent;
using Horo::PlatformServices::PlatformOfflineIntentId;
using Horo::PlatformServices::PlatformOfflineIntentState;
using Horo::PlatformServices::PlatformOfflinePresenceDesiredState;
using Horo::PlatformServices::PlatformOfflinePresenceLane;
using Horo::PlatformServices::PlatformOfflinePresencePurpose;
using Horo::PlatformServices::PlatformOfflineProgressionLane;
using Horo::PlatformServices::PlatformOfflineQueue;
using Horo::PlatformServices::PlatformOfflineQueueConfig;
using Horo::PlatformServices::PlatformOfflineScoreOrder;
using Horo::PlatformServices::PlatformOfflineSetStatMaximum;
using Horo::PlatformServices::PlatformOfflineSubjectPartition;
using Horo::PlatformServices::PlatformOfflineSubmitBestScore;
using Horo::PlatformServices::PlatformRequestMutation;
using Horo::PlatformServices::PresenceStatusId;
using Horo::PlatformServices::StatId;
using PlatformOfflineQueueTests::AchievementLane;
using PlatformOfflineQueueTests::Admit;
using PlatformOfflineQueueTests::At;
using PlatformOfflineQueueTests::Config;
using PlatformOfflineQueueTests::IntentId;
using PlatformOfflineQueueTests::PresenceLane;
using PlatformOfflineQueueTests::ScoreLane;
using PlatformOfflineQueueTests::StatLane;
using PlatformOfflineQueueTests::StatMaximum;
using PlatformOfflineQueueTests::Subject;
using std::chrono_literals::operator""s;

TEST_CASE("Platform offline monotonic coalescing preserves the semantic result and every receipt",
          "[platform-services][offline][coalescing]") {
    PlatformOfflineQueue queue(Config());
    const auto first = Admit(queue, StatMaximum(1, 1, 10, 12), 0s);
    const auto second = Admit(queue, StatMaximum(2, 1, 10, 5), 1s);
    const auto third = Admit(queue, StatMaximum(3, 1, 10, 23), 2s);
    CHECK(first.disposition == PlatformOfflineAdmissionDisposition::Accepted);
    CHECK(second.disposition == PlatformOfflineAdmissionDisposition::Coalesced);
    CHECK(third.disposition == PlatformOfflineAdmissionDisposition::Coalesced);

    const auto maxSnapshot = queue.Query(IntentId(1));
    REQUIRE(maxSnapshot.HasValue());
    CHECK(std::get<PlatformOfflineSetStatMaximum>(maxSnapshot.Value().operation).value == 23);
    CHECK(maxSnapshot.Value().receipts.size() == 3);
    CHECK(queue.PendingInLane(StatLane(1, 10)).size() == 1);
    REQUIRE(queue.MarkDispatching(first.operation, At(7s)).HasValue());
    REQUIRE(queue.CompleteSuccess(first.operation, At(8s)).HasValue());
    const auto completed = queue.Query(IntentId(1));
    REQUIRE(completed.HasValue());
    for (const auto &receipt : completed.Value().receipts)
        CHECK(receipt.state == PlatformOfflineIntentState::Succeeded);
}

TEST_CASE("Platform offline best-score coalescing keeps the semantic optimum in both score orders",
          "[platform-services][offline][coalescing]") {
    PlatformOfflineQueue queue(Config());
    const auto scoreLane = ScoreLane(1, 20);
    const auto lowFirst =
        Admit(queue,
              PlatformOfflineIntent{.id = IntentId(4),
                                    .lane = scoreLane,
                                    .operation = PlatformOfflineSubmitBestScore{.leaderboard = LeaderboardId{20},
                                                                                .score = 30,
                                                                                .order = PlatformOfflineScoreOrder::LowerIsBetter}},
              3s);
    const auto highLater =
        Admit(queue,
              PlatformOfflineIntent{.id = IntentId(5),
                                    .lane = scoreLane,
                                    .operation = PlatformOfflineSubmitBestScore{.leaderboard = LeaderboardId{20},
                                                                                .score = 80,
                                                                                .order = PlatformOfflineScoreOrder::LowerIsBetter}},
              4s);
    REQUIRE(lowFirst.operation == highLater.operation);
    const auto scoreSnapshot = queue.Query(IntentId(5));
    REQUIRE(scoreSnapshot.HasValue());
    CHECK(std::get<PlatformOfflineSubmitBestScore>(scoreSnapshot.Value().operation).score == 30);

    const auto higherBetterLane = ScoreLane(1, 21);
    Admit(queue,
          PlatformOfflineIntent{.id = IntentId(9),
                                .lane = higherBetterLane,
                                .operation = PlatformOfflineSubmitBestScore{.leaderboard = LeaderboardId{21},
                                                                            .score = 30,
                                                                            .order = PlatformOfflineScoreOrder::HigherIsBetter}},
          5s);
    Admit(queue,
          PlatformOfflineIntent{.id = IntentId(10),
                                .lane = higherBetterLane,
                                .operation = PlatformOfflineSubmitBestScore{.leaderboard = LeaderboardId{21},
                                                                            .score = 80,
                                                                            .order = PlatformOfflineScoreOrder::HigherIsBetter}},
          6s);
    CHECK(std::get<PlatformOfflineSubmitBestScore>(queue.Query(IntentId(9)).Value().operation).score == 80);

    PlatformOfflineQueue reversed(Config());
    Admit(reversed, StatMaximum(6, 1, 10, 23), 0s);
    Admit(reversed, StatMaximum(7, 1, 10, 12), 1s);
    const auto reversedResult = Admit(reversed, StatMaximum(8, 1, 10, 5), 2s);
    const auto reversedSnapshot = reversed.Query(IntentId(8));
    REQUIRE(reversedSnapshot.HasValue());
    CHECK(reversedResult.disposition == PlatformOfflineAdmissionDisposition::Coalesced);
    CHECK(std::get<PlatformOfflineSetStatMaximum>(reversedSnapshot.Value().operation).value == 23);
}

TEST_CASE("Platform offline ordering is stable per lane and non-coalescible operations cannot overtake",
          "[platform-services][offline][ordering]") {
    PlatformOfflineQueue queue(Config());
    const auto lane = StatLane(2, 40);
    const auto first = Admit(queue,
                             PlatformOfflineIntent{.id = IntentId(11),
                                                   .lane = lane,
                                                   .operation = PlatformOfflineAddStatOnce{.stat = StatId{40}, .delta = 2}},
                             0s);
    const auto second = Admit(queue,
                              PlatformOfflineIntent{.id = IntentId(12),
                                                    .lane = lane,
                                                    .operation = PlatformOfflineAddStatOnce{.stat = StatId{40}, .delta = 3}},
                              1s);
    const auto pending = queue.PendingInLane(lane);
    REQUIRE(pending.size() == 2);
    CHECK(pending[0].handle.sequence < pending[1].handle.sequence);
    CHECK(first.operation != second.operation);

    const auto overtaking = queue.MarkDispatching(second.operation, At(2s));
    REQUIRE(overtaking.HasError());
    CHECK(overtaking.ErrorValue().code.Value() == "platform.offline.invalid_transition");
    REQUIRE(queue.MarkDispatching(first.operation, At(2s)).HasValue());
    REQUIRE(queue.MarkReconciling(first.operation).HasValue());
    CHECK(queue.MarkDispatching(second.operation, At(3s)).HasError());
    REQUIRE(queue.CompleteSuccess(first.operation, At(4s)).HasValue());
    REQUIRE(queue.MarkDispatching(second.operation, At(5s)).HasValue());
}

TEST_CASE("Platform offline presence replacement reports supersession and retains the latest desired state",
          "[platform-services][offline][coalescing]") {
    PlatformOfflineQueue queue(Config());
    const auto lane = PresenceLane(3);
    auto set =
        PlatformOfflineIntent{.id = IntentId(21),
                              .lane = lane,
                              .operation = PlatformOfflinePresenceDesiredState{.action = PlatformOfflinePresenceDesiredState::Action::Set,
                                                                               .status = PresenceStatusId{7},
                                                                               .detail = "In match"}};
    const auto earlier = Admit(queue, std::move(set), 0s);
    const auto latest =
        Admit(queue,
              PlatformOfflineIntent{.id = IntentId(22),
                                    .lane = lane,
                                    .operation =
                                        PlatformOfflinePresenceDesiredState{.action = PlatformOfflinePresenceDesiredState::Action::Clear}},
              1s);
    CHECK(latest.disposition == PlatformOfflineAdmissionDisposition::PresenceSuperseded);
    REQUIRE(latest.superseded.size() == 1);
    CHECK(latest.superseded.front() == IntentId(21));

    const auto oldSnapshot = queue.Query(IntentId(21));
    REQUIRE(oldSnapshot.HasValue());
    CHECK(oldSnapshot.Value().receipts.front().state == PlatformOfflineIntentState::Superseded);
    const auto newSnapshot = queue.Query(IntentId(22));
    REQUIRE(newSnapshot.HasValue());
    CHECK(std::get<PlatformOfflinePresenceDesiredState>(newSnapshot.Value().operation).action ==
          PlatformOfflinePresenceDesiredState::Action::Clear);
    REQUIRE(queue.PendingInLane(lane).size() == 1);
    CHECK(earlier.operation != latest.operation);
}

TEST_CASE("Rejected fresh presence replacement preserves the prior receipt", "[platform-services][offline][coalescing][capacity]") {
    PlatformOfflineQueue queue(Config(2, 2, 2));
    const auto lane = PresenceLane(7);
    const auto prior =
        Admit(queue,
              PlatformOfflineIntent{.id = IntentId(95),
                                    .lane = lane,
                                    .operation =
                                        PlatformOfflinePresenceDesiredState{.action = PlatformOfflinePresenceDesiredState::Action::Set,
                                                                            .status = PresenceStatusId{8},
                                                                            .detail = "Playing"}},
              0s);
    REQUIRE(queue.Admit(StatMaximum(96, 8, 54, 6), At(1s)).HasValue());

    const auto rejected = queue.Admit(PlatformOfflineIntent{.id = IntentId(97),
                                                            .lane = lane,
                                                            .operation =
                                                                PlatformOfflinePresenceDesiredState{
                                                                    .action = PlatformOfflinePresenceDesiredState::Action::Clear}},
                                      At(2s));
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == "platform.offline.capacity_exceeded");
    CHECK(queue.Query(IntentId(95)).Value().receipts.front().state == PlatformOfflineIntentState::Pending);
    CHECK(queue.Query(IntentId(97)).ErrorValue().code.Value() == "platform.offline.expired");
    CHECK(queue.ActiveIntentCount() == 2);
    CHECK(queue.RetainedIntentCount() == 2);
    CHECK(prior.operation.IsValid());
}

TEST_CASE("Fresh presence admission leaves prior receipts unchanged on allocation failure",
          "[platform-services][offline][coalescing][allocation]") {
    bool admitted = false;
    std::size_t allocationFailures{};
    for (std::size_t successfulAllocations{}; successfulAllocations < 16 && !admitted; ++successfulAllocations) {
        PlatformOfflineQueue queue(Config());
        const auto prior =
            Admit(queue,
                  PlatformOfflineIntent{.id = IntentId(98),
                                        .lane = PresenceLane(9),
                                        .operation =
                                            PlatformOfflinePresenceDesiredState{.action = PlatformOfflinePresenceDesiredState::Action::Set,
                                                                                .status = PresenceStatusId{9},
                                                                                .detail = "Playing"}},
                  0s);
        auto replacement = PlatformOfflineIntent{.id = IntentId(99),
                                                 .lane = PresenceLane(9),
                                                 .operation = PlatformOfflinePresenceDesiredState{
                                                     .action = PlatformOfflinePresenceDesiredState::Action::Clear}};

        bool allocationFailed = false;
        PlatformOfflineAdmission admission;
        try {
            auto result = [&] {
                Horo::Tests::AllocationProbe::ScopedFailure failure{successfulAllocations};
                return queue.Admit(std::move(replacement), At(1s));
            }();
            if (result.HasError()) {
                CHECK(false);
                break;
            }
            admission = std::move(result).Value();
            admitted = true;
        } catch (const std::bad_alloc &) {
            allocationFailed = true;
            ++allocationFailures;
        }

        if (allocationFailed) {
            CHECK(queue.Query(IntentId(98)).Value().receipts.front().state == PlatformOfflineIntentState::Pending);
            CHECK(queue.Query(IntentId(99)).HasError());
            CHECK(queue.ActiveIntentCount() == 1);
            CHECK(queue.RetainedIntentCount() == 1);
            CHECK(queue.OperationCount() == 1);
            continue;
        }

        CHECK(admitted);
        CHECK(admission.disposition == PlatformOfflineAdmissionDisposition::PresenceSuperseded);
        REQUIRE(admission.superseded.size() == 1);
        CHECK(admission.superseded.front() == IntentId(98));
        CHECK(queue.Query(IntentId(98)).Value().receipts.front().state == PlatformOfflineIntentState::Superseded);
        CHECK(queue.Query(IntentId(99)).Value().receipts.front().state == PlatformOfflineIntentState::Pending);
        CHECK(queue.ActiveIntentCount() == 1);
        CHECK(queue.RetainedIntentCount() == 2);
        CHECK(queue.OperationCount() == 2);
        CHECK(prior.operation != admission.operation);
    }

    CHECK(allocationFailures > 0);
    CHECK(admitted);
}

TEST_CASE("Offline expiry preserves per-receipt deadlines and recomputes a coalesced operation", "[platform-services][offline][expiry]") {
    PlatformOfflineQueue queue(Config());
    Admit(queue, StatMaximum(31, 4, 50, 100), 0s);
    Admit(queue, StatMaximum(32, 4, 50, 20), 8s);

    const auto expired = queue.Expire(At(10s));
    REQUIRE(expired.HasValue());
    REQUIRE(expired.Value().size() == 1);
    CHECK(expired.Value().front() == IntentId(31));
    const auto oldSnapshot = queue.Query(IntentId(31));
    const auto liveSnapshot = queue.Query(IntentId(32));
    REQUIRE(oldSnapshot.HasValue());
    REQUIRE(liveSnapshot.HasValue());
    CHECK(oldSnapshot.Value().receipts.front().state == PlatformOfflineIntentState::Expired);
    CHECK(liveSnapshot.Value().receipts.back().state == PlatformOfflineIntentState::Pending);
    CHECK(std::get<PlatformOfflineSetStatMaximum>(liveSnapshot.Value().operation).value == 20);

    REQUIRE(queue.MarkDispatching(liveSnapshot.Value().handle, At(11s)).HasValue());
    REQUIRE(queue.CompleteSuccess(liveSnapshot.Value().handle, At(12s)).HasValue());
    CHECK(queue.Query(IntentId(32)).Value().receipts.back().state == PlatformOfflineIntentState::Succeeded);
    CHECK(queue.Query(IntentId(31)).Value().receipts.front().state == PlatformOfflineIntentState::Expired);
}

TEST_CASE("Dispatch leaves due lane receipts for the owner expiry transition", "[platform-services][offline][expiry][ordering]") {
    PlatformOfflineQueue queue(Config());
    const auto lane = StatLane(4, 51);
    const auto first = Admit(queue,
                             PlatformOfflineIntent{.id = IntentId(91),
                                                   .lane = lane,
                                                   .operation = PlatformOfflineAddStatOnce{.stat = StatId{51}, .delta = 1}},
                             0s);
    const auto second = Admit(queue,
                              PlatformOfflineIntent{.id = IntentId(92),
                                                    .lane = lane,
                                                    .operation = PlatformOfflineAddStatOnce{.stat = StatId{51}, .delta = 2}},
                              1s);

    const auto dispatchBeforeExpiry = queue.MarkDispatching(second.operation, At(10s));
    REQUIRE(dispatchBeforeExpiry.HasError());
    CHECK(dispatchBeforeExpiry.ErrorValue().code.Value() == "platform.offline.expired");
    CHECK(queue.Query(IntentId(91)).Value().receipts.front().state == PlatformOfflineIntentState::Pending);
    CHECK(queue.Query(IntentId(92)).Value().receipts.front().state == PlatformOfflineIntentState::Pending);

    const auto expired = queue.Expire(At(10s));
    REQUIRE(expired.HasValue());
    REQUIRE(expired.Value().size() == 1);
    CHECK(expired.Value().front() == IntentId(91));
    const auto repeatedExpiry = queue.Expire(At(10s));
    REQUIRE(repeatedExpiry.HasValue());
    CHECK(repeatedExpiry.Value().empty());
    CHECK(queue.Query(IntentId(91)).Value().receipts.front().state == PlatformOfflineIntentState::Expired);
    REQUIRE(queue.MarkDispatching(second.operation, At(10s)).HasValue());
    CHECK(queue.Query(IntentId(92)).Value().receipts.front().state == PlatformOfflineIntentState::Dispatching);
    CHECK(first.operation != second.operation);
}

TEST_CASE("Cancellation leaves due receipts for the owner expiry transition", "[platform-services][offline][expiry][cancellation]") {
    PlatformOfflineQueue queue(Config());
    const auto admission = Admit(queue, StatMaximum(93, 5, 52, 4), 0s);

    const auto cancelled = queue.CancelPending(admission.operation, At(10s));
    REQUIRE(cancelled.HasError());
    CHECK(cancelled.ErrorValue().code.Value() == "platform.offline.expired");
    CHECK(queue.Query(IntentId(93)).Value().receipts.front().state == PlatformOfflineIntentState::Pending);

    const auto expired = queue.Expire(At(10s));
    REQUIRE(expired.HasValue());
    REQUIRE(expired.Value().size() == 1);
    CHECK(expired.Value().front() == IntentId(93));
    CHECK(queue.Expire(At(10s)).Value().empty());
    CHECK(queue.Query(IntentId(93)).Value().receipts.front().state == PlatformOfflineIntentState::Expired);
}

TEST_CASE("Resume leaves due suspended receipts for the owner expiry transition", "[platform-services][offline][expiry][lifecycle]") {
    PlatformOfflineQueue queue(Config());
    const auto admission = Admit(queue, StatMaximum(94, 6, 53, 5), 0s);
    REQUIRE(queue.SuspendPending(admission.operation).HasValue());

    const auto resumed = queue.Resume(admission.operation, At(10s));
    REQUIRE(resumed.HasError());
    CHECK(resumed.ErrorValue().code.Value() == "platform.offline.expired");
    CHECK(queue.Query(IntentId(94)).Value().receipts.front().state == PlatformOfflineIntentState::Suspended);

    const auto expired = queue.Expire(At(10s));
    REQUIRE(expired.HasValue());
    REQUIRE(expired.Value().size() == 1);
    CHECK(expired.Value().front() == IntentId(94));
    CHECK(queue.Expire(At(10s)).Value().empty());
    CHECK(queue.Query(IntentId(94)).Value().receipts.front().state == PlatformOfflineIntentState::Expired);
}

TEST_CASE("Expired work is observable, cannot become success, and compaction returns every retired identity",
          "[platform-services][offline][retention]") {
    PlatformOfflineQueue queue(Config());
    const auto admission = Admit(queue, StatMaximum(41, 5, 60, 4), 0s);
    REQUIRE(queue.Expire(At(10s)).HasValue());
    const auto expired = queue.Query(IntentId(41));
    REQUIRE(expired.HasValue());
    CHECK(expired.Value().receipts.front().state == PlatformOfflineIntentState::Expired);
    CHECK(queue.MarkDispatching(admission.operation, At(10s)).HasError());
    CHECK(queue.CompleteSuccess(admission.operation, At(11s)).HasError());

    const auto tooEarly = queue.Compact(At(14s));
    REQUIRE(tooEarly.HasValue());
    CHECK(tooEarly.Value().empty());
    const auto compacted = queue.Compact(At(15s));
    REQUIRE(compacted.HasValue());
    REQUIRE(compacted.Value().size() == 1);
    CHECK(compacted.Value().front() == IntentId(41));
    CHECK(queue.Query(IntentId(41)).ErrorValue().code.Value() == "platform.offline.expired");
    CHECK(queue.RetainedIntentCount() == 0);
}

TEST_CASE("Remote ambiguity survives expiry and terminal compaction until it is reconciled", "[platform-services][offline][lifecycle]") {
    PlatformOfflineQueue queue(Config());
    const auto admission = Admit(queue, StatMaximum(51, 6, 70, 9), 0s);
    REQUIRE(queue.MarkDispatching(admission.operation, At(1s)).HasValue());
    REQUIRE(queue.MarkReconciling(admission.operation).HasValue());

    const auto expired = queue.Expire(At(30s));
    REQUIRE(expired.HasValue());
    CHECK(expired.Value().empty());
    const auto compacted = queue.Compact(At(40s));
    REQUIRE(compacted.HasValue());
    CHECK(compacted.Value().empty());
    CHECK(queue.Query(IntentId(51)).Value().receipts.front().state == PlatformOfflineIntentState::Reconciling);

    REQUIRE(queue.CompletePermanentlyFailed(admission.operation, At(41s)).HasValue());
    CHECK(queue.Query(IntentId(51)).Value().receipts.front().state == PlatformOfflineIntentState::PermanentlyFailed);
    CHECK(queue.Compact(At(46s)).Value().size() == 1);
}

TEST_CASE("Shutdown closes admission, suspends unsent work, and retains dispatched work for reconciliation",
          "[platform-services][offline][shutdown]") {
    PlatformOfflineQueue queue(Config());
    const auto pending = Admit(queue, StatMaximum(55, 9, 75, 3), 0s);
    const auto dispatching = Admit(queue, StatMaximum(56, 10, 76, 4), 0s);
    REQUIRE(queue.MarkDispatching(dispatching.operation, At(1s)).HasValue());

    const auto shutdown = queue.Shutdown(At(2s));
    REQUIRE(shutdown.HasValue());
    CHECK(shutdown.Value().empty());
    CHECK(queue.Query(IntentId(55)).Value().receipts.front().state == PlatformOfflineIntentState::Suspended);
    CHECK(queue.Query(IntentId(56)).Value().receipts.front().state == PlatformOfflineIntentState::Reconciling);
    CHECK(queue.MarkDispatching(pending.operation, At(3s)).ErrorValue().code.Value() == "platform.offline.unavailable");
    CHECK(queue.Admit(StatMaximum(57, 9, 77, 8), At(3s)).ErrorValue().code.Value() == "platform.offline.unavailable");
    CHECK(queue.Resume(pending.operation, At(3s)).ErrorValue().code.Value() == "platform.offline.unavailable");

    REQUIRE(queue.CompleteSuccess(dispatching.operation, At(4s)).HasValue());
    CHECK(queue.Query(IntentId(56)).Value().receipts.front().state == PlatformOfflineIntentState::Succeeded);
    CHECK(queue.Shutdown(At(5s)).Value().empty());
}

TEST_CASE("Offline queue bounds active receipts per protected subject", "[platform-services][offline][capacity]") {
    PlatformOfflineQueue queue(Config(4, 8, 1));
    REQUIRE(queue.Admit(StatMaximum(81, 1, 80, 5), At(0s)).HasValue());

    const auto sameSubject = queue.Admit(StatMaximum(82, 1, 81, 6), At(1s));
    REQUIRE(sameSubject.HasError());
    CHECK(sameSubject.ErrorValue().code.Value() == "platform.offline.capacity_exceeded");

    CHECK(queue.Admit(StatMaximum(83, 2, 80, 7), At(1s)).HasValue());
    CHECK(queue.ActiveIntentCount() == 2);
}

TEST_CASE("Offline cancellation abandons only unsent work and preserves terminal outcomes", "[platform-services][offline][cancellation]") {
    PlatformOfflineQueue queue(Config());
    const auto pending = Admit(queue, StatMaximum(84, 3, 82, 4), 0s);
    const auto joined = Admit(queue, StatMaximum(86, 3, 82, 6), 1s);
    const auto dispatching = Admit(queue, StatMaximum(85, 4, 83, 5), 2s);
    REQUIRE(joined.operation == pending.operation);
    REQUIRE(queue.MarkDispatching(dispatching.operation, At(2s)).HasValue());

    const auto cancelled = queue.CancelPending(pending.operation, At(3s));
    REQUIRE(cancelled.HasValue());
    CHECK(cancelled.Value() == PlatformRequestMutation::Applied);
    CHECK(queue.Query(IntentId(84)).Value().receipts.front().state == PlatformOfflineIntentState::Abandoned);
    CHECK(queue.Query(IntentId(86)).Value().receipts.front().state == PlatformOfflineIntentState::Abandoned);
    CHECK(queue.CancelPending(pending.operation, At(4s)).Value() == PlatformRequestMutation::Unchanged);

    const auto dispatchedCancellation = queue.CancelPending(dispatching.operation, At(5s));
    REQUIRE(dispatchedCancellation.HasError());
    CHECK(dispatchedCancellation.ErrorValue().code.Value() == "platform.offline.invalid_transition");
    REQUIRE(queue.CompleteSuccess(dispatching.operation, At(6s)).HasValue());
    CHECK(queue.Query(IntentId(85)).Value().receipts.front().state == PlatformOfflineIntentState::Succeeded);
}

TEST_CASE("Exact duplicate IDs join while conflicting payloads fail without evicting accepted work",
          "[platform-services][offline][identity][capacity]") {
    PlatformOfflineQueue queue(Config(1, 2));
    auto original = StatMaximum(61, 7, 80, 5);
    const auto first = Admit(queue, original, 0s);
    const auto duplicate = queue.Admit(original, At(1s));
    REQUIRE(duplicate.HasValue());
    CHECK(duplicate.Value().disposition == PlatformOfflineAdmissionDisposition::JoinedExisting);
    CHECK(duplicate.Value().operation == first.operation);

    auto conflictIntent = StatMaximum(61, 7, 80, 6);
    const auto conflict = queue.Admit(conflictIntent, At(2s));
    REQUIRE(conflict.HasError());
    CHECK(conflict.ErrorValue().code.Value() == "platform.offline.identity_conflict");

    const auto full = queue.Admit(StatMaximum(62, 7, 81, 10), At(3s));
    REQUIRE(full.HasError());
    CHECK(full.ErrorValue().code.Value() == "platform.offline.capacity_exceeded");
    CHECK(queue.Query(IntentId(61)).Value().receipts.front().state == PlatformOfflineIntentState::Pending);
    CHECK(queue.RetainedIntentCount() == 1);
}

TEST_CASE("Offline queue rejects malformed lanes, invalid configuration, and backward monotonic time",
          "[platform-services][offline][validation]") {
    auto invalidConfig = Config();
    invalidConfig.presenceMaximumAge = invalidConfig.progressionMaximumAge;
    PlatformOfflineQueue invalidQueue(invalidConfig);
    const auto invalidAdmission = invalidQueue.Admit(StatMaximum(71, 8, 90, 1), At(0s));
    REQUIRE(invalidAdmission.HasError());
    CHECK(invalidAdmission.ErrorValue().code.Value() == "platform.offline.invalid_configuration");

    auto insufficientRetention = Config();
    insufficientRetention.producerRedeliveryHorizon = 6s;
    PlatformOfflineQueue shortRetentionQueue(insufficientRetention);
    const auto shortRetentionAdmission = shortRetentionQueue.Admit(StatMaximum(74, 8, 90, 1), At(0s));
    REQUIRE(shortRetentionAdmission.HasError());
    CHECK(shortRetentionAdmission.ErrorValue().code.Value() == "platform.offline.invalid_configuration");

    PlatformOfflineQueue queue(Config());
    const auto badLane = PlatformOfflineIntent{.id = IntentId(72),
                                               .lane = AchievementLane(8, 99),
                                               .operation = PlatformOfflineSetStatMaximum{.stat = StatId{90}, .value = 1}};
    const auto malformed = queue.Admit(badLane, At(0s));
    REQUIRE(malformed.HasError());
    CHECK(malformed.ErrorValue().code.Value() == "platform.offline.invalid_intent");

    Admit(queue, StatMaximum(73, 8, 90, 2), 2s);
    const auto rollback = queue.Expire(At(1s));
    REQUIRE(rollback.HasError());
    CHECK(rollback.ErrorValue().code.Value() == "platform.offline.clock_moved_backward");
}
