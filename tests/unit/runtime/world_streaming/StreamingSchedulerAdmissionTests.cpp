#include "Horo/WorldStreaming/StreamingSchedulerAdmission.h"
#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "WorldStreamingTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <type_traits>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::Layer;
        using TestSupport::RequireError;
        using TestSupport::World;

        [[nodiscard]] StreamingCellOperation QueuedOperation(const std::uint64_t operation, const std::uint64_t generation = 1,
                                                             const StreamingCellOperationKind kind = StreamingCellOperationKind::Activate) {
            return StreamingCellOperation::Create(
                       {
                           .operation = IdentityFrom<StreamingCellOperationId>(operation),
                           .fence =
                               {
                                   .partition = World(),
                                   .epoch = IdentityFrom<PartitionEpoch>(1),
                                   .cell = {1, 2, 3, 0, Layer()},
                                   .generation = IdentityFrom<StreamingGeneration>(generation),
                               },
                       },
                       kind)
                .Value();
        }

        [[nodiscard]] StreamingConcurrencyPolicy Policy(const std::uint32_t limit = 2, const std::uint64_t revision = 1,
                                                        const WorldPartitionProjectProfile profile = WorldPartitionProjectProfile::Editor) {
            return {.profile = profile,
                    .revision = IdentityFrom<StreamingConcurrencyRevision>(revision),
                    .loads = limit,
                    .activations = limit,
                    .retirements = limit};
        }

        [[nodiscard]] StreamingSchedulerAdmissionLedger CreateLedger(const std::uint64_t owner = 1,
                                                                     const std::uint32_t concurrentOperations = 2,
                                                                     const std::uint64_t capacityUnits = 10) {
            auto result = StreamingSchedulerAdmissionLedger::Create(IdentityFrom<StreamingSchedulerLedgerId>(owner),
                                                                    {.concurrentOperations = concurrentOperations,
                                                                     .capacityUnits = capacityUnits,
                                                                     .concurrency = Policy(concurrentOperations)});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        void CompleteActivation(StreamingSchedulerAdmissionLedger &ledger, const StreamingSchedulerReservation &reservation) {
            REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::BeginPreparation).HasValue());
            REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::BeginActivation).HasValue());
            REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::Complete).HasValue());
        }

        void Retire(StreamingSchedulerAdmissionLedger &ledger, const StreamingSchedulerReservation &reservation,
                    const StreamingCellOperationTransition reason) {
            REQUIRE(ledger.Advance(reservation, reason).HasValue());
            REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::AcknowledgeRetirement).HasValue());
        }

        static_assert(!std::is_copy_constructible_v<StreamingSchedulerAdmissionLedger>);
        static_assert(!std::is_copy_assignable_v<StreamingSchedulerAdmissionLedger>);

        TEST_CASE("Scheduler admission owns canonical state and exact generic capacity", "[unit][world_streaming][scheduler]") {
            auto ledger = CreateLedger();
            const auto queued = QueuedOperation(10);
            const auto reservation = ledger.TryAdmit(queued, 4, IdentityFrom<StreamingConcurrencyRevision>(1)).Value();

            REQUIRE(ledger.Owner() == IdentityFrom<StreamingSchedulerLedgerId>(1));
            REQUIRE(ledger.Limits() ==
                    StreamingSchedulerAdmissionLimits{.concurrentOperations = 2, .capacityUnits = 10, .concurrency = Policy()});
            REQUIRE(ledger.ReservedCount() == 1);
            REQUIRE(ledger.ReservedCapacityUnits() == 4);
            REQUIRE(queued.State() == StreamingCellOperationState::Queued);
            REQUIRE(reservation.operation == queued.Handle());

            CompleteActivation(ledger, reservation);
            REQUIRE(ledger.Release(reservation).HasValue());
            REQUIRE(ledger.ReservedCount() == 0);
            REQUIRE(ledger.ReservedCapacityUnits() == 0);
        }

        TEST_CASE("Scheduler rejection never partially reserves or admits", "[unit][world_streaming][scheduler]") {
            RequireError(StreamingSchedulerAdmissionLedger::Create({}, {.concurrentOperations = 1,
                                                                        .capacityUnits = 1,
                                                                        .concurrency = Policy(1)}),
                         WorldStreamingErrors::SchedulerAdmissionInvalid);
            RequireError(StreamingSchedulerAdmissionLedger::Create(IdentityFrom<StreamingSchedulerLedgerId>(1), {}),
                         WorldStreamingErrors::SchedulerAdmissionInvalid);
            RequireError(StreamingSchedulerAdmissionLedger::Create(IdentityFrom<StreamingSchedulerLedgerId>(1),
                                                                   {.concurrentOperations =
                                                                        StreamingSchedulerAdmissionLimits::MaximumConcurrentOperations + 1,
                                                                    .capacityUnits = 1,
                                                                    .concurrency = Policy(1)}),
                         WorldStreamingErrors::SchedulerAdmissionInvalid);

            auto ledger = CreateLedger();
            const auto first = ledger.TryAdmit(QueuedOperation(10), 6, IdentityFrom<StreamingConcurrencyRevision>(1)).Value();
            const auto rejected = QueuedOperation(11);
            RequireError(ledger.TryAdmit(rejected, 5, IdentityFrom<StreamingConcurrencyRevision>(1)),
                         WorldStreamingErrors::SchedulerCapacityExceeded);
            RequireError(ledger.TryAdmit(QueuedOperation(10), 1, IdentityFrom<StreamingConcurrencyRevision>(1)),
                         WorldStreamingErrors::SchedulerReservationConflict);
            RequireError(ledger.TryAdmit(rejected, 0, IdentityFrom<StreamingConcurrencyRevision>(1)),
                         WorldStreamingErrors::SchedulerAdmissionInvalid);
            REQUIRE(ledger.ReservedCount() == 1);
            REQUIRE(ledger.ReservedCapacityUnits() == 6);
            REQUIRE(rejected.State() == StreamingCellOperationState::Queued);

            CompleteActivation(ledger, first);
            REQUIRE(ledger.Release(first).HasValue());
        }

        TEST_CASE("Scheduler enforces operation and generic-capacity ceilings independently", "[unit][world_streaming][scheduler]") {
            auto ledger = CreateLedger(1, 2, 10);
            const auto first = ledger.TryAdmit(QueuedOperation(10), 6, IdentityFrom<StreamingConcurrencyRevision>(1)).Value();
            const auto second = ledger.TryAdmit(QueuedOperation(11), 4, IdentityFrom<StreamingConcurrencyRevision>(1)).Value();
            RequireError(ledger.TryAdmit(QueuedOperation(12), 1, IdentityFrom<StreamingConcurrencyRevision>(1)),
                         WorldStreamingErrors::SchedulerCapacityExceeded);
            REQUIRE(ledger.ReservedCount() == 2);
            REQUIRE(ledger.ReservedCapacityUnits() == 10);

            CompleteActivation(ledger, first);
            CompleteActivation(ledger, second);
            REQUIRE(ledger.Release(first).HasValue());
            REQUIRE(ledger.Release(second).HasValue());
        }

        TEST_CASE("Caller-held snapshots cannot forge canonical completion", "[unit][world_streaming][scheduler][fence]") {
            auto ledger = CreateLedger();
            const auto queued = QueuedOperation(10);
            const auto reservation = ledger.TryAdmit(queued, 3, IdentityFrom<StreamingConcurrencyRevision>(1)).Value();
            const auto forgedTerminal = queued.Advance(queued.Handle(), StreamingCellOperationTransition::Cancel).Value();
            REQUIRE(forgedTerminal.IsTerminal());

            RequireError(ledger.Release(reservation), WorldStreamingErrors::SchedulerLifecycleUnavailable);
            Retire(ledger, reservation, StreamingCellOperationTransition::Cancel);
            REQUIRE(ledger.Release(reservation).HasValue());
        }

        TEST_CASE("Scheduler rejects cross-owner stale and malformed reservations", "[unit][world_streaming][scheduler][fence]") {
            auto firstLedger = CreateLedger(1);
            auto secondLedger = CreateLedger(2);
            const auto first = firstLedger.TryAdmit(QueuedOperation(10), 2, IdentityFrom<StreamingConcurrencyRevision>(1)).Value();
            const auto second = secondLedger.TryAdmit(QueuedOperation(11), 2, IdentityFrom<StreamingConcurrencyRevision>(1)).Value();

            RequireError(secondLedger.Advance(first, StreamingCellOperationTransition::Cancel),
                         WorldStreamingErrors::SchedulerReservationStale);
            RequireError(secondLedger.Release(first), WorldStreamingErrors::SchedulerReservationStale);
            RequireError(firstLedger.Release({}), WorldStreamingErrors::SchedulerAdmissionInvalid);
            auto forged = first;
            forged.capacityUnits += 1;
            RequireError(firstLedger.Release(forged), WorldStreamingErrors::SchedulerReservationStale);
            REQUIRE(firstLedger.ReservedCount() == 1);
            REQUIRE(secondLedger.ReservedCount() == 1);

            Retire(firstLedger, first, StreamingCellOperationTransition::Cancel);
            Retire(secondLedger, second, StreamingCellOperationTransition::Cancel);
            REQUIRE(firstLedger.Release(first).HasValue());
            REQUIRE(secondLedger.Release(second).HasValue());
        }

        TEST_CASE("Scheduler retains interrupted work through retirement acknowledgement",
                  "[unit][world_streaming][scheduler][retirement]") {
            for (const auto reason : {StreamingCellOperationTransition::Cancel, StreamingCellOperationTransition::Fail,
                                      StreamingCellOperationTransition::Replace}) {
                auto ledger = CreateLedger(1, 1, 1);
                const auto reservation = ledger.TryAdmit(QueuedOperation(10), 1, IdentityFrom<StreamingConcurrencyRevision>(1)).Value();
                REQUIRE(ledger.Advance(reservation, reason).HasValue());
                RequireError(ledger.Release(reservation), WorldStreamingErrors::SchedulerLifecycleUnavailable);
                REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::AcknowledgeRetirement).HasValue());
                REQUIRE(ledger.Release(reservation).HasValue());
            }
        }

        TEST_CASE("Retirement operations use their normal admitted lifecycle", "[unit][world_streaming][scheduler][retirement]") {
            auto ledger = CreateLedger();
            const auto reservation =
                ledger
                    .TryAdmit(QueuedOperation(10, 1, StreamingCellOperationKind::Retire), 2, IdentityFrom<StreamingConcurrencyRevision>(1))
                    .Value();
            REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::BeginRetirement).HasValue());
            REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::AcknowledgeRetirement).HasValue());
            REQUIRE(ledger.Release(reservation).HasValue());
        }

        TEST_CASE("Profile stage ceilings independently bound retained work", "[unit][world_streaming][scheduler][concurrency]") {
            for (const auto profile : {WorldPartitionProjectProfile::Editor, WorldPartitionProjectProfile::Standalone,
                                       WorldPartitionProjectProfile::Client, WorldPartitionProjectProfile::Server}) {
                const StreamingConcurrencyPolicy policy{.profile = profile,
                                                        .revision = IdentityFrom<StreamingConcurrencyRevision>(1),
                                                        .loads = 1,
                                                        .activations = 2,
                                                        .retirements = 3};
                auto ledger =
                    StreamingSchedulerAdmissionLedger::Create(IdentityFrom<StreamingSchedulerLedgerId>(1),
                                                              {.concurrentOperations = 7, .capacityUnits = 10, .concurrency = policy})
                        .Value();
                std::uint64_t next = 1;
                std::array<StreamingSchedulerReservation, 6> reservations{};
                std::size_t index = 0;
                const std::array stages{std::pair{StreamingCellOperationKind::Load, 1U},
                                        std::pair{StreamingCellOperationKind::Activate, 2U},
                                        std::pair{StreamingCellOperationKind::Retire, 3U}};
                for (const auto [kind, ceiling] : stages) {
                    for (std::uint32_t count = 0; count < ceiling; ++count) {
                        const auto reservation = ledger.TryAdmit(QueuedOperation(next++, 1, kind), 1, policy.revision).Value();
                        reservations[index++] = reservation;
                        REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::Cancel).HasValue());
                        REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::AcknowledgeRetirement).HasValue());
                    }
                    REQUIRE(ledger.ReservedCount(kind).Value() == ceiling);
                    RequireError(ledger.TryAdmit(QueuedOperation(next++, 1, kind), 1, policy.revision),
                                 WorldStreamingErrors::SchedulerCapacityExceeded);
                }
                REQUIRE(ledger.ReservedCount() == 6);
                REQUIRE(ledger.ReservedCapacityUnits() == 6);
                REQUIRE(ledger.Limits().concurrency.profile == profile);
                for (const auto &reservation : reservations)
                    REQUIRE(ledger.Release(reservation).HasValue());
                REQUIRE(ledger.ReservedCount() == 0);
            }
        }

        TEST_CASE("Successful stage work releases only after its canonical terminal result",
                  "[unit][world_streaming][scheduler][concurrency]") {
            for (const auto kind :
                 {StreamingCellOperationKind::Load, StreamingCellOperationKind::Activate, StreamingCellOperationKind::Retire}) {
                auto ledger = CreateLedger();
                const auto revision = IdentityFrom<StreamingConcurrencyRevision>(1);
                const auto reservation = ledger.TryAdmit(QueuedOperation(1, 1, kind), 1, revision).Value();
                RequireError(ledger.Release(reservation), WorldStreamingErrors::SchedulerLifecycleUnavailable);
                if (kind == StreamingCellOperationKind::Retire) {
                    REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::BeginRetirement).HasValue());
                    REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::AcknowledgeRetirement).Value().Outcome() ==
                            StreamingCellOperationOutcome::Succeeded);
                } else {
                    REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::BeginPreparation).HasValue());
                    if (kind == StreamingCellOperationKind::Activate)
                        REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::BeginActivation).HasValue());
                    REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::Complete).Value().Outcome() ==
                            StreamingCellOperationOutcome::Succeeded);
                }
                REQUIRE(ledger.ReservedCount(kind).Value() == 1);
                REQUIRE(ledger.Release(reservation).HasValue());
                REQUIRE(ledger.ReservedCount(kind).Value() == 0);
            }
        }

        TEST_CASE("Stage slots survive every interrupted phase until exact release", "[unit][world_streaming][scheduler][concurrency]") {
            for (const auto reason : {StreamingCellOperationTransition::Cancel, StreamingCellOperationTransition::Fail,
                                      StreamingCellOperationTransition::Replace, StreamingCellOperationTransition::Shutdown}) {
                for (const auto kind :
                     {StreamingCellOperationKind::Load, StreamingCellOperationKind::Activate, StreamingCellOperationKind::Retire}) {
                    auto ledger = CreateLedger(1, 3, 10);
                    auto policy = Policy(1, 2);
                    REQUIRE(ledger.ReplaceConcurrency(IdentityFrom<StreamingConcurrencyRevision>(1), policy).HasValue());
                    const auto reservation = ledger.TryAdmit(QueuedOperation(10, 1, kind), 2, policy.revision).Value();
                    if (kind != StreamingCellOperationKind::Retire)
                        REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::BeginPreparation).HasValue());
                    if (kind == StreamingCellOperationKind::Activate)
                        REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::BeginActivation).HasValue());
                    REQUIRE(ledger.Advance(reservation, reason).HasValue());
                    RequireError(ledger.Release(reservation), WorldStreamingErrors::SchedulerLifecycleUnavailable);
                    RequireError(ledger.TryAdmit(QueuedOperation(11, 2, kind), 1, policy.revision),
                                 WorldStreamingErrors::SchedulerCapacityExceeded);
                    auto stale = reservation;
                    stale.operation.fence.generation = IdentityFrom<StreamingGeneration>(2);
                    RequireError(ledger.Advance(stale, StreamingCellOperationTransition::AcknowledgeRetirement),
                                 WorldStreamingErrors::SchedulerReservationStale);
                    REQUIRE(ledger.ReservedCount(kind).Value() == 1);
                    REQUIRE(ledger.Advance(reservation, StreamingCellOperationTransition::AcknowledgeRetirement).HasValue());
                    // Terminal snapshots still own their slots until the authority explicitly releases the reservation.
                    RequireError(ledger.TryAdmit(QueuedOperation(11, 2, kind), 1, policy.revision),
                                 WorldStreamingErrors::SchedulerCapacityExceeded);
                    REQUIRE(ledger.Release(reservation).HasValue());
                    const auto next = ledger.TryAdmit(QueuedOperation(11, 2, kind), 1, policy.revision).Value();
                    Retire(ledger, next, StreamingCellOperationTransition::Cancel);
                    REQUIRE(ledger.Release(next).HasValue());
                    RequireError(ledger.Release(reservation), WorldStreamingErrors::SchedulerReservationStale);
                }
            }
        }

        TEST_CASE("Concurrency replacement fences new work and preserves old reservations",
                  "[unit][world_streaming][scheduler][concurrency][fence]") {
            auto ledger = CreateLedger(1, 4, 10);
            const auto revision = IdentityFrom<StreamingConcurrencyRevision>(1);
            const auto first = ledger.TryAdmit(QueuedOperation(10), 1, revision).Value();
            const auto second = ledger.TryAdmit(QueuedOperation(11), 1, revision).Value();
            auto policy = Policy(1, 2);
            REQUIRE(ledger.ReplaceConcurrency(revision, policy).HasValue());
            REQUIRE(ledger.ReservedCount(StreamingCellOperationKind::Activate).Value() == 2);
            RequireError(ledger.TryAdmit(QueuedOperation(12), 1, revision), WorldStreamingErrors::SchedulerConcurrencyStale);
            RequireError(ledger.TryAdmit(QueuedOperation(12), 1, policy.revision), WorldStreamingErrors::SchedulerCapacityExceeded);
            CompleteActivation(ledger, first);
            REQUIRE(ledger.Release(first).HasValue());
            RequireError(ledger.TryAdmit(QueuedOperation(12), 1, policy.revision), WorldStreamingErrors::SchedulerCapacityExceeded);
            CompleteActivation(ledger, second);
            REQUIRE(ledger.Release(second).HasValue());
            const auto next = ledger.TryAdmit(QueuedOperation(12), 1, policy.revision).Value();
            Retire(ledger, next, StreamingCellOperationTransition::Replace);
            REQUIRE(ledger.Release(next).HasValue());
            REQUIRE(ledger.ReservedCount() == 0);
        }

        TEST_CASE("Malformed disabled and stale concurrency inputs never mutate admission",
                  "[unit][world_streaming][scheduler][concurrency]") {
            auto ledger = CreateLedger();
            const auto revision = IdentityFrom<StreamingConcurrencyRevision>(1);
            auto candidate = Policy(1, 2);
            const auto original = ledger.Limits();
            RequireError(ledger.TryAdmit(QueuedOperation(1), 1, {}), WorldStreamingErrors::SchedulerAdmissionInvalid);
            RequireError(ledger.ReplaceConcurrency({}, candidate), WorldStreamingErrors::SchedulerAdmissionInvalid);
            RequireError(ledger.ReplaceConcurrency(candidate.revision, candidate), WorldStreamingErrors::SchedulerConcurrencyStale);
            candidate.revision = revision;
            RequireError(ledger.ReplaceConcurrency(revision, candidate), WorldStreamingErrors::SchedulerConcurrencyStale);
            candidate = Policy(1, 2, WorldPartitionProjectProfile::Server);
            RequireError(ledger.ReplaceConcurrency(revision, candidate), WorldStreamingErrors::SchedulerConcurrencyStale);
            candidate = Policy(1, 2, static_cast<WorldPartitionProjectProfile>(255));
            RequireError(ledger.ReplaceConcurrency(revision, candidate), WorldStreamingErrors::SchedulerConcurrencyUnsupported);
            RequireError(StreamingSchedulerAdmissionLedger::Create(IdentityFrom<StreamingSchedulerLedgerId>(2), {.concurrentOperations = 2,
                                                                                                                 .capacityUnits = 10,
                                                                                                                 .concurrency = candidate}),
                         WorldStreamingErrors::SchedulerConcurrencyUnsupported);
            candidate = Policy(StreamingSchedulerAdmissionLimits::MaximumConcurrentOperations + 1, 2);
            RequireError(ledger.ReplaceConcurrency(revision, candidate), WorldStreamingErrors::SchedulerAdmissionInvalid);
            candidate = Policy(0, 2);
            RequireError(ledger.ReplaceConcurrency(revision, candidate), WorldStreamingErrors::SchedulerAdmissionInvalid);
            candidate = Policy(1, 2);
            candidate.revision = {};
            RequireError(ledger.ReplaceConcurrency(revision, candidate), WorldStreamingErrors::SchedulerAdmissionInvalid);
            REQUIRE(ledger.Limits() == original);
            REQUIRE(ledger.ReservedCount() == 0);
            REQUIRE(ledger.ReservedCapacityUnits() == 0);
            RequireError(ledger.ReservedCount(static_cast<StreamingCellOperationKind>(255)),
                         WorldStreamingErrors::SchedulerConcurrencyUnsupported);
            RequireError(candidate.Limit(static_cast<StreamingCellOperationKind>(255)),
                         WorldStreamingErrors::SchedulerConcurrencyUnsupported);

            candidate = Policy(1, 2);
            candidate.loads = 0;
            REQUIRE(ledger.ReplaceConcurrency(revision, candidate).HasValue());
            RequireError(ledger.TryAdmit(QueuedOperation(1, 1, StreamingCellOperationKind::Load), 1, candidate.revision),
                         WorldStreamingErrors::SchedulerConcurrencyUnsupported);
            REQUIRE(ledger.ReservedCount() == 0);
            ledger.BeginShutdown();
            RequireError(ledger.ReplaceConcurrency(candidate.revision, Policy(1, 3)), WorldStreamingErrors::SchedulerLifecycleUnavailable);
        }

        TEST_CASE("Concurrency policy revisions never wrap at their maximum", "[unit][world_streaming][scheduler][concurrency][fence]") {
            auto ledger = CreateLedger();
            const auto last = Policy(1, std::numeric_limits<std::uint64_t>::max());
            REQUIRE(ledger.ReplaceConcurrency(IdentityFrom<StreamingConcurrencyRevision>(1), last).HasValue());
            RequireError(ledger.ReplaceConcurrency(last.revision, Policy(1, 1)), WorldStreamingErrors::SchedulerConcurrencyStale);
            RequireError(ledger.ReplaceConcurrency(last.revision, last), WorldStreamingErrors::SchedulerConcurrencyStale);
            REQUIRE(ledger.Limits().concurrency == last);
        }

        TEST_CASE("Moving a concurrency owner transfers charges and closes the source",
                  "[unit][world_streaming][scheduler][concurrency][shutdown]") {
            auto source = CreateLedger();
            const auto revision = IdentityFrom<StreamingConcurrencyRevision>(1);
            const auto reservation = source.TryAdmit(QueuedOperation(1), 4, revision).Value();
            auto destination = std::move(source);
            REQUIRE(source.State() == StreamingSchedulerAdmissionState::Closed);
            REQUIRE(source.ReservedCount() == 0);
            REQUIRE(source.ReservedCapacityUnits() == 0);
            RequireError(source.TryAdmit(QueuedOperation(2), 1, revision), WorldStreamingErrors::SchedulerLifecycleUnavailable);
            REQUIRE(destination.ReservedCount(StreamingCellOperationKind::Activate).Value() == 1);
            REQUIRE(destination.ReservedCapacityUnits() == 4);
            destination.BeginShutdown();
            REQUIRE(destination.State() == StreamingSchedulerAdmissionState::Draining);
            Retire(destination, reservation, StreamingCellOperationTransition::Shutdown);
            REQUIRE(destination.Release(reservation).HasValue());
            REQUIRE(destination.State() == StreamingSchedulerAdmissionState::Closed);
        }

        TEST_CASE("Scheduler shutdown rejects new work and closes after canonical drain", "[unit][world_streaming][scheduler][shutdown]") {
            auto empty = CreateLedger();
            empty.BeginShutdown();
            REQUIRE(empty.State() == StreamingSchedulerAdmissionState::Closed);
            RequireError(empty.TryAdmit(QueuedOperation(10), 1, IdentityFrom<StreamingConcurrencyRevision>(1)),
                         WorldStreamingErrors::SchedulerLifecycleUnavailable);

            auto ledger = CreateLedger();
            const auto reservation = ledger.TryAdmit(QueuedOperation(20), 5, IdentityFrom<StreamingConcurrencyRevision>(1)).Value();
            ledger.BeginShutdown();
            REQUIRE(ledger.State() == StreamingSchedulerAdmissionState::Draining);
            RequireError(ledger.TryAdmit(QueuedOperation(21), 1, IdentityFrom<StreamingConcurrencyRevision>(1)),
                         WorldStreamingErrors::SchedulerLifecycleUnavailable);

            Retire(ledger, reservation, StreamingCellOperationTransition::Shutdown);
            REQUIRE(ledger.Release(reservation).HasValue());
            REQUIRE(ledger.State() == StreamingSchedulerAdmissionState::Closed);
            REQUIRE(ledger.ReservedCount() == 0);
            REQUIRE(ledger.ReservedCapacityUnits() == 0);
        }
    }  // namespace
}  // namespace Horo::WorldStreaming
