#include "AllocationProbe.h"
#include "Horo/PlatformServices/PlatformOfflineQueue.h"
#include "Horo/PlatformServices/PlatformOfflineQueueErrors.h"
#include "Horo/PlatformServices/PlatformProgressionIdempotency.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::PlatformServices;
    using namespace std::chrono_literals;

    /** @brief Creates a nonzero test-only protected subject partition. */
    [[nodiscard]] PlatformOfflineSubjectPartition Subject(const std::uint8_t value) {
        PlatformOfflineSubjectPartition subject;
        subject.bytes.back() = static_cast<std::byte>(value);
        return subject;
    }

    /** @brief Creates a nonzero test-only producer identity. */
    [[nodiscard]] PlatformOfflineIntentId IntentId(const std::uint8_t value) {
        PlatformOfflineIntentId id;
        id.bytes.back() = static_cast<std::byte>(value);
        return id;
    }

    /** @brief Maps a test offset onto the queue's monotonic clock. */
    [[nodiscard]] PlatformOfflineQueue::TimePoint At(const std::chrono::seconds offset) {
        return PlatformOfflineQueue::TimePoint{} + offset;
    }

    /** @brief Builds finite queue limits used by lifecycle regression tests. */
    [[nodiscard]] PlatformOfflineQueueConfig Config() {
        return PlatformOfflineQueueConfig{.activeCapacity = 16,
                                          .perSubjectActiveCapacity = 16,
                                          .retainedCapacity = 32,
                                          .progressionMaximumAge = 10s,
                                          .presenceMaximumAge = 3s,
                                          .terminalRetention = 5s,
                                          .producerRedeliveryHorizon = 4s,
                                          .maximumPresenceDetailBytes = 32,
                                          .generation = {7}};
    }

    /** @brief Creates the exact semantic lane used by stat receipt tests. */
    [[nodiscard]] PlatformOfflineProgressionLane StatLane(const std::uint8_t subject, const std::uint64_t stat) {
        return PlatformOfflineProgressionLane{.subject = Subject(subject), .target = StatId{stat}, .policy = {1}, .accessPolicy = {1}};
    }

    /** @brief Creates a consented presence desired-state lane. */
    [[nodiscard]] PlatformOfflinePresenceLane PresenceLane(const std::uint8_t subject) {
        return PlatformOfflinePresenceLane{.subject = Subject(subject),
                                           .purpose = PlatformOfflinePresencePurpose::PresencePublish,
                                           .policy = {1},
                                           .accessPolicy = {1}};
    }

    /** @brief Admits one intent or records a focused test setup failure. */
    PlatformOfflineAdmission Admit(PlatformOfflineQueue &queue, const PlatformOfflineIntent &intent, const std::chrono::seconds at) {
        auto admitted = queue.Admit(intent, At(at));
        REQUIRE(admitted.HasValue());
        return std::move(admitted).Value();
    }

    /** @brief Creates a progression intent with a monotonic maximum operation. */
    [[nodiscard]] PlatformOfflineIntent StatMaximum(const std::uint8_t id, const std::uint8_t subject, const std::uint64_t stat,
                                                    const std::int64_t value) {
        return PlatformOfflineIntent{.id = IntentId(id),
                                     .lane = StatLane(subject, stat),
                                     .operation = PlatformOfflineSetStatMaximum{.stat = StatId{stat}, .value = value}};
    }

    /** @brief Creates a non-coalescible AddStatOnce intent in the chosen stat lane. */
    [[nodiscard]] PlatformOfflineIntent AddStatOnce(const std::uint8_t id, const std::int64_t delta) {
        constexpr std::uint64_t stat = 51;
        return PlatformOfflineIntent{.id = IntentId(id),
                                     .lane = StatLane(4, stat),
                                     .operation = PlatformOfflineAddStatOnce{.stat = StatId{stat}, .delta = delta}};
    }

    /** @brief Admits the prior Set state and returns its aggregate handle. */
    [[nodiscard]] PlatformOfflineOperationHandle AdmitPriorPresence(PlatformOfflineQueue &queue) {
        return Admit(queue,
                     PlatformOfflineIntent{.id = IntentId(98),
                                           .lane = PresenceLane(9),
                                           .operation =
                                               PlatformOfflinePresenceDesiredState{.action =
                                                                                       PlatformOfflinePresenceDesiredState::Action::Set,
                                                                                   .status = PresenceStatusId{9},
                                                                                   .detail = "Playing"}},
                     0s)
            .operation;
    }

    /** @brief Creates the Clear replacement that exercises presence receipt supersession. */
    [[nodiscard]] PlatformOfflineIntent PresenceReplacement() {
        return PlatformOfflineIntent{.id = IntentId(99),
                                     .lane = PresenceLane(9),
                                     .operation =
                                         PlatformOfflinePresenceDesiredState{.action = PlatformOfflinePresenceDesiredState::Action::Clear}};
    }

    /** @brief Confirms a failed allocation did not publish or mutate either receipt. */
    void CheckPresenceAdmissionUnchanged(const PlatformOfflineQueue &queue) {
        CHECK(queue.Query(IntentId(98)).Value().receipts.front().state == PlatformOfflineIntentState::Pending);
        CHECK(queue.Query(IntentId(99)).HasError());
        CHECK(queue.ActiveIntentCount() == 1);
        CHECK(queue.RetainedIntentCount() == 1);
        CHECK(queue.OperationCount() == 1);
    }

    /** @brief Confirms the fully prepared replacement reports and commits supersession. */
    void CheckPresenceAdmissionCommitted(const PlatformOfflineQueue &queue, const PlatformOfflineOperationHandle prior,
                                         const PlatformOfflineAdmission &admission) {
        CHECK(admission.disposition == PlatformOfflineAdmissionDisposition::PresenceSuperseded);
        REQUIRE(admission.superseded.size() == 1);
        CHECK(admission.superseded.front() == IntentId(98));
        CHECK(queue.Query(IntentId(98)).Value().receipts.front().state == PlatformOfflineIntentState::Superseded);
        CHECK(queue.Query(IntentId(99)).Value().receipts.front().state == PlatformOfflineIntentState::Pending);
        CHECK(queue.ActiveIntentCount() == 1);
        CHECK(queue.RetainedIntentCount() == 2);
        CHECK(queue.OperationCount() == 2);
        CHECK(prior != admission.operation);
    }
}  // namespace

