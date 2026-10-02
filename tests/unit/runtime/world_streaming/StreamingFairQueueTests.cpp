#include "Horo/WorldStreaming/StreamingFairQueue.h"
#include "Horo/WorldStreaming/StreamingSchedulerAdmission.h"
#include "WorldStreamingTestUtils.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::RequireError;
        using namespace WorldStreamingErrors;
        constexpr auto Admissible = StreamingFairQueueEligibility::Admissible;
        constexpr auto Deferred = StreamingFairQueueEligibility::Deferred;

        StreamingPriorityPolicy Policy() {
            return StreamingPriorityPolicy::Create({.id = IdentityFrom<StreamingPriorityPolicyId>(1),
                                                    .revision = IdentityFrom<StreamingPriorityPolicyRevision>(1),
                                                    .maximumCandidates = 8})
                .Value();
        }

        StreamingFairQueueRequest Request(const std::uint32_t capacity = 8, const std::uint32_t burst = 3) {
            return {.id = IdentityFrom<StreamingFairQueueId>(41),
                    .partition = TestSupport::World(),
                    .epoch = IdentityFrom<PartitionEpoch>(1),
                    .maximumEntries = capacity,
                    .maximumPriorityDispatches = burst};
        }

        StreamingFairQueue Queue(const std::uint32_t capacity = 8, const std::uint32_t burst = 3) {
            auto queue = StreamingFairQueue::Create(Request(capacity, burst), Policy());
            REQUIRE(queue.HasValue());
            return std::move(queue).Value();
        }

        StreamingFairQueueContext Context(const StreamingFairQueue &queue, const std::uint64_t time = 0) {
            return {IdentityFrom<StreamingFairQueueId>(41), queue.Revision(), time};
        }

        StreamingFairQueueEntry Entry(const std::uint64_t id, const float priority = 1.0F, const std::uint64_t generation = 1) {
            const StreamingCellId cell{static_cast<std::int32_t>(id), 0, 0, 0, TestSupport::Layer(0)};
            return {.operation = {IdentityFrom<StreamingCellOperationId>(id),
                                  {TestSupport::World(), IdentityFrom<PartitionEpoch>(1), cell,
                                   IdentityFrom<StreamingGeneration>(generation)}},
                    .priority = {.source = {.id = IdentityFrom<StreamingSourceId>(id),
                                            .owner = TestSupport::Owner(),
                                            .intent = StreamingSourceIntent::Camera,
                                            .priority = StreamingSourcePriority::Create(priority).Value(),
                                            .revision = IdentityFrom<StreamingSourceRevision>(1)},
                                 .cell = cell,
                                 .distanceMillimeters = 0,
                                 .queuedAtServiceMilliseconds = 999'999}};
        }

        StreamingFairQueueSelection Select(StreamingFairQueue &queue, const std::span<const StreamingFairQueueAdmission> rows,
                                           const std::uint64_t time = 0) {
            const auto result = queue.Select(Context(queue, time), rows);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().has_value());
            return *result.Value();
        }

        void Commit(StreamingFairQueue &queue, const StreamingFairQueueSelection &selection, const std::uint64_t time = 0) {
            REQUIRE(queue.CommitDispatch(Context(queue, time), selection).HasValue());
        }

        struct PendingProposalFixture final {
            StreamingFairQueue queue{Queue()};
            StreamingFairQueueEntry entry{Entry(1)};
            std::array<StreamingFairQueueAdmission, 1> rows{{{entry.operation, Admissible}}};
            StreamingFairQueueSelection proposal{};

            PendingProposalFixture() {
                REQUIRE(queue.Enqueue(Context(queue), entry).HasValue());
                proposal = Select(queue, rows);
            }
        };
    }  // namespace

    TEST_CASE("Fair queue preserves urgent score ranking and dispatches old work under sustained arrivals",
              "[unit][world_streaming][fair_queue]") {
        auto queue = Queue();
        const auto old = Entry(1, 0.0F);
        REQUIRE(queue.Enqueue(Context(queue), old).HasValue());
        // Wait far past the numerical age cap: score-only ranking would still starve this cell.
        for (std::uint64_t round = 0; round < 4; ++round) {
            const auto urgent = Entry(10 + round, 1'000.0F);
            const auto time = 100'000 + round;
            REQUIRE(queue.Enqueue(Context(queue, time), urgent).HasValue());
            const std::array rows{StreamingFairQueueAdmission{urgent.operation, Admissible},
                                  StreamingFairQueueAdmission{old.operation, Admissible}};
            const auto proposal = Select(queue, rows, time);
            CHECK(proposal.operation == (round == 3 ? old.operation : urgent.operation));
            CHECK(proposal.reason ==
                  (round == 3 ? StreamingFairQueueDispatchReason::OldestAdmissible : StreamingFairQueueDispatchReason::Priority));
            CHECK(queue.Size() == 2);
            Commit(queue, proposal, time);
        }
        CHECK(queue.Size() == 1);
        CHECK(queue.PendingEntries()[0].priority.queuedAtServiceMilliseconds == 100'003);
    }

    TEST_CASE("Every continuously admissible waiter progresses within the bounded fair service window",
              "[unit][world_streaming][fair_queue]") {
        auto queue = Queue();
        std::vector<StreamingFairQueueAdmission> rows;
        for (std::uint64_t id = 1; id <= 3; ++id) {
            const auto waiter = Entry(id, 0.0F);
            REQUIRE(queue.Enqueue(Context(queue), waiter).HasValue());
            rows.push_back({waiter.operation, Admissible});
        }
        std::uint64_t completedWaiters = 0;
        for (std::uint64_t dispatch = 0; dispatch < 12; ++dispatch) {
            const auto urgent = Entry(100 + dispatch, 1'000.0F);
            REQUIRE(queue.Enqueue(Context(queue, dispatch), urgent).HasValue());
            rows.push_back({urgent.operation, Admissible});
            const auto proposal = Select(queue, rows, dispatch);
            if (proposal.reason == StreamingFairQueueDispatchReason::OldestAdmissible) {
                ++completedWaiters;
                CHECK(proposal.operation.operation.Value() == completedWaiters);
            }
            Commit(queue, proposal, dispatch);
            std::erase_if(rows, [&](const auto &row) {
                return row.operation == proposal.operation;
            });
            // Unselected urgent work remains pending: new arrivals do not overtake old fair entries.
        }
        CHECK(completedWaiters == 3);
    }

    TEST_CASE("Fair queue failed admission and deferred snapshots spend no fairness credit", "[unit][world_streaming][fair_queue]") {
        auto queue = Queue(8, 1);
        const auto old = Entry(1, 0.0F);
        const auto urgent = Entry(2, 100.0F);
        REQUIRE(queue.Enqueue(Context(queue), old).HasValue());
        REQUIRE(queue.Enqueue(Context(queue), urgent).HasValue());
        const std::array eligible{StreamingFairQueueAdmission{old.operation, Admissible},
                                  StreamingFairQueueAdmission{urgent.operation, Admissible}};
        auto proposal = Select(queue, eligible);
        for (int retry = 0; retry < 5; ++retry) {
            const auto superseded = proposal;
            proposal = Select(queue, eligible);
            CHECK(proposal.operation == urgent.operation);
            RequireError(queue.CommitDispatch(Context(queue), superseded), FairQueueStale);
        }
        const std::array deferred{StreamingFairQueueAdmission{old.operation, Deferred},
                                  StreamingFairQueueAdmission{urgent.operation, Deferred}};
        const auto empty = queue.Select(Context(queue), deferred);
        REQUIRE(empty.HasValue());
        CHECK_FALSE(empty.Value());
        RequireError(queue.CommitDispatch(Context(queue), proposal), FairQueueStale);
        Commit(queue, Select(queue, eligible));
        const auto nextUrgent = Entry(3, 100.0F);
        REQUIRE(queue.Enqueue(Context(queue), nextUrgent).HasValue());
        const std::array next{StreamingFairQueueAdmission{old.operation, Admissible},
                              StreamingFairQueueAdmission{nextUrgent.operation, Admissible}};
        proposal = Select(queue, next);
        CHECK(proposal.operation == old.operation);
        CHECK(proposal.reason == StreamingFairQueueDispatchReason::OldestAdmissible);
    }

    TEST_CASE("Fair slots skip infeasible cells without bypassing eligibility", "[unit][world_streaming][fair_queue]") {
        auto queue = Queue(8, 1);
        const auto denied = Entry(1);
        const auto waiting = Entry(2, 0.0F);
        const auto urgent = Entry(3, 100.0F);
        for (const auto &entry : {denied, waiting, urgent})
            REQUIRE(queue.Enqueue(Context(queue), entry).HasValue());
        const std::array first{StreamingFairQueueAdmission{denied.operation, Deferred},
                               StreamingFairQueueAdmission{waiting.operation, Admissible},
                               StreamingFairQueueAdmission{urgent.operation, Admissible}};
        Commit(queue, Select(queue, first));
        const std::array second{StreamingFairQueueAdmission{denied.operation, Deferred},
                                StreamingFairQueueAdmission{waiting.operation, Admissible}};
        const auto proposal = Select(queue, second);
        CHECK(proposal.operation == waiting.operation);
        CHECK(proposal.reason == StreamingFairQueueDispatchReason::OldestAdmissible);
        Commit(queue, proposal);
        CHECK(queue.Size() == 1);
    }

    TEST_CASE("Same-time fair order preserves first arrival through successor replacement", "[unit][world_streaming][fair_queue]") {
        auto queue = Queue(8, 1);
        const auto old = Entry(8, 0.0F);
        const auto younger = Entry(1, 0.0F);
        const auto urgent = Entry(2, 100.0F);
        for (const auto &entry : {old, younger, urgent})
            REQUIRE(queue.Enqueue(Context(queue), entry).HasValue());
        auto replacement = old;
        replacement.operation.operation = IdentityFrom<StreamingCellOperationId>(99);
        replacement.operation.fence.generation = IdentityFrom<StreamingGeneration>(2);
        REQUIRE(queue.Replace(Context(queue, 10), old.operation, replacement).HasValue());
        CHECK(queue.PendingEntries()[0].priority.queuedAtServiceMilliseconds == 0);
        RequireError(queue.Discard(Context(queue, 10), old.operation, StreamingCellOperationOutcome::Cancelled), FairQueueStale);
        const std::array first{StreamingFairQueueAdmission{younger.operation, Admissible},
                               StreamingFairQueueAdmission{urgent.operation, Admissible},
                               StreamingFairQueueAdmission{replacement.operation, Admissible}};
        Commit(queue, Select(queue, first, 10), 10);
        const std::array second{StreamingFairQueueAdmission{younger.operation, Admissible},
                                StreamingFairQueueAdmission{replacement.operation, Admissible}};
        CHECK(Select(queue, second, 10).operation == replacement.operation);
    }

    TEST_CASE("Priority ties use canonical cells independently of queue and snapshot order", "[unit][world_streaming][fair_queue]") {
        for (const bool reverse : {false, true}) {
            auto queue = Queue();
            const auto first = Entry(1);
            const auto second = Entry(2);
            REQUIRE(queue.Enqueue(Context(queue), reverse ? second : first).HasValue());
            REQUIRE(queue.Enqueue(Context(queue), reverse ? first : second).HasValue());
            const std::array rows{StreamingFairQueueAdmission{second.operation, Admissible},
                                  StreamingFairQueueAdmission{first.operation, Admissible}};
            CHECK(Select(queue, rows).operation == first.operation);
        }
    }

    TEST_CASE("Fair queue admission snapshot errors preserve a valid proposal atomically", "[unit][world_streaming][fair_queue]") {
        auto queue = Queue(2);
        const auto first = Entry(1);
        const auto second = Entry(2);
        REQUIRE(queue.Enqueue(Context(queue), first).HasValue());
        REQUIRE(queue.Enqueue(Context(queue), second).HasValue());
        const std::array valid{StreamingFairQueueAdmission{first.operation, Admissible},
                               StreamingFairQueueAdmission{second.operation, Deferred}};
        const auto proposal = Select(queue, valid);
        const auto revision = queue.Revision();
        SECTION("missing") {
            RequireError(queue.Select(Context(queue), std::span(valid).first(1)), FairQueueInvalid);
        }
        SECTION("duplicate") {
            auto rows = valid;
            rows[1] = rows[0];
            RequireError(queue.Select(Context(queue), rows), FairQueueIdentityConflict);
        }
        SECTION("stale generation") {
            auto rows = valid;
            rows[1].operation.fence.generation = IdentityFrom<StreamingGeneration>(2);
            RequireError(queue.Select(Context(queue), rows), FairQueueStale);
        }
        SECTION("invalid identity") {
            auto rows = valid;
            rows[1].operation.operation = {};
            RequireError(queue.Select(Context(queue), rows), FairQueueInvalid);
        }
        SECTION("unsupported eligibility") {
            auto rows = valid;
            rows[1].eligibility = static_cast<StreamingFairQueueEligibility>(255);
            RequireError(queue.Select(Context(queue), rows), FairQueueUnsupported);
        }
        SECTION("overcapacity") {
            const std::array rows{valid[0], valid[1], valid[0]};
            RequireError(queue.Select(Context(queue), rows), FairQueueCapacityExceeded);
        }
        CHECK(queue.Revision() == revision);
        CHECK(queue.Size() == 2);
        Commit(queue, proposal);
    }

    TEST_CASE("Fair queue rejects malformed conflicting and overcapacity enqueue without mutation", "[unit][world_streaming][fair_queue]") {
        auto queue = Queue(1);
        auto entry = Entry(1);
        const auto revision = queue.Revision();
        SECTION("mismatched cell") {
            entry.priority.cell.x = 20;
            RequireError(queue.Enqueue(Context(queue), entry), FairQueueInvalid);
        }
        SECTION("foreign partition") {
            entry.operation.fence.partition = TestSupport::World(2);
            RequireError(queue.Enqueue(Context(queue), entry), FairQueueStale);
        }
        SECTION("foreign source incarnation") {
            entry.priority.source.owner.epoch = IdentityFrom<PartitionEpoch>(2);
            RequireError(queue.Enqueue(Context(queue), entry), PriorityPolicyStale);
        }
        SECTION("nan priority override") {
            entry.priority.priorityOverride = std::numeric_limits<double>::quiet_NaN();
            RequireError(queue.Enqueue(Context(queue), entry), PriorityPolicyInvalid);
        }
        CHECK(queue.Revision() == revision);
        CHECK(queue.Size() == 0);
        REQUIRE(queue.Enqueue(Context(queue), Entry(1)).HasValue());
        const auto fullRevision = queue.Revision();
        RequireError(queue.Enqueue(Context(queue), Entry(1)), FairQueueIdentityConflict);
        RequireError(queue.Enqueue(Context(queue), Entry(2)), FairQueueCapacityExceeded);
        CHECK(queue.Revision() == fullRevision);
    }

    TEST_CASE("Fair queue owner revision and monotonic service time fence every command", "[unit][world_streaming][fair_queue]") {
        auto queue = Queue();
        const auto stale = Context(queue, 10);
        const auto entry = Entry(1);
        REQUIRE(queue.Enqueue(stale, entry).HasValue());
        RequireError(queue.Discard(stale, entry.operation, StreamingCellOperationOutcome::Cancelled), FairQueueStale);
        auto wrong = Context(queue, 10);
        wrong.owner = IdentityFrom<StreamingFairQueueId>(42);
        RequireError(queue.Shutdown(wrong), FairQueueStale);
        RequireError(queue.Enqueue(Context(queue, 9), Entry(2)), FairQueueStale);
        auto invalid = Context(queue, 10);
        invalid.revision = {};
        RequireError(queue.Shutdown(invalid), FairQueueInvalid);
        CHECK(queue.Size() == 1);
    }

    TEST_CASE("Queued cancellation failure and replacement withdrawal invalidate proposals", "[unit][world_streaming][fair_queue]") {
        for (const auto outcome :
             {StreamingCellOperationOutcome::Cancelled, StreamingCellOperationOutcome::Failed, StreamingCellOperationOutcome::Replaced}) {
            auto [queue, entry, rows, proposal] = PendingProposalFixture{};
            const auto revision = queue.Revision();
            RequireError(queue.Discard(Context(queue), entry.operation, StreamingCellOperationOutcome::Succeeded), FairQueueUnsupported);
            CHECK(queue.Revision() == revision);
            REQUIRE(queue.Discard(Context(queue), entry.operation, outcome).HasValue());
            CHECK(queue.Size() == 0);
            RequireError(queue.CommitDispatch(Context(queue), proposal), FairQueueStale);
            RequireError(queue.Discard(Context(queue), entry.operation, outcome), FairQueueStale);
        }
    }

    TEST_CASE("Forged dispatch proposals cannot remove queued work or alter fairness credit", "[unit][world_streaming][fair_queue]") {
        auto [queue, entry, rows, proposal] = PendingProposalFixture{};
        auto forged = proposal;
        SECTION("reason") {
            forged.reason = StreamingFairQueueDispatchReason::OldestAdmissible;
        }
        SECTION("operation") {
            forged.operation.operation = IdentityFrom<StreamingCellOperationId>(2);
        }
        SECTION("owner") {
            forged.owner = IdentityFrom<StreamingFairQueueId>(42);
        }
        const auto revision = queue.Revision();
        RequireError(queue.CommitDispatch(Context(queue), forged), FairQueueStale);
        CHECK(queue.Revision() == revision);
        CHECK(queue.Size() == 1);
        Commit(queue, proposal);
        RequireError(queue.CommitDispatch(Context(queue), proposal), FairQueueStale);
    }

    TEST_CASE("Fair queue replacement failures leave original work and proposal current", "[unit][world_streaming][fair_queue]") {
        auto [queue, entry, rows, proposal] = PendingProposalFixture{};
        const auto revision = queue.Revision();
        auto replacement = entry;
        replacement.operation.operation = IdentityFrom<StreamingCellOperationId>(2);
        RequireError(queue.Replace(Context(queue), entry.operation, replacement), FairQueueStale);
        replacement.operation.fence.generation = IdentityFrom<StreamingGeneration>(2);
        replacement.priority.cell.x = 2;
        RequireError(queue.Replace(Context(queue), entry.operation, replacement), FairQueueInvalid);
        CHECK(queue.Revision() == revision);
        Commit(queue, proposal);
    }

    TEST_CASE("Fair queue shutdown is terminal idempotent and revokes pending admission", "[unit][world_streaming][fair_queue]") {
        auto [queue, entry, rows, proposal] = PendingProposalFixture{};
        REQUIRE(queue.Shutdown(Context(queue)).HasValue());
        CHECK(queue.IsClosed());
        CHECK(queue.Size() == 0);
        const auto revision = queue.Revision();
        REQUIRE(queue.Shutdown(Context(queue)).HasValue());
        CHECK(queue.Revision() == revision);
        RequireError(queue.Enqueue(Context(queue), entry), FairQueueLifecycleUnavailable);
        RequireError(queue.Select(Context(queue), {}), FairQueueLifecycleUnavailable);
        RequireError(queue.CommitDispatch(Context(queue), proposal), FairQueueLifecycleUnavailable);
    }

    TEST_CASE("Fair queue creation rejects unsupported and unbounded policy inputs", "[unit][world_streaming][fair_queue]") {
        auto request = Request();
        SECTION("version") {
            request.contractVersion = 2;
            RequireError(StreamingFairQueue::Create(request, Policy()), FairQueueUnsupported);
        }
        SECTION("owner") {
            request.id = {};
            RequireError(StreamingFairQueue::Create(request, Policy()), FairQueueInvalid);
        }
        SECTION("capacity zero") {
            request.maximumEntries = 0;
            RequireError(StreamingFairQueue::Create(request, Policy()), FairQueueInvalid);
        }
        SECTION("policy ceiling") {
            request.maximumEntries = 9;
            RequireError(StreamingFairQueue::Create(request, Policy()), FairQueueCapacityExceeded);
        }
        SECTION("burst zero") {
            request.maximumPriorityDispatches = 0;
            RequireError(StreamingFairQueue::Create(request, Policy()), FairQueueInvalid);
        }
        SECTION("burst excessive") {
            request.maximumPriorityDispatches = 1'025;
            RequireError(StreamingFairQueue::Create(request, Policy()), FairQueueCapacityExceeded);
        }
    }

    TEST_CASE("Fair queue proposal composes with atomic scheduler reservation and retains denied work",
              "[unit][world_streaming][fair_queue]") {
        // Queue proposals own no admitted resources; cancellation after ledger admission remains ledger-owned.
        auto [queue, entry, rows, proposal] = PendingProposalFixture{};
        auto operation = StreamingCellOperation::Create(entry.operation, StreamingCellOperationKind::Load).Value();
        auto ledgerResult = StreamingSchedulerAdmissionLedger::Create(IdentityFrom<StreamingSchedulerLedgerId>(1), {1, 1});
        REQUIRE(ledgerResult.HasValue());
        auto ledger = std::move(ledgerResult).Value();
        RequireError(ledger.TryAdmit(operation, 2), SchedulerCapacityExceeded);
        CHECK(queue.Size() == 1);
        const auto admitted = ledger.TryAdmit(operation, 1);
        REQUIRE(admitted.HasValue());
        Commit(queue, proposal);
        CHECK(queue.Size() == 0);
        REQUIRE(queue.Shutdown(Context(queue)).HasValue());
        CHECK(ledger.ReservedCount() == 1);
        ledger.BeginShutdown();
        REQUIRE(ledger.Advance(admitted.Value(), StreamingCellOperationTransition::Shutdown).HasValue());
        RequireError(ledger.Release(admitted.Value()), SchedulerLifecycleUnavailable);
        REQUIRE(ledger.Advance(admitted.Value(), StreamingCellOperationTransition::AcknowledgeRetirement).HasValue());
        REQUIRE(ledger.Release(admitted.Value()).HasValue());
        CHECK(ledger.ReservedCount() == 0);
    }
}  // namespace Horo::WorldStreaming
