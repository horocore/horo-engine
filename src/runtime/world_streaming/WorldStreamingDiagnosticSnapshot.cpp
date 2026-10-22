#include "Horo/WorldStreaming/WorldStreamingDiagnosticSnapshot.h"

#include "WorldStreamingInternal.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <new>
#include <tuple>

namespace Horo::WorldStreaming {
    namespace {
        /** @brief Checks the closed lifecycle vocabulary without accepting a future enum implicitly. */
        bool IsKnownLifecycle(const WorldStreamingRuntimeCompositionState state) noexcept {
            return state >= WorldStreamingRuntimeCompositionState::Active && state <= WorldStreamingRuntimeCompositionState::Closed;
        }

        /** @brief Checks the closed scheduler lifecycle vocabulary. */
        bool IsKnownQueueState(const StreamingSchedulerAdmissionState state) noexcept {
            return state >= StreamingSchedulerAdmissionState::Accepting && state <= StreamingSchedulerAdmissionState::Closed;
        }

        /** @brief Checks the failure-only subset of cell-operation terminal dispositions. */
        bool IsFailureReason(const StreamingCellOperationOutcome outcome) noexcept {
            using enum StreamingCellOperationOutcome;
            return outcome == Cancelled || outcome == Failed || outcome == Replaced || outcome == Shutdown;
        }

        bool IsKnownCategory(const StreamingDiagnosticEventCategory category) noexcept {
            return category >= StreamingDiagnosticEventCategory::CellLifecycle && category <= StreamingDiagnosticEventCategory::Rollback;
        }

        bool IsKnownSeverity(const StreamingDiagnosticEventSeverity severity) noexcept {
            return severity >= StreamingDiagnosticEventSeverity::Info && severity <= StreamingDiagnosticEventSeverity::Error;
        }

        bool IsKnownAdmission(const StreamingDiagnosticAdmissionDecision decision) noexcept {
            return decision >= StreamingDiagnosticAdmissionDecision::Accepted && decision <= StreamingDiagnosticAdmissionDecision::Rejected;
        }

        bool IsKnownContextKey(const StreamingDiagnosticContextKey key) noexcept {
            return key >= StreamingDiagnosticContextKey::RequestedCapacityUnits && key <= StreamingDiagnosticContextKey::BudgetRevision;
        }

        bool IsKnownCellState(const StreamingCellState state) noexcept {
            return state >= StreamingCellState::Unloaded && state <= StreamingCellState::Failed;
        }

        /** @brief Verifies that one fence belongs to the captured mounted authority. */
        bool MatchesOwner(const StreamingFence &fence, const StreamingRuntimeOwnerToken &owner) noexcept {
            return fence.partition == owner.partition && fence.epoch == owner.epoch;
        }

        bool IsLegalTransition(const StreamingDiagnosticCellTransition transition) noexcept {
            using enum StreamingCellState;
            static constexpr std::array allowed{
                StreamingDiagnosticCellTransition{Unloaded, Loading},  StreamingDiagnosticCellTransition{Loading, Resident},
                StreamingDiagnosticCellTransition{Loading, Evicting},  StreamingDiagnosticCellTransition{Resident, Active},
                StreamingDiagnosticCellTransition{Resident, Evicting}, StreamingDiagnosticCellTransition{Active, Evicting},
                StreamingDiagnosticCellTransition{Evicting, Unloaded}, StreamingDiagnosticCellTransition{Evicting, Failed},
                StreamingDiagnosticCellTransition{Failed, Loading},
            };
            return IsKnownCellState(transition.previous) && IsKnownCellState(transition.current) &&
                   std::ranges::find(allowed, transition) != allowed.end();
        }