TEST_CASE("Fresh presence admission leaves prior receipts unchanged on allocation failure",
          "[platform-services][offline][coalescing][allocation]") {
    static_assert(std::is_aggregate_v<PlatformOfflinePresenceDesiredState>);
    static_assert(std::is_aggregate_v<PlatformOfflineIntent>);
    static_assert(std::is_aggregate_v<PlatformOfflineAdmission>);
    static_assert(!std::is_nothrow_move_constructible_v<PlatformOfflinePresenceDesiredState>);
    static_assert(!std::is_nothrow_move_constructible_v<PlatformOfflineAdmission>);
    std::size_t requests{};
    {
        PlatformOfflineQueue queue(Config());
        (void)AdmitPriorPresence(queue);
        const auto replacement = PresenceReplacement();
        Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
        const auto result = queue.Admit(replacement, At(1s));
        requests = measurement.Snapshot().requests;
        REQUIRE(result.HasValue());
    }
    REQUIRE(requests > 0);
    bool admitted = false;
    std::size_t allocationFailures{};
    for (std::size_t successfulAllocations{}; successfulAllocations <= requests && !admitted; ++successfulAllocations) {
        PlatformOfflineQueue queue(Config());
        const auto prior = AdmitPriorPresence(queue);
        const auto replacement = PresenceReplacement();
        const auto original = replacement;
        auto result = [&] {
            Horo::Tests::AllocationProbe::ScopedFailure failure{successfulAllocations};
            return queue.Admit(replacement, At(1s));
        }();
        CHECK(replacement == original);
        if (result.HasError()) {
            CHECK(result.ErrorValue().code.Value() == OfflineQueueErrors::CapacityExceeded.code.Value());
            ++allocationFailures;
            CheckPresenceAdmissionUnchanged(queue);
            const auto retry = queue.Admit(replacement, At(1s));
            REQUIRE(retry.HasValue());
            CHECK(retry.Value().operation.sequence == prior.sequence + 1);
            CheckPresenceAdmissionCommitted(queue, prior, retry.Value());
            continue;
        }
        admitted = true;
        CheckPresenceAdmissionCommitted(queue, prior, result.Value());
    }
    CHECK(allocationFailures == requests);
    CHECK(admitted);
}

