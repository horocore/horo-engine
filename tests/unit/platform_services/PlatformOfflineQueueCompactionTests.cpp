#include "AllocationProbe.h"
#include "Horo/PlatformServices/PlatformOfflineQueue.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>

namespace {
    using namespace Horo;
    using namespace Horo::PlatformServices;
    using namespace std::chrono_literals;

    /** @brief Builds a stable producer identity for compaction assertions. */
    [[nodiscard]] PlatformOfflineIntentId Identity(const std::uint8_t value) {
        PlatformOfflineIntentId identity;
        identity.bytes.back() = static_cast<std::byte>(value);
        return identity;
    }

    /** @brief Maps test seconds onto the monotonic owner clock. */
    [[nodiscard]] PlatformOfflineQueue::TimePoint At(const std::chrono::seconds offset) {
        return PlatformOfflineQueue::TimePoint{} + offset;
    }

    /** @brief Admits an intent into a chosen semantic lane. */
    [[nodiscard]] PlatformOfflineOperationHandle Admit(PlatformOfflineQueue &queue, const std::uint8_t identity, const std::uint64_t stat,
                                                       const std::chrono::seconds at = 0s) {
        PlatformOfflineSubjectPartition subject;
        subject.bytes.back() = std::byte{1};
        const auto admission =
            queue.Admit({.id = Identity(identity),
                         .lane =
                             PlatformOfflineProgressionLane{.subject = subject, .target = StatId{stat}, .policy = {1}, .accessPolicy = {1}},
                         .operation = PlatformOfflineSetStatMaximum{.stat = StatId{stat}, .value = identity}},
                        At(at));
        REQUIRE(admission.HasValue());
        return admission.Value().operation;
    }

    /** @brief Creates interleaved removable, pending, ambiguous and recently terminal operations. */
    void Populate(PlatformOfflineQueue &queue) {
        REQUIRE(queue.CancelPending(Admit(queue, 1, 10), At(0s)).HasValue());
        PlatformOfflineSubjectPartition subject;
        subject.bytes.back() = std::byte{1};
        REQUIRE(queue
                    .Admit({.id = Identity(2),
                            .lane = PlatformOfflinePresenceLane{.subject = subject, .policy = {1}, .accessPolicy = {1}},
                            .operation = PlatformOfflinePresenceDesiredState{.action = PlatformOfflinePresenceDesiredState::Action::Set,
                                                                             .status = PresenceStatusId{1},
                                                                             .detail = std::string(32, 'p')}},
                           At(0s))
                    .HasValue());
        const auto coalesced = Admit(queue, 3, 20);
        REQUIRE(Admit(queue, 4, 20) == coalesced);
        REQUIRE(queue.CancelPending(coalesced, At(0s)).HasValue());
        const auto ambiguous = Admit(queue, 5, 30);
        REQUIRE(queue.MarkDispatching(ambiguous, At(0s)).HasValue());
        REQUIRE(queue.MarkReconciling(ambiguous).HasValue());
        REQUIRE(queue.CancelPending(Admit(queue, 6, 40, 1s), At(1s)).HasValue());
    }

    /** @brief Checks that allocation failure retained every original receipt and survivor payload. */
    void CheckUnchanged(const PlatformOfflineQueue &queue) {
        CHECK(queue.OperationCount() == 5);
        CHECK(queue.RetainedIntentCount() == 6);
        CHECK(queue.ActiveIntentCount() == 2);
        for (const auto id : {1, 3, 4, 6}) {
            const auto snapshot = queue.Query(Identity(static_cast<std::uint8_t>(id)));
            REQUIRE(snapshot.HasValue());
            CHECK(snapshot.Value().state == PlatformOfflineOperationState::Terminal);
            for (const auto &receipt : snapshot.Value().receipts)
                CHECK(receipt.state == PlatformOfflineIntentState::Abandoned);
        }
        CHECK(queue.Query(Identity(5)).Value().state == PlatformOfflineOperationState::Reconciling);
        const auto pending = queue.Query(Identity(2));
        REQUIRE(pending.HasValue());
        CHECK(pending.Value().state == PlatformOfflineOperationState::Pending);
        CHECK(std::get<PlatformOfflinePresenceDesiredState>(pending.Value().operation).detail == std::string(32, 'p'));
    }

    /** @brief Checks complete ordered publication, retained ambiguity and retention of the recent terminal receipt. */
    void CheckCompacted(const PlatformOfflineQueue &queue, const std::vector<PlatformOfflineIntentId> &ids) {
        CHECK(ids == std::vector{Identity(1), Identity(3), Identity(4)});
        CHECK(queue.OperationCount() == 3);
        CHECK(queue.RetainedIntentCount() == 3);
        CHECK(queue.ActiveIntentCount() == 2);
        for (const auto id : ids)
            CHECK(queue.Query(id).HasError());
        CHECK(queue.Query(Identity(2)).Value().state == PlatformOfflineOperationState::Pending);
        CHECK(queue.Query(Identity(5)).Value().state == PlatformOfflineOperationState::Reconciling);
        CHECK(queue.Query(Identity(6)).Value().receipts.front().state == PlatformOfflineIntentState::Abandoned);
    }

    /** @brief Selects finite bounds with a five-second terminal retention window. */
    [[nodiscard]] PlatformOfflineQueueConfig Config() {
        return {.activeCapacity = 16,
                .perSubjectActiveCapacity = 16,
                .retainedCapacity = 32,
                .progressionMaximumAge = 10s,
                .presenceMaximumAge = 10s,
                .terminalRetention = 5s,
                .producerRedeliveryHorizon = 4s,
                .maximumPresenceDetailBytes = 32,
                .generation = {7}};
    }
}  // namespace

TEST_CASE("Offline compaction preserves all receipts when allocation fails", "[platform-services][offline][compaction][allocation]") {
    std::size_t requests{};
    {
        PlatformOfflineQueue queue(Config());
        Populate(queue);
        Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
        const auto result = queue.Compact(At(5s));
        requests = measurement.Snapshot().requests;
        REQUIRE(result.HasValue());
        CheckCompacted(queue, result.Value());
    }
    REQUIRE(requests > 0);
    std::size_t failures{};
    for (std::size_t budget = 0; budget <= requests; ++budget) {
        PlatformOfflineQueue queue(Config());
        Populate(queue);
        try {
            const auto result = [&] {
                Horo::Tests::AllocationProbe::ScopedFailure failure{budget};
                return queue.Compact(At(5s));
            }();
            REQUIRE(result.HasValue());
            CHECK(budget == requests);
            CheckCompacted(queue, result.Value());
        } catch (const std::bad_alloc &) {
            ++failures;
            CheckUnchanged(queue);
            const auto retry = queue.Compact(At(5s));
            REQUIRE(retry.HasValue());
            CheckCompacted(queue, retry.Value());
        }
    }
    CHECK(failures == requests);
}
