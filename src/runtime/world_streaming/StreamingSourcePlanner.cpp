#include "Horo/WorldStreaming/StreamingSourcePlanner.h"

#include "Horo/WorldStreaming/StreamingDesiredStateReduction.h"
#include "StreamingSourcePlannerInternal.h"

#include <algorithm>
#include <array>
#include <limits>

namespace Horo::WorldStreaming {
    using SourcePlanning::Failure;

    namespace {
        /** @brief Canonical min-heap order independent of source registration and BVH traversal order. */
        bool ContributionHeapLess(const StreamingPlannedSourceCell &left, const StreamingPlannedSourceCell &right) {
            if (left.source.id != right.source.id)
                return left.source.id > right.source.id;
            if (left.cell.slot != right.cell.slot)
                return left.cell.slot > right.cell.slot;
            return left.predicted && !right.predicted;
        }

        /** @brief Withdraws incomplete publication on any failed service attempt; ownership cannot be copied. */
        struct CycleAttempt final {
            StreamingSourcePlannerState &state;
            bool success{};

            explicit CycleAttempt(StreamingSourcePlannerState &owner) noexcept : state(owner) {}

            CycleAttempt(const CycleAttempt &) = delete;
            CycleAttempt &operator=(const CycleAttempt &) = delete;
            CycleAttempt(CycleAttempt &&) = delete;
            CycleAttempt &operator=(CycleAttempt &&) = delete;

            ~CycleAttempt() {
                if (!success) {
                    state.cycle.cycleActive = false;
                    state.publication.publishedRevision = 0;
                    state.cycle.cursor.reset();
                    state.cycle.queryShape.reset();
                    state.cycle.predicting = false;
                }
            }

            Result<StreamingSourcePlannerProgress> Complete(const StreamingSourcePlannerProgress &value) {
                success = true;
                return Result<StreamingSourcePlannerProgress>::Success(value);
            }
        };

        auto FindPrevious(const std::vector<StreamingPlannedCell> &rows, const StreamingCellId cell) {
            return std::ranges::lower_bound(rows, cell, StreamingCellCanonicalLess{}, [](const StreamingPlannedCell &row) {
                return row.cell.cell;
            });
        }
    }  // namespace

    Result<void> StreamingSourcePlannerState::CaptureSource(const std::size_t slot) {
        auto &source = sources[slot];
        source.captured = *source.request;
        source.capturedTime = lastServiceTime;
        source.capturedServiceTime = lastServiceTime;
        if (source.motion) {
            if (source.motion->sampledAtServiceMilliseconds > lastServiceTime ||
                lastServiceTime - source.motion->sampledAtServiceMilliseconds > config.limits.maximumPlanAgeMilliseconds)
                return Failure<void>(WorldStreamingErrors::PrefetchStale);
            const auto moved = SourcePlanning::Reposition(source.captured->shape, source.motion->position);
            if (moved.HasError())
                return Result<void>::Failure(moved.ErrorValue());
            source.captured->shape = moved.Value();
            source.capturedTime = source.motion->sampledAtServiceMilliseconds;
            if (source.captured->prediction)
                source.captured->prediction = source.motion;
        }
        cycle.oldestCycleSampleTime = std::min(cycle.oldestCycleSampleTime, source.capturedTime);
        return Result<void>::Success();
    }

    Result<void> StreamingSourcePlannerState::BeginSource(const std::size_t slot, const bool prediction) {
        auto &source = sources[slot];
        if (!prediction) {
            if (const auto captured = CaptureSource(slot); captured.HasError())
                return captured;
        }
        const auto &request = *source.captured;
        cycle.queryShape = request.shape;
        if (prediction) {
            if (!request.prediction) {
                cycle.queryShape.reset();
                return Result<void>::Success();
            }
            const auto descriptor = Descriptor(slot);
            const StreamingSourceAdmissionContext admission{descriptor.owner,
                                                            {},
                                                            0,
                                                            config.limits.sources,
                                                            StreamingSourceOwnerState::Active};
            const StreamingPrefetchEvaluationContext context{config.prefetch.Id(),       config.prefetch.Revision(),
                                                             descriptor.owner.partition, descriptor.owner.epoch,
                                                             source.capturedServiceTime, admission,
                                                             config.shapeSupport,        StreamingPrefetchLifecycle::Active};
            const auto &sample = *request.prediction;
            const auto result =
                EvaluateStreamingVelocityPrefetch(config.prefetch, context,
                                                  {descriptor, sample.position, sample.velocity, sample.sampledAtServiceMilliseconds});
            if (result.HasError())
                return Result<void>::Failure(result.ErrorValue());
            if (!result.Value().path) {
                cycle.queryShape.reset();
                return Result<void>::Success();
            }
            cycle.queryShape = *result.Value().path;
        }
        const auto bounds = StreamingSourceQueryBounds(*cycle.queryShape, config.stability.ExitMarginMillimeters());
        if (bounds.HasError())
            return Result<void>::Failure(bounds.ErrorValue());
        const auto query = partition.BeginQuery({bounds.Value(), request.selection.layer, request.selection.lod});
        if (query.HasError())
            return Result<void>::Failure(query.ErrorValue());
        cycle.cursor = query.Value();
        return Result<void>::Success();
    }