TEST_CASE("Coalesced admission leaves prior receipts unchanged on allocation failure",
          "[platform-services][offline][coalescing][allocation]") {
    std::size_t requests{};
    {
        PlatformOfflineQueue queue(Config());
        (void)Admit(queue, StatMaximum(96, 9, 52, 3), 0s);
        const auto replacement = StatMaximum(97, 9, 52, 7);
        Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
        const auto result = queue.Admit(replacement, At(1s));
        requests = measurement.Snapshot().requests;
        REQUIRE(result.HasValue());
    }
    REQUIRE(requests > 0);
    bool admitted = false;
    std::size_t allocationFailures{};
    for (std::size_t successfulAllocations{}; successfulAllocations <= requests && !admitted; ++successfulAllocations) {
        PlatformOfflineQueue queue(Config());
        const auto prior = Admit(queue, StatMaximum(96, 9, 52, 3), 0s);
        const auto replacement = StatMaximum(97, 9, 52, 7);
        const auto result = [&] {
            Horo::Tests::AllocationProbe::ScopedFailure failure{successfulAllocations};
            return queue.Admit(replacement, At(1s));
        }();
        if (result.HasError()) {
            CHECK(result.ErrorValue().code.Value() == OfflineQueueErrors::CapacityExceeded.code.Value());
            ++allocationFailures;
            const auto snapshot = queue.Query(IntentId(96));
            REQUIRE(snapshot.HasValue());
            CHECK(std::get<PlatformOfflineSetStatMaximum>(snapshot.Value().operation).value == 3);
            CHECK(snapshot.Value().receipts.size() == 1);
            CHECK(queue.Query(IntentId(97)).HasError());
            CHECK(queue.ActiveIntentCount() == 1);
            CHECK(queue.RetainedIntentCount() == 1);
            CHECK(queue.OperationCount() == 1);
            const auto retry = queue.Admit(replacement, At(1s));
            REQUIRE(retry.HasValue());
            CHECK(retry.Value().operation == prior.operation);
            CHECK(queue.ActiveIntentCount() == 2);
            CHECK(queue.RetainedIntentCount() == 2);
            const auto next = Admit(queue, StatMaximum(98, 8, 53, 1), 1s);
            CHECK(next.operation.sequence == prior.operation.sequence + 1);
            continue;
        }
        admitted = true;
        CHECK(result.Value().disposition == PlatformOfflineAdmissionDisposition::Coalesced);
        CHECK(result.Value().operation == prior.operation);
        CHECK(std::get<PlatformOfflineSetStatMaximum>(queue.Query(IntentId(96)).Value().operation).value == 7);
        CHECK(queue.Query(IntentId(97)).Value().receipts.front().state == PlatformOfflineIntentState::Pending);
        CHECK(queue.ActiveIntentCount() == 2);
        CHECK(queue.RetainedIntentCount() == 2);
        CHECK(queue.OperationCount() == 1);
    }
    CHECK(allocationFailures == requests);
    CHECK(admitted);
}

