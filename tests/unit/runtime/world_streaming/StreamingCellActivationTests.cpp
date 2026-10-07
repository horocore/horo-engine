#include "Horo/WorldStreaming/StreamingCellActivation.h"
#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "StreamingCellCandidateTestSupport.h"
#include "WorldStreamingTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::RequireError;

        struct ReceiptLog final {
            std::vector<std::uint64_t> published;
            std::vector<std::uint64_t> rolledBack;
        };

        class TestReceipt final : public IStreamingCellActivationReceipt {
        public:
            TestReceipt(const StreamingCellActivationRequirement requirement, const StreamingCellOperationHandle operation,
                        ReceiptLog &log) noexcept
                : requirement_(requirement), operation_(operation), log_(log) {}

            StreamingCellActivationRequirement Requirement() const noexcept override {
                return requirement_;
            }

            StreamingCellOperationHandle Operation() const noexcept override {
                return operation_;
            }

            void PublishPrepared() noexcept override {
                log_.published.push_back(requirement_.participant.Value());
                published_ = true;
            }

            void RollbackPrepared() noexcept override {
                if (published_ || rolledBack_)
                    return;
                log_.rolledBack.push_back(requirement_.participant.Value());
                rolledBack_ = true;
            }

            std::uint64_t MaximumPublicationNanoseconds() const noexcept override {
                return cost;
            }

            std::uint64_t cost{10};

        private:
            StreamingCellActivationRequirement requirement_;
            StreamingCellOperationHandle operation_;
            ReceiptLog &log_;
            bool published_{};
            bool rolledBack_{};
        };

        [[nodiscard]] StreamingCellOperation Advance(StreamingCellOperation operation, const StreamingCellOperationTransition transition) {
            return operation.Advance(operation.Handle(), transition).Value();
        }

        [[nodiscard]] StreamingCellOperation Activating(const StreamingCellOperationHandle handle = CandidateTestSupport::Operation()) {
            auto operation = StreamingCellOperation::Create(handle, StreamingCellOperationKind::Activate).Value();
            operation = Advance(std::move(operation), StreamingCellOperationTransition::Admit);
            operation = Advance(std::move(operation), StreamingCellOperationTransition::BeginPreparation);
            return Advance(std::move(operation), StreamingCellOperationTransition::BeginActivation);
        }

        [[nodiscard]] StreamingCellActivationRequirement Requirement(const std::uint64_t id, const std::uint64_t revision = 1) {
            return {.participant = IdentityFrom<StreamingRuntimeServiceId>(id),
                    .revision = IdentityFrom<StreamingRuntimeServiceRevision>(revision)};
        }

        [[nodiscard]] StreamingCellActivationContext Context(
            StreamingCellOperation operation, const StreamingCellActivationLifecycle lifecycle = StreamingCellActivationLifecycle::Active,
            const std::size_t maximumReceipts = 4) {
            return {.activation = IdentityFrom<StreamingCellActivationId>(9),
                    .operation = std::move(operation),
                    .maximumReceipts = maximumReceipts,
                    .lifecycle = lifecycle,
                    .scheduler = IdentityFrom<StreamingSchedulerLedgerId>(1)};
        }

        [[nodiscard]] std::vector<std::unique_ptr<IStreamingCellActivationReceipt>> Receipts(
            const std::vector<StreamingCellActivationRequirement> &requirements, const StreamingCellOperationHandle operation,
            ReceiptLog &log) {
            std::vector<std::unique_ptr<IStreamingCellActivationReceipt>> receipts;
            receipts.reserve(requirements.size());
            for (const auto requirement : requirements)
                receipts.push_back(std::make_unique<TestReceipt>(requirement, operation, log));
            return receipts;
        }

        TEST_CASE("Cell activation publishes the complete set once in canonical order at the Scene safe point",
                  "[unit][world_streaming][activation]") {
            [[maybe_unused]] auto frameBudget =
                StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                                   IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                    .Value();
            const auto operation = Activating();
            const std::vector required{Requirement(30), Requirement(10), Requirement(20)};
            ReceiptLog log;
            auto transaction =
                StreamingCellActivationTransaction::Prepare(Context(operation), required, Receipts(required, operation.Handle(), log))
                    .Value();

            REQUIRE(transaction.State() == StreamingCellActivationState::Prepared);
            REQUIRE(transaction.Requirements()[0].participant == Requirement(10).participant);
            REQUIRE(transaction
                        .Commit(operation, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges,
                                StreamingCellActivationLifecycle::Active, frameBudget, 0)
                        .HasValue());
            REQUIRE(transaction.State() == StreamingCellActivationState::Published);
            REQUIRE(log.published == std::vector<std::uint64_t>{10, 20, 30});
            REQUIRE(log.rolledBack.empty());
            RequireError(transaction.Commit(operation, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges,
                                            StreamingCellActivationLifecycle::Active, frameBudget, 0),
                         WorldStreamingErrors::CellActivationLifecycleUnavailable);
        }

        TEST_CASE("Cell activation waits for CommitDeferredLifecycleChanges without losing prepared ownership",
                  "[unit][world_streaming][activation][safe_point]") {
            [[maybe_unused]] auto frameBudget =
                StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                                   IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                    .Value();
            const auto operation = Activating();
            const std::vector required{Requirement(10)};
            ReceiptLog log;
            auto transaction =
                StreamingCellActivationTransaction::Prepare(Context(operation), required, Receipts(required, operation.Handle(), log))
                    .Value();

            RequireError(transaction.Commit(operation, StreamingCellActivationCommitPoint::PreUpdate,
                                            StreamingCellActivationLifecycle::Active, frameBudget, 0),
                         WorldStreamingErrors::CellActivationSafePointUnavailable);
            REQUIRE(transaction.State() == StreamingCellActivationState::Prepared);
            REQUIRE(log.published.empty());
            REQUIRE(log.rolledBack.empty());
        }

        TEST_CASE("Stale operation snapshots and shutdown roll back every prepared receipt in reverse order",
                  "[unit][world_streaming][activation][rollback]") {
            [[maybe_unused]] auto frameBudget =
                StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                                   IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                    .Value();
            const auto operation = Activating();
            const std::vector required{Requirement(10), Requirement(20), Requirement(30)};
            ReceiptLog staleLog;
            auto stale =
                StreamingCellActivationTransaction::Prepare(Context(operation), required, Receipts(required, operation.Handle(), staleLog))
                    .Value();
            auto replacement = operation.Handle();
            replacement.operation = IdentityFrom<StreamingCellOperationId>(10);
            replacement.fence.generation = IdentityFrom<StreamingGeneration>(2);
            RequireError(stale.Commit(Activating(replacement), StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges,
                                      StreamingCellActivationLifecycle::Active, frameBudget, 0),
                         WorldStreamingErrors::CellActivationStale);
            REQUIRE(stale.State() == StreamingCellActivationState::RolledBack);
            REQUIRE(staleLog.published.empty());
            REQUIRE(staleLog.rolledBack == std::vector<std::uint64_t>{30, 20, 10});

            ReceiptLog shutdownLog;
            auto shutdown = StreamingCellActivationTransaction::Prepare(Context(operation), required,
                                                                        Receipts(required, operation.Handle(), shutdownLog))
                                .Value();
            RequireError(shutdown.Commit(operation, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges,
                                         StreamingCellActivationLifecycle::Closed, frameBudget, 0),
                         WorldStreamingErrors::CellActivationLifecycleUnavailable);
            REQUIRE(shutdownLog.rolledBack == std::vector<std::uint64_t>{30, 20, 10});

            ReceiptLog cancelledLog;
            auto cancelled = StreamingCellActivationTransaction::Prepare(Context(operation), required,
                                                                         Receipts(required, operation.Handle(), cancelledLog))
                                 .Value();
            const auto retiring = Advance(operation, StreamingCellOperationTransition::Cancel);
            REQUIRE(retiring.Handle() == operation.Handle());
            RequireError(cancelled.Commit(retiring, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges,
                                          StreamingCellActivationLifecycle::Active, frameBudget, 0),
                         WorldStreamingErrors::CellActivationStale);
            REQUIRE(cancelled.State() == StreamingCellActivationState::RolledBack);
            REQUIRE(cancelledLog.published.empty());
            REQUIRE(cancelledLog.rolledBack == std::vector<std::uint64_t>{30, 20, 10});
        }

        TEST_CASE("Activation preparation rejects incomplete duplicate over-capacity and stale receipt sets transactionally",
                  "[unit][world_streaming][activation][failure]") {
            [[maybe_unused]] auto frameBudget =
                StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                                   IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                    .Value();
            const auto operation = Activating();
            const std::vector required{Requirement(10), Requirement(20)};

            ReceiptLog emptyLog;
            const std::vector<StreamingCellActivationRequirement> empty;
            RequireError(StreamingCellActivationTransaction::Prepare(Context(operation), empty,
                                                                     Receipts(empty, operation.Handle(), emptyLog)),
                         WorldStreamingErrors::CellActivationIncomplete);
            REQUIRE(emptyLog.rolledBack.empty());

            ReceiptLog incompleteLog;
            const std::vector incomplete{Requirement(10)};
            RequireError(StreamingCellActivationTransaction::Prepare(Context(operation), required,
                                                                     Receipts(incomplete, operation.Handle(), incompleteLog)),
                         WorldStreamingErrors::CellActivationIncomplete);
            REQUIRE(incompleteLog.rolledBack == std::vector<std::uint64_t>{10});

            ReceiptLog duplicateLog;
            const std::vector duplicate{Requirement(10), Requirement(10)};
            RequireError(StreamingCellActivationTransaction::Prepare(Context(operation), duplicate,
                                                                     Receipts(duplicate, operation.Handle(), duplicateLog)),
                         WorldStreamingErrors::CellActivationInvalid);
            REQUIRE(duplicateLog.rolledBack.size() == 2);

            ReceiptLog capacityLog;
            RequireError(StreamingCellActivationTransaction::Prepare(Context(operation, StreamingCellActivationLifecycle::Active, 1),
                                                                     required, Receipts(required, operation.Handle(), capacityLog)),
                         WorldStreamingErrors::CellActivationCapacityExceeded);
            REQUIRE(capacityLog.rolledBack.size() == 2);

            ReceiptLog staleLog;
            auto replacement = operation.Handle();
            replacement.operation = IdentityFrom<StreamingCellOperationId>(10);
            replacement.fence.generation = IdentityFrom<StreamingGeneration>(2);
            RequireError(StreamingCellActivationTransaction::Prepare(Context(operation), required,
                                                                     Receipts(required, replacement, staleLog)),
                         WorldStreamingErrors::CellActivationStale);
            REQUIRE(staleLog.rolledBack.size() == 2);

            ReceiptLog revisionLog;
            const std::vector staleRevision{Requirement(10, 2), Requirement(20)};
            RequireError(StreamingCellActivationTransaction::Prepare(Context(operation), required,
                                                                     Receipts(staleRevision, operation.Handle(), revisionLog)),
                         WorldStreamingErrors::CellActivationStale);
            REQUIRE(revisionLog.rolledBack.size() == 2);
        }

        TEST_CASE("Cancellation closed admission and invalid operation phase release all acquired receipts",
                  "[unit][world_streaming][activation][lifecycle]") {
            [[maybe_unused]] auto frameBudget =
                StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                                   IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                    .Value();
            const auto operation = Activating();
            const std::vector required{Requirement(10)};
            ReceiptLog cancellationLog;
            RequireError(StreamingCellActivationTransaction::Prepare(Context(operation, StreamingCellActivationLifecycle::Cancelling),
                                                                     required, Receipts(required, operation.Handle(), cancellationLog)),
                         WorldStreamingErrors::CellActivationLifecycleUnavailable);
            REQUIRE(cancellationLog.rolledBack == std::vector<std::uint64_t>{10});

            auto preparing =
                StreamingCellOperation::Create(CandidateTestSupport::Operation(), StreamingCellOperationKind::Activate).Value();
            preparing = Advance(std::move(preparing), StreamingCellOperationTransition::Admit);
            preparing = Advance(std::move(preparing), StreamingCellOperationTransition::BeginPreparation);
            ReceiptLog phaseLog;
            RequireError(StreamingCellActivationTransaction::Prepare(Context(preparing), required,
                                                                     Receipts(required, preparing.Handle(), phaseLog)),
                         WorldStreamingErrors::CellActivationInvalid);
            REQUIRE(phaseLog.rolledBack == std::vector<std::uint64_t>{10});
        }

        TEST_CASE("Move ownership and destruction roll back a prepared set exactly once",
                  "[unit][world_streaming][activation][ownership]") {
            [[maybe_unused]] auto frameBudget =
                StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                                   IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                    .Value();
            const auto operation = Activating();
            const std::vector required{Requirement(10), Requirement(20)};
            ReceiptLog log;
            {
                auto first =
                    StreamingCellActivationTransaction::Prepare(Context(operation), required, Receipts(required, operation.Handle(), log))
                        .Value();
                auto owner = std::move(first);
                REQUIRE(first.State() == StreamingCellActivationState::RolledBack);
                REQUIRE(owner.State() == StreamingCellActivationState::Prepared);
            }
            REQUIRE(log.rolledBack == std::vector<std::uint64_t>{20, 10});
            REQUIRE(log.published.empty());
        }

        TEST_CASE("Prepared cell publication defers atomically and resumes in the next shared frame",
                  "[unit][world_streaming][activation][frame_budget]") {
            const auto operation = Activating();
            const std::vector required{Requirement(10), Requirement(20)};
            ReceiptLog log;
            auto transaction =
                StreamingCellActivationTransaction::Prepare(Context(operation), required, Receipts(required, operation.Handle(), log))
                    .Value();
            auto frame =
                StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                                   IdentityFrom<StreamingOwnerFrameId>(1), 20, 1})
                    .Value();
            REQUIRE(frame.TryConsume(frame.Limits().owner, 1, 0).Value());
            RequireError(transaction.Commit(operation, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges,
                                            StreamingCellActivationLifecycle::Active, frame, 0),
                         WorldStreamingErrors::OwnerFrameDeferred);
            REQUIRE(transaction.State() == StreamingCellActivationState::Prepared);
            REQUIRE(log.published.empty());
            REQUIRE(log.rolledBack.empty());
            auto limits = frame.Limits();
            limits.frame = IdentityFrom<StreamingOwnerFrameId>(2);
            auto nextFrame = StreamingOwnerFrameBudget::Create(limits).Value();
            REQUIRE(transaction
                        .Commit(operation, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges,
                                StreamingCellActivationLifecycle::Active, nextFrame, 0)
                        .HasValue());
            REQUIRE(log.published == std::vector<std::uint64_t>{10, 20});
            REQUIRE(nextFrame.ConsumedUnits() == 1);
            REQUIRE(nextFrame.ChargedNanoseconds() == 20);
        }

        TEST_CASE("An oversized atomic publication never partially publishes and cancellation still revokes it",
                  "[unit][world_streaming][activation][frame_budget][failure]") {
            const auto operation = Activating();
            const std::vector required{Requirement(10), Requirement(20)};
            ReceiptLog log;
            auto transaction =
                StreamingCellActivationTransaction::Prepare(Context(operation), required, Receipts(required, operation.Handle(), log))
                    .Value();
            auto frame =
                StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                                   IdentityFrom<StreamingOwnerFrameId>(1), 19, 1})
                    .Value();
            RequireError(transaction.Commit(operation, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges,
                                            StreamingCellActivationLifecycle::Active, frame, 0),
                         WorldStreamingErrors::OwnerFrameCapacityExceeded);
            REQUIRE(log.published.empty());
            REQUIRE(frame.ConsumedUnits() == 0);
            const auto cancelled = Advance(operation, StreamingCellOperationTransition::Cancel);
            RequireError(transaction.Commit(cancelled, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges,
                                            StreamingCellActivationLifecycle::Active, frame, 0),
                         WorldStreamingErrors::CellActivationStale);
            REQUIRE(log.rolledBack == std::vector<std::uint64_t>{20, 10});
        }

        TEST_CASE("Malformed publication cost fails preparation and transfers every receipt to rollback",
                  "[unit][world_streaming][activation][frame_budget][failure]") {
            const auto operation = Activating();
            const std::vector required{Requirement(10), Requirement(20)};
            for (const auto cost : {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}) {
                ReceiptLog log;
                std::vector<std::unique_ptr<IStreamingCellActivationReceipt>> receipts;
                auto first = std::make_unique<TestReceipt>(required[0], operation.Handle(), log);
                first->cost = cost;
                receipts.push_back(std::move(first));
                receipts.push_back(std::make_unique<TestReceipt>(required[1], operation.Handle(), log));
                RequireError(StreamingCellActivationTransaction::Prepare(Context(operation), required, std::move(receipts)),
                             WorldStreamingErrors::OwnerFrameInvalid);
                REQUIRE(log.published.empty());
                REQUIRE(log.rolledBack == std::vector<std::uint64_t>{20, 10});
            }
        }
    }  // namespace
}  // namespace Horo::WorldStreaming