        Result<void> ValidateEventContext(const StreamingDiagnosticDecisionEvent &event) {
            if (event.contextCount > event.context.size())
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionCapacityExceeded);
            for (std::size_t index = 0; index < event.contextCount; ++index) {
                if (!IsKnownContextKey(event.context[index].key))
                    return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionUnsupported);
            }
            return Result<void>::Success();
        }

        Result<void> ValidateLifecycleEvent(const StreamingDiagnosticDecisionEvent &event) {
            if (!event.transition || event.admission || event.outcome != StreamingCellOperationOutcome::None)
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionInvalid);
            return Result<void>::Success();
        }

        Result<void> ValidateAdmissionEvent(const StreamingDiagnosticDecisionEvent &event) {
            if (event.transition || !event.admission || event.outcome != StreamingCellOperationOutcome::None)
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionInvalid);
            if (!IsKnownAdmission(*event.admission))
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionUnsupported);
            return Result<void>::Success();
        }

        Result<void> ValidateRollbackEvent(const StreamingDiagnosticDecisionEvent &event) {
            if (!event.transition || event.admission || !IsFailureReason(event.outcome))
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionInvalid);
            using enum StreamingCellState;
            if (constexpr std::array terminalStates{Evicting, Unloaded, Failed};
                std::ranges::find(terminalStates, event.transition->current) == terminalStates.end())
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionInvalid);
            return Result<void>::Success();
        }

        Result<void> ValidateEventSemantics(const StreamingDiagnosticDecisionEvent &event) {
            if (!IsKnownCategory(event.category) || !IsKnownSeverity(event.severity))
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionUnsupported);
            using enum StreamingDiagnosticEventCategory;
            Result<void> category = Result<void>::Success();
            switch (event.category) {
                case CellLifecycle:
                    category = ValidateLifecycleEvent(event);
                    break;
                case Admission:
                    category = ValidateAdmissionEvent(event);
                    break;
                case Rollback:
                    category = ValidateRollbackEvent(event);
                    break;
            }
            if (category.HasError())
                return category;
            if (event.transition && !IsLegalTransition(*event.transition))
                return Internal::Failure<void>(WorldStreamingErrors::CellStateTransitionInvalid);
            return ValidateEventContext(event);
        }

        Result<void> ValidateEventIdentity(const StreamingDiagnosticDecisionEvent &event,
                                           const WorldStreamingDiagnosticSnapshotInput &input) {
            if (!event.id.IsValid() || !event.sequence.IsValid() || !event.ownerRevision.IsValid() || !event.snapshotRevision.IsValid() ||
                !event.operation.IsValid())
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionInvalid);
            if (event.ownerRevision != input.ownerRevision || event.snapshotRevision != input.revision ||
                !MatchesOwner(event.operation.fence, input.owner))
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionStale);
            return Result<void>::Success();
        }

        bool OperationLess(const StreamingCellOperationHandle &left, const StreamingCellOperationHandle &right) noexcept;

        Result<void> ValidateEventCellCorrelation(const StreamingDiagnosticDecisionEvent &event,
                                                  const std::span<const StreamingCellStateRecord> cells) {
            if (event.category == StreamingDiagnosticEventCategory::Admission)
                return Result<void>::Success();
            const bool matchingCell =
                std::ranges::binary_search(cells, event.operation, OperationLess, &StreamingCellStateRecord::operation);
            return matchingCell ? Result<void>::Success() : Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionStale);
        }

        /** @brief Orders exact operation handles by persistent cell key, generation and operation identity. */
        bool OperationLess(const StreamingCellOperationHandle &left, const StreamingCellOperationHandle &right) noexcept {
            const StreamingCellCanonicalLess cellLess;
            if (cellLess(left.fence.cell, right.fence.cell))
                return true;
            if (cellLess(right.fence.cell, left.fence.cell))
                return false;
            return std::tuple{left.fence.generation.Value(), left.operation.Value()} <
                   std::tuple{right.fence.generation.Value(), right.operation.Value()};
        }

        /** @brief Checks whether lifecycle and queue admission states describe one coherent safe point. */
        bool LifecycleQueueMismatch(const WorldStreamingDiagnosticSnapshotInput &input) noexcept {
            const auto &queue = input.queue;
            using enum WorldStreamingRuntimeCompositionState;
            switch (input.lifecycle) {
                case Active:
                    return queue.state != StreamingSchedulerAdmissionState::Accepting;
                case Cancelling:
                case Draining:
                    return queue.state == StreamingSchedulerAdmissionState::Accepting;
                case Closed:
                    return queue.state != StreamingSchedulerAdmissionState::Closed || queue.reservedOperations != 0 ||
                           queue.reservedCapacityUnits != 0;
            }
            return true;
        }

        /** @brief Verifies lifecycle/queue relationships and bounded aggregate pressure. */
        Result<void> ValidateQueue(const WorldStreamingDiagnosticSnapshotInput &input) {
            const auto &queue = input.queue;
            if (!queue.owner.IsValid() || !queue.limits.IsValid())
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionInvalid);
            if (!IsKnownLifecycle(input.lifecycle) || !IsKnownQueueState(queue.state))
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionUnsupported);
            if (queue.reservedOperations > queue.limits.concurrentOperations || queue.reservedCapacityUnits > queue.limits.capacityUnits)
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionCapacityExceeded);

            if (LifecycleQueueMismatch(input))
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionInvalid);
            return Result<void>::Success();
        }

        bool SnapshotIdentityIsValid(const WorldStreamingDiagnosticSnapshotInput &input) noexcept {
            return input.owner.IsValid() && input.revision.IsValid() && input.ownerRevision.IsValid() && input.limits.IsValid();
        }

        bool IsKnownInstrumentation(const StreamingDiagnosticInstrumentation instrumentation) noexcept {
            return instrumentation >= StreamingDiagnosticInstrumentation::Enabled &&
                   instrumentation <= StreamingDiagnosticInstrumentation::Disabled;
        }

        bool RowsFitDeclaredLimits(const WorldStreamingDiagnosticSnapshotInput &input,
                                   const std::span<const StreamingSourceDesiredState> sources,
                                   const std::span<const StreamingCellStateRecord> cells,
                                   const std::span<const StreamingDiagnosticFailureRecord> failures,
                                   const std::span<const StreamingDiagnosticDecisionEvent> events) noexcept {
            const bool baseRowsFit =
                sources.size() <= input.limits.sources && cells.size() <= input.limits.cells && failures.size() <= input.limits.failures;
            const bool eventRowsFit =
                input.instrumentation == StreamingDiagnosticInstrumentation::Disabled || events.size() <= input.limits.events;
            return baseRowsFit && eventRowsFit;
        }

        /** @brief Validates immutable aggregate identity, policy correlation and declared capacities. */
        Result<void> ValidateSnapshotInput(const WorldStreamingDiagnosticSnapshotInput &input,
                                           const std::optional<StreamingBudgetPolicy> &policy,
                                           const std::optional<StreamingBudgetSample> &sample, const WorldStreamingDiagnosticRows &rows) {
            if (!SnapshotIdentityIsValid(input))
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionInvalid);
            if (!IsKnownInstrumentation(input.instrumentation))
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionUnsupported);
            if (const auto queue = ValidateQueue(input); queue.HasError())
                return queue;
            if (sample.has_value() != policy.has_value())
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionInvalid);
            if (sample && sample->PolicyRevision() != policy->Revision())
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionStale);
            if (!RowsFitDeclaredLimits(input, rows.sources, rows.cells, rows.failures, rows.events))
                return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionCapacityExceeded);
            return Result<void>::Success();
        }

        Result<void> ValidateEvents(const std::span<const StreamingDiagnosticDecisionEvent> events,
                                    const std::span<const StreamingCellStateRecord> cells,
                                    const WorldStreamingDiagnosticSnapshotInput &input) {
            if (input.instrumentation == StreamingDiagnosticInstrumentation::Disabled)
                return Result<void>::Success();
            for (const auto &event : events) {
                if (const auto identity = ValidateEventIdentity(event, input); identity.HasError())
                    return identity;
                if (const auto semantics = ValidateEventSemantics(event); semantics.HasError())
                    return semantics;
                if (const auto correlation = ValidateEventCellCorrelation(event, cells); correlation.HasError())
                    return correlation;
            }
            return Result<void>::Success();
        }

        /** @brief Validates captured sources against the mounted authority. */
        Result<void> ValidateSources(const std::span<const StreamingSourceDesiredState> sources, const StreamingRuntimeOwnerToken &owner) {
            for (const auto &source : sources) {
                const auto &descriptor = source.Source();
                if (!descriptor.IsValid())
                    return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionInvalid);
                if (descriptor.owner.partition != owner.partition || descriptor.owner.epoch != owner.epoch)
                    return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionStale);
            }
            return Result<void>::Success();
        }

        /** @brief Validates captured cell records against the mounted authority. */
        Result<void> ValidateCells(const std::span<const StreamingCellStateRecord> cells, const StreamingRuntimeOwnerToken &owner) {
            for (const auto &cell : cells) {
                if (!cell.IsValid())
                    return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionUnsupported);
                if (!MatchesOwner(cell.operation.fence, owner))
                    return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionStale);
            }
            return Result<void>::Success();
        }

        /** @brief Validates failure records and their captured-cell correlation. */
        Result<void> ValidateFailures(const std::span<const StreamingDiagnosticFailureRecord> failures,
                                      const std::span<const StreamingCellStateRecord> cells, const StreamingRuntimeOwnerToken &owner) {
            for (const auto &failure : failures) {
                if (!failure.operation.IsValid())
                    return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionInvalid);
                if (!IsFailureReason(failure.reason))
                    return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionUnsupported);
                if (!MatchesOwner(failure.operation.fence, owner))
                    return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionStale);
                if (!std::ranges::binary_search(cells, failure.operation, OperationLess, &StreamingCellStateRecord::operation))
                    return Internal::Failure<void>(WorldStreamingErrors::DiagnosticProjectionStale);
            }
            return Result<void>::Success();
        }

        /** @brief Validates records that do not require a sorted cell lookup. */
        Result<void> ValidateBaseRecords(const WorldStreamingDiagnosticSnapshotInput &input, const WorldStreamingDiagnosticRows &rows) {
            if (const auto validSources = ValidateSources(rows.sources, input.owner); validSources.HasError())
                return validSources;
            return ValidateCells(rows.cells, input.owner);
        }

        /** @brief Validates failure/event correlation against canonical sorted cell records. */
        Result<void> ValidateCorrelatedRecords(const WorldStreamingDiagnosticSnapshotInput &input, const WorldStreamingDiagnosticRows &rows,
                                               const std::span<const StreamingCellStateRecord> sortedCells) {
            if (const auto validFailures = ValidateFailures(rows.failures, sortedCells, input.owner); validFailures.HasError())
                return validFailures;
            return ValidateEvents(rows.events, sortedCells, input);
        }

        /** @brief Checks canonicalized records for competing identities. */
        bool HasIdentityConflict(const std::vector<StreamingSourceDesiredState> &sources,
                                 const std::vector<StreamingCellStateRecord> &cells,
                                 const std::vector<StreamingDiagnosticFailureRecord> &failures,
                                 const std::vector<StreamingDiagnosticDecisionEvent> &events) {
            if (const bool rowConflict = std::ranges::adjacent_find(sources,
                                                                    [](const auto &left, const auto &right) {
                return left.Source().id == right.Source().id;
            }) != sources.end() ||
                                         std::ranges::adjacent_find(cells,
                                                                    [](const auto &left, const auto &right) {
                return left.operation.fence == right.operation.fence;
            }) != cells.end() ||
                                         std::ranges::adjacent_find(failures,
                                                                    [](const auto &left, const auto &right) {
                return left.operation == right.operation;
            }) != failures.end() ||
                                         std::ranges::adjacent_find(events,
                                                                    [](const auto &left, const auto &right) {
                return left.sequence == right.sequence;
            }) != events.end();
                rowConflict)
                return true;

            std::vector<std::uint64_t> eventIds;
            eventIds.reserve(events.size());
            std::ranges::transform(events, std::back_inserter(eventIds), [](const auto &event) {
                return event.id.Value();
            });
            std::ranges::sort(eventIds);
            return std::ranges::adjacent_find(eventIds) != eventIds.end();
        }

        void CanonicalizeEvent(StreamingDiagnosticDecisionEvent &event) {
            const auto end = event.context.begin() + event.contextCount;
            std::sort(event.context.begin(), end, [](const auto &left, const auto &right) {
                return left.key < right.key;
            });
        }

        bool HasContextConflict(const std::vector<StreamingDiagnosticDecisionEvent> &events) {
            return std::ranges::any_of(events, [](const StreamingDiagnosticDecisionEvent &event) {
                const auto end = event.context.begin() + event.contextCount;
                return std::adjacent_find(event.context.begin(), end, [](const auto &left, const auto &right) {
                    return left.key == right.key;
                }) != end;
            });
        }
    }  // namespace

    /** @copydoc WorldStreamingDiagnosticLimits::IsValid */
    bool WorldStreamingDiagnosticLimits::IsValid() const noexcept {
        return sources > 0 && sources <= MaximumSources && cells > 0 && cells <= MaximumCells && failures > 0 &&
               failures <= MaximumFailures && events > 0 && events <= MaximumEvents;
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::Create */
    Result<WorldStreamingDiagnosticSnapshot> WorldStreamingDiagnosticSnapshot::Create(const WorldStreamingDiagnosticSnapshotInput &input,
                                                                                      const StreamingBudgetPolicy &policy,
                                                                                      const StreamingBudgetSample &sample,
                                                                                      const WorldStreamingDiagnosticRows &rows) {
        return Capture(input, policy, sample, rows);
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::CreateStateOnly */
    Result<WorldStreamingDiagnosticSnapshot> WorldStreamingDiagnosticSnapshot::CreateStateOnly(
        const WorldStreamingDiagnosticSnapshotInput &input, const WorldStreamingDiagnosticRows &rows) {
        return Capture(input, std::nullopt, std::nullopt, rows);
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::Capture */
    Result<WorldStreamingDiagnosticSnapshot> WorldStreamingDiagnosticSnapshot::Capture(const WorldStreamingDiagnosticSnapshotInput &input,
                                                                                       std::optional<StreamingBudgetPolicy> policy,
                                                                                       std::optional<StreamingBudgetSample> sample,
                                                                                       const WorldStreamingDiagnosticRows &rows) {
        if (const auto validInput = ValidateSnapshotInput(input, policy, sample, rows); validInput.HasError())
            return Result<WorldStreamingDiagnosticSnapshot>::Failure(validInput.ErrorValue());
        if (const auto validRecords = ValidateBaseRecords(input, rows); validRecords.HasError())
            return Result<WorldStreamingDiagnosticSnapshot>::Failure(validRecords.ErrorValue());

        try {
            std::vector<StreamingSourceDesiredState> ownedSources(rows.sources.begin(), rows.sources.end());
            std::vector<StreamingCellStateRecord> ownedCells(rows.cells.begin(), rows.cells.end());
            std::vector<StreamingDiagnosticFailureRecord> ownedFailures(rows.failures.begin(), rows.failures.end());
            std::vector<StreamingDiagnosticDecisionEvent> ownedEvents;
            if (input.instrumentation == StreamingDiagnosticInstrumentation::Enabled)
                ownedEvents.assign(rows.events.begin(), rows.events.end());

            std::ranges::sort(ownedSources, {}, [](const StreamingSourceDesiredState &source) {
                return source.Source().id.Value();
            });
            std::ranges::sort(ownedCells, [](const StreamingCellStateRecord &left, const StreamingCellStateRecord &right) {
                return OperationLess(left.operation, right.operation);
            });
            if (const auto validRecords = ValidateCorrelatedRecords(input, rows, ownedCells); validRecords.HasError())
                return Result<WorldStreamingDiagnosticSnapshot>::Failure(validRecords.ErrorValue());
            std::ranges::sort(ownedFailures,
                              [](const StreamingDiagnosticFailureRecord &left, const StreamingDiagnosticFailureRecord &right) {
                return OperationLess(left.operation, right.operation);
            });

            for (auto &event : ownedEvents)
                CanonicalizeEvent(event);
            std::ranges::sort(ownedEvents, {}, [](const StreamingDiagnosticDecisionEvent &event) {
                return event.sequence.Value();
            });

            if (HasIdentityConflict(ownedSources, ownedCells, ownedFailures, ownedEvents) || HasContextConflict(ownedEvents))
                return Internal::Failure<WorldStreamingDiagnosticSnapshot>(WorldStreamingErrors::DiagnosticProjectionIdentityConflict);

            return Result<WorldStreamingDiagnosticSnapshot>::Success(
                WorldStreamingDiagnosticSnapshot{input, policy, sample, std::move(ownedSources), std::move(ownedCells),
                                                 std::move(ownedFailures), std::move(ownedEvents)});
        } catch (const std::bad_alloc &) {
            return Internal::Failure<WorldStreamingDiagnosticSnapshot>(WorldStreamingErrors::DiagnosticProjectionCapacityExceeded);
        }
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::Replace */
    Result<WorldStreamingDiagnosticSnapshot> WorldStreamingDiagnosticSnapshot::Replace(const WorldStreamingDiagnosticSnapshot &previous,
                                                                                       const WorldStreamingDiagnosticSnapshotInput &input,
                                                                                       const StreamingBudgetPolicy &policy,
                                                                                       const StreamingBudgetSample &sample,
                                                                                       const WorldStreamingDiagnosticRows &rows) {
        if (input.owner != previous.Owner() || input.revision.Value() <= previous.Revision().Value() ||
            input.ownerRevision.Value() < previous.OwnerRevision().Value())
            return Internal::Failure<WorldStreamingDiagnosticSnapshot>(WorldStreamingErrors::DiagnosticProjectionStale);
        return Create(input, policy, sample, rows);
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::WorldStreamingDiagnosticSnapshot */
    WorldStreamingDiagnosticSnapshot::WorldStreamingDiagnosticSnapshot(WorldStreamingDiagnosticSnapshotInput input,
                                                                       std::optional<StreamingBudgetPolicy> policy,
                                                                       std::optional<StreamingBudgetSample> sample,
                                                                       std::vector<StreamingSourceDesiredState> sources,
                                                                       std::vector<StreamingCellStateRecord> cells,
                                                                       std::vector<StreamingDiagnosticFailureRecord> failures,
                                                                       std::vector<StreamingDiagnosticDecisionEvent> events) noexcept
        : input_(std::move(input)), policy_(std::move(policy)), sample_(std::move(sample)), sources_(std::move(sources)),
          cells_(std::move(cells)), failures_(std::move(failures)), events_(std::move(events)) {}

    /** @copydoc WorldStreamingDiagnosticSnapshot::Owner */
    const StreamingRuntimeOwnerToken &WorldStreamingDiagnosticSnapshot::Owner() const noexcept {
        return input_.owner;
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::Revision */
    WorldStreamingDiagnosticRevision WorldStreamingDiagnosticSnapshot::Revision() const noexcept {
        return input_.revision;
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::OwnerRevision */
    StreamingRuntimeCompositionRevision WorldStreamingDiagnosticSnapshot::OwnerRevision() const noexcept {
        return input_.ownerRevision;
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::Lifecycle */
    WorldStreamingRuntimeCompositionState WorldStreamingDiagnosticSnapshot::Lifecycle() const noexcept {
        return input_.lifecycle;
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::Queue */
    const StreamingDiagnosticQueueSnapshot &WorldStreamingDiagnosticSnapshot::Queue() const noexcept {
        return input_.queue;
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::BudgetPolicy */
    const StreamingBudgetPolicy *WorldStreamingDiagnosticSnapshot::BudgetPolicy() const noexcept {
        return policy_ ? &*policy_ : nullptr;
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::BudgetSample */
    const StreamingBudgetSample *WorldStreamingDiagnosticSnapshot::BudgetSample() const noexcept {
        return sample_ ? &*sample_ : nullptr;
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::Sources */
    std::span<const StreamingSourceDesiredState> WorldStreamingDiagnosticSnapshot::Sources() const noexcept {
        return sources_;
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::Cells */
    std::span<const StreamingCellStateRecord> WorldStreamingDiagnosticSnapshot::Cells() const noexcept {
        return cells_;
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::Failures */
    std::span<const StreamingDiagnosticFailureRecord> WorldStreamingDiagnosticSnapshot::Failures() const noexcept {
        return failures_;
    }

    /** @copydoc WorldStreamingDiagnosticSnapshot::Events */
    std::span<const StreamingDiagnosticDecisionEvent> WorldStreamingDiagnosticSnapshot::Events() const noexcept {
        return events_;
    }
}  // namespace Horo::WorldStreaming