    Result<void> StreamingSourcePlannerState::AdmitContribution(const std::size_t slot, const WorldPartitionCellHandle &cell,
                                                                const StreamingSourceDescriptor &descriptor, const bool prediction) {
        const auto &request = *sources[slot].request;
        if (stagedContributions.size() == config.limits.contributions)
            return Failure<void>(WorldStreamingErrors::SourceReductionCapacityExceeded);
        const StreamingPlannedSourceCell contribution{cell,
                                                      descriptor,
                                                      prediction ? std::min(request.residency, StreamingDesiredResidency::Loaded)
                                                                 : request.residency,
                                                      prediction ? StreamingRetention::Releasable : request.retention,
                                                      prediction,
                                                      sources[slot].capturedTime};
        stagedContributions.push_back(contribution);
        std::ranges::push_heap(stagedContributions, ContributionHeapLess);
        return Result<void>::Success();
    }

    Result<void> StreamingSourcePlannerState::Accumulate(const std::size_t slot, const WorldPartitionCellHandle &cell,
                                                         const StreamingSourceShape &shape, const bool prediction) {
        if (!std::ranges::binary_search(enabledLayers, cell.cell.layer))
            return Result<void>::Success();
        const auto &request = *sources[slot].request;
        const auto boundary =
            EvaluateStreamingSourceCellBoundary(shape, *partition.Grid(), cell.cell, config.stability.EnterMarginMillimeters(),
                                                config.stability.ExitMarginMillimeters());
        if (boundary.HasError())
            return Result<void>::Failure(boundary.ErrorValue());
        if (boundary.Value() < -config.stability.ExitMarginMillimeters())
            return Result<void>::Success();
        const auto descriptor = Descriptor(slot);
        if (boundary.Value() >= 0) {
            if (const auto added = AdmitContribution(slot, cell, descriptor, prediction); added.HasError())
                return added;
        }
        StreamingPlannedCell value;
        value.cell = cell;
        value.boundaryThresholdMillimeters = std::numeric_limits<std::int64_t>::min();
        const auto previous = FindPrevious(published, cell.cell);
        value.queuedAtServiceMilliseconds =
            previous != published.end() && previous->cell.cell == cell.cell ? previous->queuedAtServiceMilliseconds : cycle.cycleTime;
        const auto admitted = AdmitRow(value);
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        auto *found = &staged[admitted.Value()];
        const auto residency = prediction ? std::min(request.residency, StreamingDesiredResidency::Loaded) : request.residency;
        if (residency > found->residency) {
            found->residency = residency;
            found->boundaryThresholdMillimeters = boundary.Value();
        } else if (residency == found->residency) {
            found->boundaryThresholdMillimeters = std::max(found->boundaryThresholdMillimeters, boundary.Value());
        }
        const auto desired =
            StreamingSourceDesiredState::Create(descriptor, residency,
                                                !prediction && boundary.Value() >= 0 ? request.retention : StreamingRetention::Releasable);
        if (desired.HasError())
            return Result<void>::Failure(desired.ErrorValue());
        const auto summary = CombineStreamingDesiredState({found->residency, found->pinnedFloor}, desired.Value());
        found->residency = summary.effectiveResidency;
        found->pinnedFloor = summary.pinnedResidencyFloor;
        return RankRow(slot, *found, descriptor);
    }