TEST_CASE("Dispatch leaves due lane receipts for the owner expiry transition", "[platform-services][offline][expiry][ordering]") {
    PlatformOfflineQueue queue(Config());
    const auto first = Admit(queue, AddStatOnce(91, 1), 0s);
    const auto second = Admit(queue, AddStatOnce(92, 2), 1s);

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

TEST_CASE("Offline retries have finite attempts and a positive capped provider delay", "[platform-services][offline][retry]") {
    auto config = Config();
    config.maximumAttempts = 2;
    config.maximumRetryDelay = 4s;
    PlatformOfflineQueue queue(config);
    const auto admission = Admit(queue, AddStatOnce(1, 3), 0s);
    REQUIRE(queue.MarkDispatching(admission.operation, At(0s)).HasValue());
    REQUIRE(queue.MarkDispatching(admission.operation, At(0s)).HasValue());
    CHECK(queue.Query(IntentId(1)).Value().attempts == 1);
    REQUIRE(queue.RetryUncommitted(admission.operation, At(0s), 0, std::chrono::seconds::max()).HasValue());
    const auto pending = queue.Query(IntentId(1)).Value();
    CHECK(pending.nextEligibleAt == At(4s));
    CHECK(pending.receipts.front().expiresAt == At(10s));
    CHECK(queue.MarkDispatching(admission.operation, At(3s)).HasError());
    REQUIRE(queue.MarkDispatching(admission.operation, At(4s)).HasValue());
    REQUIRE(queue.RetryUncommitted(admission.operation, At(4s), 0).HasValue());
    const auto exhausted = queue.Query(IntentId(1)).Value();
    CHECK(exhausted.attempts == 2);
    CHECK(exhausted.receipts.front().state == PlatformOfflineIntentState::PermanentlyFailed);
    CHECK(queue.MarkDispatching(admission.operation, At(5s)).HasError());
}

TEST_CASE("Suspension and duplicate admission cannot reset retry backoff", "[platform-services][offline][retry][lifecycle]") {
    PlatformOfflineQueue queue(Config());
    const auto intent = AddStatOnce(2, 4);
    const auto admission = Admit(queue, intent, 0s);
    REQUIRE(queue.MarkDispatching(admission.operation, At(0s)).HasValue());
    REQUIRE(queue.RetryUncommitted(admission.operation, At(0s), 0, 4s).HasValue());
    REQUIRE(queue.SuspendPending(admission.operation).HasValue());
    CHECK(queue.RetryUncommitted(admission.operation, At(1s), 0).HasError());
    REQUIRE(queue.Resume(admission.operation, At(1s)).HasValue());
    CHECK(Admit(queue, intent, 1s).disposition == PlatformOfflineAdmissionDisposition::JoinedExisting);
    CHECK(queue.MarkDispatching(admission.operation, At(1s)).HasError());
    CHECK(queue.Query(IntentId(2)).Value().nextEligibleAt == At(4s));
    REQUIRE(queue.Shutdown(At(2s)).HasValue());
    CHECK(queue.RetryUncommitted(admission.operation, At(2s), 0).HasError());
    CHECK(queue.MarkDispatching(admission.operation, At(4s)).HasError());
}

TEST_CASE("Remote ambiguity never becomes retryable or expires at the attempt cap", "[platform-services][offline][retry][ambiguity]") {
    auto config = Config();
    config.maximumAttempts = 1;
    PlatformOfflineQueue queue(config);
    const auto admission = Admit(queue, AddStatOnce(3, 7), 0s);
    REQUIRE(queue.MarkDispatching(admission.operation, At(0s)).HasValue());
    REQUIRE(queue.MarkReconciling(admission.operation).HasValue());
    CHECK(queue.RetryUncommitted(admission.operation, At(1s), 0).HasError());
    REQUIRE(queue.Expire(At(100s)).HasValue());
    REQUIRE(queue.Compact(At(100s)).HasValue());
    CHECK(queue.Query(IntentId(3)).Value().state == PlatformOfflineOperationState::Reconciling);
    CHECK(queue.ActiveIntentCount() == 1);
}

TEST_CASE("Retry keeps independent lanes runnable and its own envelope frozen", "[platform-services][offline][retry][ordering]") {
    PlatformOfflineQueue queue(Config());
    const auto first = Admit(queue, StatMaximum(4, 1, 51, 2), 0s);
    REQUIRE(queue.MarkDispatching(first.operation, At(0s)).HasValue());
    REQUIRE(queue.RetryUncommitted(first.operation, At(0s), 0, 4s).HasValue());
    const auto second = Admit(queue, StatMaximum(5, 1, 51, 9), 1s);
    CHECK(second.disposition == PlatformOfflineAdmissionDisposition::Accepted);
    CHECK(second.operation != first.operation);
    CHECK(std::get<PlatformOfflineSetStatMaximum>(queue.Query(IntentId(4)).Value().operation).value == 2);
    CHECK(queue.MarkDispatching(second.operation, At(1s)).HasError());
    const auto independent = Admit(queue, StatMaximum(6, 2, 52, 1), 1s);
    CHECK(queue.MarkDispatching(independent.operation, At(1s)).HasValue());
    CHECK(queue.RetryUncommitted(independent.operation, At(1s), 0, -1s).HasError());
    CHECK(queue.Query(IntentId(6)).Value().state == PlatformOfflineOperationState::Dispatching);
}

TEST_CASE("Retry jitter grows within the hard cap and rejects clock rollback", "[platform-services][offline][retry][clock]") {
    auto config = Config();
    config.progressionMaximumAge = 100s;
    config.initialRetryDelay = 2s;
    config.maximumRetryDelay = 5s;
    PlatformOfflineQueue queue(config);
    const auto admission = Admit(queue, AddStatOnce(7, 1), 0s);
    REQUIRE(queue.MarkDispatching(admission.operation, At(0s)).HasValue());
    REQUIRE(queue.RetryUncommitted(admission.operation, At(1s), 1000).HasValue());
    CHECK(queue.Query(IntentId(7)).Value().nextEligibleAt == At(4s));
    REQUIRE(queue.MarkDispatching(admission.operation, At(4s)).HasValue());
    CHECK(queue.RetryUncommitted(admission.operation, At(3s), 2).HasError());
    REQUIRE(queue.RetryUncommitted(admission.operation, At(4s), 2000).HasValue());
    CHECK(queue.Query(IntentId(7)).Value().nextEligibleAt == At(9s));
}

TEST_CASE("Offline diagnostics is a detached bounded aggregate across lifecycle transitions", "[platform-services][offline][diagnostics]") {
    PlatformOfflineQueue queue(Config());
    const auto first = Admit(queue, AddStatOnce(8, 1), 0s);
    const auto second = Admit(queue, StatMaximum(9, 2, 52, 7), 0s);
    REQUIRE(queue.MarkDispatching(first.operation, At(0s)).HasValue());
    const auto before = queue.Diagnostics();
    CHECK(before.active == 2);
    CHECK(before.retained == 2);
    CHECK(before.receiptsByState[static_cast<std::size_t>(PlatformOfflineIntentState::Dispatching)] == 1);
    CHECK(queue.SuspendAllPending() == 1);
    CHECK(queue.SuspendAllPending() == 0);
    CHECK(queue.Query(IntentId(8)).Value().state == PlatformOfflineOperationState::Dispatching);
    CHECK(queue.Query(IntentId(9)).Value().state == PlatformOfflineOperationState::Suspended);
    CHECK(before.receiptsByState[static_cast<std::size_t>(PlatformOfflineIntentState::Pending)] == 1);
    REQUIRE(queue.Shutdown(At(1s)).HasValue());
    const auto after = queue.Diagnostics();
    CHECK(after.closed);
    CHECK(after.receiptsByState[static_cast<std::size_t>(PlatformOfflineIntentState::Reconciling)] == 1);
    CHECK(after.receiptsByState[static_cast<std::size_t>(PlatformOfflineIntentState::Suspended)] == 1);
    CHECK_FALSE(before.closed);
    CHECK(queue.MarkDispatching(second.operation, At(1s)).HasError());
}

TEST_CASE("Partial expiry cannot rewrite an aggregate after its first attempt", "[platform-services][offline][retry][expiry]") {
    PlatformOfflineQueue queue(Config());
    const auto original = Admit(queue, StatMaximum(10, 1, 51, 9), 0s);
    const auto joined = Admit(queue, StatMaximum(11, 1, 51, 4), 1s);
    REQUIRE(joined.operation == original.operation);
    REQUIRE(queue.MarkDispatching(original.operation, At(1s)).HasValue());
    REQUIRE(queue.RetryUncommitted(original.operation, At(1s), 0).HasValue());
    const auto expired = queue.Expire(At(10s));
    REQUIRE(expired.HasValue());
    REQUIRE(expired.Value() == std::vector{IntentId(10)});
    CHECK(std::get<PlatformOfflineSetStatMaximum>(queue.Query(IntentId(11)).Value().operation).value == 9);
    CHECK(queue.MarkDispatching(original.operation, At(10s)).HasError());
    CHECK(queue.Query(IntentId(11)).Value().attempts == 1);
    const auto independent = Admit(queue, StatMaximum(12, 2, 52, 1), 10s);
    CHECK(queue.MarkDispatching(independent.operation, At(10s)).HasValue());
    const auto lastExpired = queue.Expire(At(11s));
    REQUIRE(lastExpired.HasValue());
    CHECK(lastExpired.Value() == std::vector{IntentId(11)});
    CHECK(queue.Query(IntentId(11)).Value().state == PlatformOfflineOperationState::Terminal);
}

TEST_CASE("First offline retry has subsecond jitter without a zero-delay path", "[platform-services][offline][retry]") {
    PlatformOfflineQueue queue(Config());
    const auto admission = Admit(queue, AddStatOnce(13, 1), 0s);
    REQUIRE(queue.MarkDispatching(admission.operation, At(0s)).HasValue());
    REQUIRE(queue.RetryUncommitted(admission.operation, At(0s), 500).HasValue());
    CHECK(queue.Query(IntentId(13)).Value().nextEligibleAt == At(0s) + 1500ms);
    CHECK(queue.MarkDispatching(admission.operation, At(1s)).HasError());
    CHECK(queue.MarkDispatching(admission.operation, At(0s) + 1500ms).HasValue());
}

TEST_CASE("Malformed offline retry limits fail before intent admission", "[platform-services][offline][retry][validation]") {
    for (const unsigned invalid : {0U, 1U, 2U, 3U, 4U}) {
        auto config = Config();
        if (invalid == 0)
            config.maximumAttempts = 0;
        else if (invalid == 1)
            config.maximumAttempts = PlatformOfflineQueueMaximumAttempts + 1;
        else if (invalid == 2)
            config.initialRetryDelay = 0s;
        else if (invalid == 3)
            config.maximumRetryDelay = config.initialRetryDelay - 1s;
        else
            config.maximumRetryDelay = PlatformOfflineQueueMaximumRetryDelay + 1s;
        PlatformOfflineQueue queue(config);
        CHECK(queue.Admit(AddStatOnce(14, 1), At(0s)).HasError());
        CHECK(queue.ActiveIntentCount() == 0);
    }
}
