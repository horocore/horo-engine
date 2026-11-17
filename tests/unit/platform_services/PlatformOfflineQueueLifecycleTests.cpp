#include "AllocationProbe.h"
#include "Horo/PlatformServices/PlatformOfflineQueue.h"
#include "Horo/PlatformServices/PlatformOfflineQueueErrors.h"

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
