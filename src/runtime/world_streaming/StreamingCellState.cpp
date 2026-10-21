#include "Horo/WorldStreaming/StreamingCellState.h"

#include "WorldStreamingInternal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        using Internal::Failure;

        /** @brief Checks whether a residency-state value belongs to the canonical six-state contract. */
        [[nodiscard]] bool KnownState(const StreamingCellState state) noexcept {
            return state >= StreamingCellState::Unloaded && state <= StreamingCellState::Failed;
        }

        /** @brief Reports whether a state may still own cell-scoped resources or live publication. */
        [[nodiscard]] bool RetainsResources(const StreamingCellState state) noexcept {
            using enum StreamingCellState;
            return state == Loading || state == Resident || state == Active || state == Evicting;
        }

        /** @brief Projects a retirement-acknowledged terminal operation onto canonical residency. */
        [[nodiscard]] Result<StreamingCellState> ProjectTerminalState(const StreamingCellOperation &operation) {
            using enum StreamingCellOperationKind;
            using enum StreamingCellOperationOutcome;

            if (operation.Outcome() == Failed)
                return Result<StreamingCellState>::Success(StreamingCellState::Failed);
            if (operation.Outcome() == Cancelled || operation.Outcome() == Replaced || operation.Outcome() == Shutdown)
                return Result<StreamingCellState>::Success(StreamingCellState::Unloaded);
            if (operation.Outcome() != Succeeded)
                return Failure<StreamingCellState>(WorldStreamingErrors::CellStateUnsupported);
            constexpr std::array SuccessStates{StreamingCellState::Resident, StreamingCellState::Active, StreamingCellState::Unloaded};
            return Result<StreamingCellState>::Success(SuccessStates[static_cast<std::size_t>(operation.Kind())]);
        }

        /** @brief Projects a non-terminal admitted operation while provider phases remain separate. */
        [[nodiscard]] Result<StreamingCellState> ProjectProgressState(const StreamingCellOperation &operation) {
            using enum StreamingCellOperationKind;
            using enum StreamingCellOperationState;
            if (operation.Kind() == Load && (operation.State() == Admitted || operation.State() == Preparing))
                return Result<StreamingCellState>::Success(StreamingCellState::Loading);
            if (operation.Kind() == Activate &&
                (operation.State() == Admitted || operation.State() == Preparing || operation.State() == Activating))
                return Result<StreamingCellState>::Success(StreamingCellState::Resident);
            return Failure<StreamingCellState>(WorldStreamingErrors::CellStateUnsupported);
        }

        /** @brief Projects a canonical operation snapshot onto the six-state residency model. */
        [[nodiscard]] Result<StreamingCellState> ProjectState(const StreamingCellOperation &operation) {
            if (operation.State() == StreamingCellOperationState::Retiring)
                return Result<StreamingCellState>::Success(StreamingCellState::Evicting);
            return operation.State() == StreamingCellOperationState::Terminal ? ProjectTerminalState(operation)
                                                                              : ProjectProgressState(operation);
        }

        /** @brief Reports whether an operation with a fresh identity may take over an existing residency record. */
        [[nodiscard]] bool CanStartOperation(const StreamingCellState current, const StreamingCellOperation &operation,
                                             const StreamingCellState projected) noexcept {
            using enum StreamingCellOperationKind;
            using enum StreamingCellState;
            if (current == Resident)
                return operation.Kind() == Activate || (operation.Kind() == Retire && projected == Evicting);
            return current == Active && operation.Kind() == Retire && projected == Evicting;
        }

        /** @brief Checks one projected residency transition without changing the tracked record. */
        [[nodiscard]] bool CanTransition(const StreamingCellState current, const StreamingCellState projected,
                                         const bool sameOperation) noexcept {
            using enum StreamingCellState;
            if (current == projected)
                return sameOperation || current == Resident;
            if (current == Evicting && !sameOperation)
                return false;
            using Edge = std::pair<StreamingCellState, StreamingCellState>;
            constexpr std::array AllowedEdges{Edge{Loading, Resident},  Edge{Loading, Evicting}, Edge{Resident, Active},
                                              Edge{Resident, Evicting}, Edge{Active, Evicting},  Edge{Evicting, Unloaded},
                                              Edge{Evicting, Failed}};
            return std::ranges::find(AllowedEdges, Edge{current, projected}) != AllowedEdges.end();
        }

        /** @brief Finds an exact attempt fence while preserving the storage's const qualification. */
        template <typename Records> [[nodiscard]] auto FindExact(Records &records, const StreamingFence &fence) {
            return std::ranges::find_if(records, [&fence](const StreamingCellStateRecord &record) {
                return record.operation.fence == fence;
            });
        }

        /** @brief Checks whether two fences address the same mounted cell independently of generation. */
        [[nodiscard]] bool SameMountedCell(const StreamingFence &left, const StreamingFence &right) noexcept {
            return left.partition == right.partition && left.epoch == right.epoch && left.cell == right.cell;
        }

        /** @brief Reports whether any retained generation belongs to the same mounted cell. */
        [[nodiscard]] bool HasMountedCell(const std::vector<StreamingCellStateRecord> &records, const StreamingFence &fence) {
            return std::ranges::any_of(records, [&fence](const auto &candidate) {
                return SameMountedCell(candidate.operation.fence, fence);
            });
        }

        /** @brief Applies a projected snapshot to one exact tracked generation. */
        [[nodiscard]] Result<StreamingCellStateRecord> ApplyExisting(std::vector<StreamingCellStateRecord> &records,
                                                                     std::vector<StreamingCellStateRecord>::iterator exact,
                                                                     const StreamingCellOperation &operation,
                                                                     const StreamingCellState projected) {
            const bool sameOperation = exact->operation.operation == operation.Handle().operation;
            if (!sameOperation && operation.Kind() == StreamingCellOperationKind::Load)
                return Failure<StreamingCellStateRecord>(WorldStreamingErrors::CellStateStale);
            if (!sameOperation && !CanStartOperation(exact->state, operation, projected))
                return Failure<StreamingCellStateRecord>(WorldStreamingErrors::CellStateTransitionInvalid);
            if (!CanTransition(exact->state, projected, sameOperation))
                return Failure<StreamingCellStateRecord>(WorldStreamingErrors::CellStateTransitionInvalid);

            exact->operation = operation.Handle();
            exact->state = projected;
            const StreamingCellStateRecord result = *exact;
            const bool terminalResidency = projected == StreamingCellState::Unloaded || projected == StreamingCellState::Failed;
            if (const bool newerAttemptExists = terminalResidency && std::ranges::any_of(records,
                                                                                         [&result](const auto &candidate) {
                return SameMountedCell(candidate.operation.fence, result.operation.fence) &&
                       candidate.operation.fence.generation.Value() > result.operation.fence.generation.Value();
            });
                newerAttemptExists)
                records.erase(exact);
            return Result<StreamingCellStateRecord>::Success(result);
        }

        /** @brief Finds a reusable terminal slot while validating same-cell generation ordering. */
        [[nodiscard]] Result<std::size_t> FindReusableSlot(const std::vector<StreamingCellStateRecord> &records,
                                                           const StreamingFence &fence) {
            using enum StreamingCellState;
            std::size_t reusable = std::numeric_limits<std::size_t>::max();
            for (std::size_t index = 0; index < records.size(); ++index) {
                const auto &candidate = records[index];
                if (!SameMountedCell(candidate.operation.fence, fence))
                    continue;
                if (fence.generation.Value() <= candidate.operation.fence.generation.Value())
                    return Failure<std::size_t>(WorldStreamingErrors::CellStateStale);
                if (candidate.state == Unloaded || candidate.state == Failed)
                    reusable = index;
                else if (candidate.state != Evicting)
                    return Failure<std::size_t>(WorldStreamingErrors::CellStateTransitionInvalid);
            }
            return Result<std::size_t>::Success(reusable);
        }

        /** @brief Admits the first residency snapshot for a fresh generation without partial mutation. */
        [[nodiscard]] Result<StreamingCellStateRecord> ApplyFresh(std::vector<StreamingCellStateRecord> &records,
                                                                  const std::uint32_t maximumTrackedAttempts,
                                                                  const StreamingCellOperation &operation,
                                                                  const StreamingCellState projected) {
            if (operation.Kind() != StreamingCellOperationKind::Load || projected != StreamingCellState::Loading)
                return Failure<StreamingCellStateRecord>(HasMountedCell(records, operation.Handle().fence)
                                                             ? WorldStreamingErrors::CellStateStale
                                                             : WorldStreamingErrors::CellStateUnresolved);

            const auto reusable = FindReusableSlot(records, operation.Handle().fence);
            if (reusable.HasError())
                return Result<StreamingCellStateRecord>::Failure(reusable.ErrorValue());

            const StreamingCellStateRecord admitted{.operation = operation.Handle(), .state = StreamingCellState::Loading};
            if (reusable.Value() != std::numeric_limits<std::size_t>::max()) {
                records[reusable.Value()] = admitted;
                return Result<StreamingCellStateRecord>::Success(admitted);
            }
            if (records.size() >= maximumTrackedAttempts)
                return Failure<StreamingCellStateRecord>(WorldStreamingErrors::CellStateCapacityExceeded);
            records.push_back(admitted);
            return Result<StreamingCellStateRecord>::Success(admitted);
        }
    }  // namespace

    /** @copydoc StreamingCellStateRecord::IsValid */
    bool StreamingCellStateRecord::IsValid() const noexcept {
        return operation.IsValid() && KnownState(state);
    }

    /** @copydoc StreamingCellStateLedgerConfig::IsValid */
    bool StreamingCellStateLedgerConfig::IsValid() const noexcept {
        return owner.IsValid() && maximumTrackedAttempts > 0 && maximumTrackedAttempts <= MaximumTrackedAttempts;
    }

    StreamingCellStateLedger::StreamingCellStateLedger(const StreamingCellStateLedgerConfig &config,
                                                       std::vector<StreamingCellStateRecord> records) noexcept
        : config_(config), records_(std::move(records)) {}

    /** @copydoc StreamingCellStateLedger::StreamingCellStateLedger */
    StreamingCellStateLedger::StreamingCellStateLedger(StreamingCellStateLedger &&other) noexcept
        : config_(other.config_), records_(std::move(other.records_)), lifecycle_(other.lifecycle_) {
        other.lifecycle_ = StreamingCellStateLedgerLifecycle::Closed;
    }

    /** @copydoc StreamingCellStateLedger::Create */
    Result<StreamingCellStateLedger> StreamingCellStateLedger::Create(const StreamingCellStateLedgerConfig &config) {
        if (!config.IsValid())
            return Failure<StreamingCellStateLedger>(WorldStreamingErrors::CellStateInvalid);
        try {
            std::vector<StreamingCellStateRecord> records;
            records.reserve(config.maximumTrackedAttempts);
            return Result<StreamingCellStateLedger>::Success(StreamingCellStateLedger{config, std::move(records)});
        } catch (const std::bad_alloc &) {
            return Failure<StreamingCellStateLedger>(WorldStreamingErrors::CellStateCapacityExceeded);
        } catch (const std::length_error &) {
            return Failure<StreamingCellStateLedger>(WorldStreamingErrors::CellStateCapacityExceeded);
        }
    }

    /** @copydoc StreamingCellStateLedger::Apply */
    Result<StreamingCellStateRecord> StreamingCellStateLedger::Apply(const StreamingRuntimeOwnerToken &owner,
                                                                     const StreamingCellOperation &operation) {
        if (!owner.IsValid())
            return Failure<StreamingCellStateRecord>(WorldStreamingErrors::CellStateInvalid);
        if (owner != config_.owner || operation.Handle().fence.partition != owner.partition ||
            operation.Handle().fence.epoch != owner.epoch)
            return Failure<StreamingCellStateRecord>(WorldStreamingErrors::CellStateStale);

        const auto projectedResult = ProjectState(operation);
        if (projectedResult.HasError())
            return Result<StreamingCellStateRecord>::Failure(projectedResult.ErrorValue());
        const StreamingCellState projected = projectedResult.Value();
        if (lifecycle_ != StreamingCellStateLedgerLifecycle::Active && RetainsResources(projected) &&
            projected != StreamingCellState::Evicting)
            return Failure<StreamingCellStateRecord>(WorldStreamingErrors::CellStateLifecycleUnavailable);

        auto exact = FindExact(records_, operation.Handle().fence);
        return exact == records_.end() ? ApplyFresh(records_, config_.maximumTrackedAttempts, operation, projected)
                                       : ApplyExisting(records_, exact, operation, projected);
    }

    /** @copydoc StreamingCellStateLedger::Resolve */
    Result<StreamingCellStateRecord> StreamingCellStateLedger::Resolve(const StreamingRuntimeOwnerToken &owner,
                                                                       const StreamingFence &fence) const {
        if (!owner.IsValid() || !fence.IsValid())
            return Failure<StreamingCellStateRecord>(WorldStreamingErrors::CellStateInvalid);
        if (owner != config_.owner || fence.partition != owner.partition || fence.epoch != owner.epoch)
            return Failure<StreamingCellStateRecord>(WorldStreamingErrors::CellStateStale);
        if (const auto found = FindExact(records_, fence); found != records_.end())
            return Result<StreamingCellStateRecord>::Success(*found);
        return Failure<StreamingCellStateRecord>(HasMountedCell(records_, fence) ? WorldStreamingErrors::CellStateStale
                                                                                 : WorldStreamingErrors::CellStateUnresolved);
    }

    /** @copydoc StreamingCellStateLedger::BeginShutdown */
    Result<void> StreamingCellStateLedger::BeginShutdown(const StreamingRuntimeOwnerToken &owner) noexcept {
        if (!owner.IsValid())
            return Failure<void>(WorldStreamingErrors::CellStateInvalid);
        if (owner != config_.owner)
            return Failure<void>(WorldStreamingErrors::CellStateStale);
        if (Lifecycle() != StreamingCellStateLedgerLifecycle::Closed)
            lifecycle_ = StreamingCellStateLedgerLifecycle::Draining;
        return Result<void>::Success();
    }

    /** @copydoc StreamingCellStateLedger::Owner */
    const StreamingRuntimeOwnerToken &StreamingCellStateLedger::Owner() const noexcept {
        return config_.owner;
    }

    /** @copydoc StreamingCellStateLedger::Lifecycle */
    StreamingCellStateLedgerLifecycle StreamingCellStateLedger::Lifecycle() const noexcept {
        if (lifecycle_ == StreamingCellStateLedgerLifecycle::Draining &&
            std::ranges::none_of(records_, [](const StreamingCellStateRecord &record) {
            return RetainsResources(record.state);
        }))
            return StreamingCellStateLedgerLifecycle::Closed;
        return lifecycle_;
    }

    /** @copydoc StreamingCellStateLedger::TrackedAttemptCount */
    std::size_t StreamingCellStateLedger::TrackedAttemptCount() const noexcept {
        return records_.size();
    }

    /** @copydoc StreamingCellStateLedger::Records */
    std::span<const StreamingCellStateRecord> StreamingCellStateLedger::Records() const noexcept {
        return records_;
    }
}  // namespace Horo::WorldStreaming