    Result<void> StreamingSourcePlannerState::RankRow(const std::size_t slot, StreamingPlannedCell &row,
                                                      const StreamingSourceDescriptor &descriptor) {
        const auto &cell = row.cell;
        const auto distance = StreamingSourceCellDistance(sources[slot].captured->shape, *partition.Grid(), cell.cell);
        if (distance.HasError())
            return Result<void>::Failure(distance.ErrorValue());
        const StreamingCellPriorityCandidate candidate{descriptor, cell.cell, distance.Value(), 1.0, row.queuedAtServiceMilliseconds};
        const StreamingPriorityEvaluationContext context{config.priority.Id(),
                                                         config.priority.Revision(),
                                                         descriptor.owner.partition,
                                                         descriptor.owner.epoch,
                                                         cycle.cycleTime,
                                                         StreamingPriorityPolicyState::Active};
        std::array<StreamingRankedCellPriority, 1> ranked;
        if (const auto result = RankStreamingCellPriorities(config.priority, context, std::span{&candidate, 1}, ranked); result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        if (!row.prioritySource.has_value() || ranked[0].score > row.priority ||
            (ranked[0].score == row.priority && descriptor.id < row.prioritySource->id)) {
            row.priority = ranked[0].score;
            row.prioritySource = descriptor;
        }
        return Result<void>::Success();
    }

    bool StreamingSourcePlannerState::HeapLess(const std::uint32_t left, const std::uint32_t right) const noexcept {
        return staged[left].cell.slot > staged[right].cell.slot;
    }

    Result<std::size_t> StreamingSourcePlannerState::AdmitRow(const StreamingPlannedCell &row) {
        auto &entry = lookup[row.cell.slot];
        if (entry.cycle == cycle.cycleSequence)
            return Result<std::size_t>::Success(entry.row);
        if (staged.size() == config.limits.cells)
            return Failure<std::size_t>(WorldStreamingErrors::SourceCapacityExceeded);
        entry = {cycle.cycleSequence, static_cast<std::uint32_t>(staged.size())};
        staged.push_back(row);
        readyHeap.push_back(entry.row);
        std::ranges::push_heap(readyHeap, [this](auto left, auto right) {
            return HeapLess(left, right);
        });
        return Result<std::size_t>::Success(entry.row);
    }

    Result<void> StreamingSourcePlannerState::Stabilize(StreamingPlannedCell &row, const StreamingCellStabilityPressure pressure) {
        const auto previous = FindPrevious(published, row.cell.cell);
        std::optional<StreamingCellStabilitySnapshot> snapshot;
        if (previous != published.end() && previous->cell.cell == row.cell.cell)
            snapshot = previous->stability;
        const auto &world = partition.Binding().owner;
        const StreamingCellStabilityContext context{config.stability.Id(),
                                                    config.stability.Revision(),
                                                    world.partition,
                                                    world.epoch,
                                                    cycle.cycleTime,
                                                    static_cast<std::uint32_t>(staged.size() - (snapshot ? 0 : 1)),
                                                    StreamingCellStabilityLifecycle::Active,
                                                    pressure};
        const StreamingCellStabilityObservation observation{row.cell.cell, row.residency, row.pinnedFloor,
                                                            row.boundaryThresholdMillimeters};
        const auto result = EvaluateStreamingCellStability(config.stability, context, observation, snapshot);
        if (result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        row.stability = result.Value().snapshot;
        row.residency = row.stability.retainedResidency;
        return Result<void>::Success();
    }

    void StreamingSourcePlannerState::CompleteSourceTraversal() {
        cycle.cursor.reset();
        cycle.queryShape.reset();
        if (cycle.predicting) {
            cycle.predicting = false;
            ++cycle.sourceSlot;
        } else
            cycle.predicting = true;
    }

    /** @copydoc StreamingSourcePlannerState::StepCursor */
    Result<void> StreamingSourcePlannerState::StepCursor(const std::size_t slot, StreamingSourcePlannerProgress &progress) {
        const auto limits = config.limits;
        std::array<WorldPartitionCellHandle, 1> cell;
        const auto batch = partition.QueryNext(*cycle.cursor,
                                               {static_cast<std::uint32_t>(limits.nodesPerFrame - progress.nodesVisited),
                                                static_cast<std::uint32_t>(limits.candidatesPerFrame - progress.candidatesExamined)},
                                               cell);
        if (batch.HasError())
            return Result<void>::Failure(batch.ErrorValue());
        progress.nodesVisited += batch.Value().nodesVisited;
        progress.candidatesExamined += batch.Value().candidatesExamined;
        if (batch.Value().matches != 0) {
            if (const auto added = Accumulate(slot, cell[0], *cycle.queryShape, cycle.predicting); added.HasError())
                return Result<void>::Failure(added.ErrorValue());
        }
        if (batch.Value().complete)
            CompleteSourceTraversal();
        return Result<void>::Success();
    }

    Result<void> StreamingSourcePlannerState::StepSources(StreamingSourcePlannerProgress &progress) {
        auto &state = *this;
        const auto limits = config.limits;
        while (state.cycle.sourceSlot < state.sourceOrder.size()) {
            const auto slot = state.sourceOrder[state.cycle.sourceSlot];
            if (!state.cycle.cursor) {
                if (progress.sourceVisits == limits.sourceVisitsPerFrame)
                    return Result<void>::Success();
                ++progress.sourceVisits;
                if (const auto &source = state.sources[slot]; !source.request || !state.Selected(*source.request) ||
                                                              source.request->residency == StreamingDesiredResidency::Unloaded) {
                    ++state.cycle.sourceSlot;
                    continue;
                }
                if (const auto begun = state.BeginSource(slot, state.cycle.predicting); begun.HasError())
                    return Result<void>::Failure(begun.ErrorValue());
                if (!state.cycle.cursor) {
                    state.cycle.predicting = false;
                    ++state.cycle.sourceSlot;
                    continue;
                }
            }
            if (progress.nodesVisited == limits.nodesPerFrame || progress.candidatesExamined == limits.candidatesPerFrame)
                break;
            if (const auto advanced = state.StepCursor(slot, progress); advanced.HasError())
                return advanced;
        }
        return Result<void>::Success();
    }

    Result<void> StreamingSourcePlannerState::StepHistory(StreamingSourcePlannerProgress &progress) {
        auto &state = *this;
        const auto limits = config.limits;
        // Only bounded previous demand is visited. No manifest or active source registry scan occurs here.
        while (state.cycle.historySlot < state.published.size() && progress.stabilityRows < limits.stabilityRowsPerFrame) {
            auto old = state.published[state.cycle.historySlot++];
            ++progress.stabilityRows;
            if (!std::ranges::binary_search(state.enabledLayers, old.cell.cell.layer))
                continue;
            if (state.lookup[old.cell.slot].cycle != state.cycle.cycleSequence) {
                old.residency = StreamingDesiredResidency::Unloaded;
                old.pinnedFloor.reset();
                old.prioritySource.reset();
                old.priority = 0;
                old.boundaryThresholdMillimeters = -state.config.stability.ExitMarginMillimeters() - 1;
                const auto admitted = state.AdmitRow(old);
                if (admitted.HasError())
                    return Result<void>::Failure(admitted.ErrorValue());
            }
        }
        return Result<void>::Success();
    }

    Result<void> StreamingSourcePlannerState::StepOutput(StreamingSourcePlannerProgress &progress) {
        auto &state = *this;
        const auto limits = config.limits;
        while (!state.readyHeap.empty() && progress.stabilityRows < limits.stabilityRowsPerFrame) {
            std::ranges::pop_heap(state.readyHeap, [&state](auto left, auto right) {
                return state.HeapLess(left, right);
            });
            const auto index = state.readyHeap.back();
            state.readyHeap.pop_back();
            auto &row = state.staged[index];
            ++progress.stabilityRows;
            if (const auto stable = state.Stabilize(row, state.cycle.cyclePressure); stable.HasError())
                return Result<void>::Failure(stable.ErrorValue());
            if (row.stability.phase != StreamingCellStabilityPhase::Unloaded)
                state.nextPublished.push_back(row);
        }
        while (state.readyHeap.empty() && !state.stagedContributions.empty() && progress.stabilityRows < limits.stabilityRowsPerFrame) {
            ++progress.stabilityRows;
            std::ranges::pop_heap(state.stagedContributions, ContributionHeapLess);
            state.nextPublishedContributions.push_back(state.stagedContributions.back());
            state.stagedContributions.pop_back();
        }
        return Result<void>::Success();
    }

    void StreamingSourcePlannerState::BeginCycle(const std::uint64_t time, const StreamingCellStabilityPressure pressure) noexcept {
        ++cycle.cycleSequence;
        cycle.cyclePressure = pressure;
        readyHeap.clear();
        nextPublished.clear();
        nextPublishedContributions.clear();
        cycle.cycleActive = true;
        cycle.cycleTime = time;
        cycle.oldestCycleSampleTime = time;
        cycle.sourceSlot = 0;
        cycle.historySlot = 0;
        staged.clear();
        stagedContributions.clear();
    }

    /** @copydoc StreamingSourcePlanner::Service */
    Result<StreamingSourcePlannerProgress> StreamingSourcePlanner::Service(const std::uint64_t serviceTimeMilliseconds,
                                                                           const StreamingCellStabilityPressure pressure) {
        auto *owner = OwnerState();
        if (!owner || owner->closed)
            return Failure<StreamingSourcePlannerProgress>(WorldStreamingErrors::SourceLifecycleUnavailable);
        auto &state = *owner;
        if (serviceTimeMilliseconds < state.lastServiceTime)
            return Failure<StreamingSourcePlannerProgress>(WorldStreamingErrors::CellStabilityStale);
        if (pressure >= StreamingCellStabilityPressure::Count)
            return Failure<StreamingSourcePlannerProgress>(WorldStreamingErrors::CellStabilityUnsupported);
        state.lastServiceTime = serviceTimeMilliseconds;
        CycleAttempt guard{state};
        if (!state.cycle.cycleActive) {
            if (state.cycle.cycleSequence == std::numeric_limits<std::uint64_t>::max())
                return Failure<StreamingSourcePlannerProgress>(WorldStreamingErrors::GenerationExhausted);
            state.BeginCycle(serviceTimeMilliseconds, pressure);
        }
        StreamingSourcePlannerProgress progress;
        progress.revision = state.publication.planSequence;
        if (const auto sources = state.StepSources(progress); sources.HasError())
            return Result<StreamingSourcePlannerProgress>::Failure(sources.ErrorValue());
        if (state.cycle.sourceSlot != state.sourceOrder.size())
            return guard.Complete(progress);
        if (const auto history = state.StepHistory(progress); history.HasError())
            return Result<StreamingSourcePlannerProgress>::Failure(history.ErrorValue());
        if (state.cycle.historySlot != state.published.size())
            return guard.Complete(progress);
        if (const auto output = state.StepOutput(progress); output.HasError())
            return Result<StreamingSourcePlannerProgress>::Failure(output.ErrorValue());
        if (state.readyHeap.empty() && state.stagedContributions.empty()) {
            if (serviceTimeMilliseconds - state.cycle.oldestCycleSampleTime > state.config.limits.maximumPlanAgeMilliseconds)
                return Failure<StreamingSourcePlannerProgress>(WorldStreamingErrors::PrefetchStale);
            if (state.publication.planSequence == std::numeric_limits<std::uint64_t>::max())
                return Failure<StreamingSourcePlannerProgress>(WorldStreamingErrors::GenerationExhausted);
            ++state.publication.planSequence;
            state.published.swap(state.nextPublished);
            state.publishedContributions.swap(state.nextPublishedContributions);
            state.publication.publishedRevision = state.revision;
            state.publication.publishedTime = state.cycle.oldestCycleSampleTime;
            state.cycle.cycleActive = false;
            progress.published = true;
            progress.revision = state.publication.planSequence;
        }
        return guard.Complete(progress);
    }

    /** @copydoc StreamingSourcePlanner::Plan */
    Result<StreamingSourcePlanView> StreamingSourcePlanner::Plan() const {
        if (!state_->OnLane() || state_->closed)
            return Failure<StreamingSourcePlanView>(WorldStreamingErrors::SourceLifecycleUnavailable);
        if (state_->publication.publishedRevision == 0 || state_->publication.publishedRevision != state_->revision ||
            state_->lastServiceTime - state_->publication.publishedTime > state_->config.limits.maximumPlanAgeMilliseconds)
            return Failure<StreamingSourcePlanView>(WorldStreamingErrors::PartitionRegistryUnavailable);
        return Result<StreamingSourcePlanView>::Success({state_->partition.Binding(), state_->publication.planSequence,
                                                         state_->publication.publishedTime, state_->published,
                                                         state_->publishedContributions});
    }
}  // namespace Horo::WorldStreaming
