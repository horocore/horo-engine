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
            return left.predicted > right.predicted;
        }

        struct CycleAttempt final {
            StreamingSourcePlannerState &state;
            bool success{};

            ~CycleAttempt() {
                if (!success) {
                    state.cycleActive = false;
                    state.publishedRevision = 0;
                    state.cursor.reset();
                    state.queryShape.reset();
                    state.predicting = false;
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
        oldestCycleSampleTime = std::min(oldestCycleSampleTime, source.capturedTime);
        return Result<void>::Success();
    }

    Result<void> StreamingSourcePlannerState::BeginSource(const std::size_t slot, const bool prediction) {
        auto &source = sources[slot];
        if (!prediction) {
            if (const auto captured = CaptureSource(slot); captured.HasError())
                return captured;
        }
        const auto &request = *source.captured;
        queryShape = request.shape;
        if (prediction) {
            if (!request.prediction) {
                queryShape.reset();
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
                queryShape.reset();
                return Result<void>::Success();
            }
            queryShape = *result.Value().path;
        }
        const auto bounds = StreamingSourceQueryBounds(*queryShape, config.stability.ExitMarginMillimeters());
        if (bounds.HasError())
            return Result<void>::Failure(bounds.ErrorValue());
        const auto query = partition.BeginQuery({bounds.Value(), request.selection.layer, request.selection.lod});
        if (query.HasError())
            return Result<void>::Failure(query.ErrorValue());
        cursor = query.Value();
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
        std::push_heap(stagedContributions.begin(), stagedContributions.end(), ContributionHeapLess);
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
            previous != published.end() && previous->cell.cell == cell.cell ? previous->queuedAtServiceMilliseconds : cycleTime;
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
                                                         cycleTime,
                                                         StreamingPriorityPolicyState::Active};
        std::array<StreamingRankedCellPriority, 1> ranked;
        const auto result = RankStreamingCellPriorities(config.priority, context, std::span{&candidate, 1}, ranked);
        if (result.HasError())
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
        if (entry.cycle == cycleSequence)
            return Result<std::size_t>::Success(entry.row);
        if (staged.size() == config.limits.cells)
            return Failure<std::size_t>(WorldStreamingErrors::SourceCapacityExceeded);
        entry = {cycleSequence, static_cast<std::uint32_t>(staged.size())};
        staged.push_back(row);
        readyHeap.push_back(entry.row);
        std::push_heap(readyHeap.begin(), readyHeap.end(), [this](auto left, auto right) {
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
                                                    cycleTime,
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
        cursor.reset();
        queryShape.reset();
        if (predicting) {
            predicting = false;
            ++sourceSlot;
        } else
            predicting = true;
    }

    Result<void> StreamingSourcePlannerState::StepSources(StreamingSourcePlannerProgress &progress) {
        auto &state = *this;
        const auto limits = config.limits;
        while (state.sourceSlot < state.sourceOrder.size()) {
            const auto slot = state.sourceOrder[state.sourceSlot];
            if (!state.cursor) {
                if (progress.sourceVisits == limits.sourceVisitsPerFrame)
                    break;
                ++progress.sourceVisits;
                const auto &source = state.sources[slot];
                if (!source.request || !state.Selected(*source.request) ||
                    source.request->residency == StreamingDesiredResidency::Unloaded) {
                    ++state.sourceSlot;
                    continue;
                }
                if (const auto begun = state.BeginSource(slot, state.predicting); begun.HasError())
                    return Result<void>::Failure(begun.ErrorValue());
                if (!state.cursor) {
                    state.predicting = false;
                    ++state.sourceSlot;
                    continue;
                }
            }
            if (progress.nodesVisited == limits.nodesPerFrame || progress.candidatesExamined == limits.candidatesPerFrame)
                break;
            std::array<WorldPartitionCellHandle, 1> cell;
            const auto batch =
                state.partition.QueryNext(*state.cursor,
                                          {static_cast<std::uint32_t>(limits.nodesPerFrame - progress.nodesVisited),
                                           static_cast<std::uint32_t>(limits.candidatesPerFrame - progress.candidatesExamined)},
                                          cell);
            if (batch.HasError())
                return Result<void>::Failure(batch.ErrorValue());
            progress.nodesVisited += batch.Value().nodesVisited;
            progress.candidatesExamined += batch.Value().candidatesExamined;
            if (batch.Value().matches != 0) {
                if (const auto added = state.Accumulate(slot, cell[0], *state.queryShape, state.predicting); added.HasError())
                    return Result<void>::Failure(added.ErrorValue());
            }
            if (batch.Value().complete)
                state.CompleteSourceTraversal();
        }
        return Result<void>::Success();
    }

    Result<void> StreamingSourcePlannerState::StepHistory(StreamingSourcePlannerProgress &progress) {
        auto &state = *this;
        const auto limits = config.limits;
        // Only bounded previous demand is visited. No manifest or active source registry scan occurs here.
        while (state.historySlot < state.published.size() && progress.stabilityRows < limits.stabilityRowsPerFrame) {
            auto old = state.published[state.historySlot++];
            ++progress.stabilityRows;
            if (!std::ranges::binary_search(state.enabledLayers, old.cell.cell.layer))
                continue;
            if (state.lookup[old.cell.slot].cycle != state.cycleSequence) {
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
            std::pop_heap(state.readyHeap.begin(), state.readyHeap.end(), [&state](auto left, auto right) {
                return state.HeapLess(left, right);
            });
            const auto index = state.readyHeap.back();
            state.readyHeap.pop_back();
            auto &row = state.staged[index];
            ++progress.stabilityRows;
            if (const auto stable = state.Stabilize(row, state.cyclePressure); stable.HasError())
                return Result<void>::Failure(stable.ErrorValue());
            if (row.stability.phase != StreamingCellStabilityPhase::Unloaded)
                state.nextPublished.push_back(row);
        }
        while (state.readyHeap.empty() && !state.stagedContributions.empty() && progress.stabilityRows < limits.stabilityRowsPerFrame) {
            ++progress.stabilityRows;
            std::pop_heap(state.stagedContributions.begin(), state.stagedContributions.end(), ContributionHeapLess);
            state.nextPublishedContributions.push_back(state.stagedContributions.back());
            state.stagedContributions.pop_back();
        }
        return Result<void>::Success();
    }

    void StreamingSourcePlannerState::BeginCycle(const std::uint64_t time, const StreamingCellStabilityPressure pressure) noexcept {
        ++cycleSequence;
        cyclePressure = pressure;
        readyHeap.clear();
        nextPublished.clear();
        nextPublishedContributions.clear();
        cycleActive = true;
        cycleTime = time;
        oldestCycleSampleTime = time;
        sourceSlot = 0;
        historySlot = 0;
        staged.clear();
        stagedContributions.clear();
    }

    /** @copydoc StreamingSourcePlanner::Service */
    Result<StreamingSourcePlannerProgress> StreamingSourcePlanner::Service(const std::uint64_t serviceTimeMilliseconds,
                                                                           const StreamingCellStabilityPressure pressure) {
        auto &state = *state_;
        if (!state.OnLane() || state.closed)
            return Failure<StreamingSourcePlannerProgress>(WorldStreamingErrors::SourceLifecycleUnavailable);
        if (serviceTimeMilliseconds < state.lastServiceTime)
            return Failure<StreamingSourcePlannerProgress>(WorldStreamingErrors::CellStabilityStale);
        if (pressure >= StreamingCellStabilityPressure::Count)
            return Failure<StreamingSourcePlannerProgress>(WorldStreamingErrors::CellStabilityUnsupported);
        state.lastServiceTime = serviceTimeMilliseconds;
        CycleAttempt guard{state};
        if (!state.cycleActive) {
            if (state.cycleSequence == std::numeric_limits<std::uint64_t>::max())
                return Failure<StreamingSourcePlannerProgress>(WorldStreamingErrors::GenerationExhausted);
            state.BeginCycle(serviceTimeMilliseconds, pressure);
        }
        StreamingSourcePlannerProgress progress;
        progress.revision = state.planSequence;
        const auto sources = state.StepSources(progress);
        if (sources.HasError())
            return Result<StreamingSourcePlannerProgress>::Failure(sources.ErrorValue());
        if (state.sourceSlot != state.sourceOrder.size())
            return guard.Complete(progress);
        const auto history = state.StepHistory(progress);
        if (history.HasError())
            return Result<StreamingSourcePlannerProgress>::Failure(history.ErrorValue());
        if (state.historySlot != state.published.size())
            return guard.Complete(progress);
        const auto output = state.StepOutput(progress);
        if (output.HasError())
            return Result<StreamingSourcePlannerProgress>::Failure(output.ErrorValue());
        if (state.readyHeap.empty() && state.stagedContributions.empty()) {
            if (serviceTimeMilliseconds - state.oldestCycleSampleTime > state.config.limits.maximumPlanAgeMilliseconds)
                return Failure<StreamingSourcePlannerProgress>(WorldStreamingErrors::PrefetchStale);
            if (state.planSequence == std::numeric_limits<std::uint64_t>::max())
                return Failure<StreamingSourcePlannerProgress>(WorldStreamingErrors::GenerationExhausted);
            ++state.planSequence;
            state.published.swap(state.nextPublished);
            state.publishedContributions.swap(state.nextPublishedContributions);
            state.publishedRevision = state.revision;
            state.publishedTime = state.oldestCycleSampleTime;
            state.cycleActive = false;
            progress.published = true;
            progress.revision = state.planSequence;
        }
        return guard.Complete(progress);
    }

    /** @copydoc StreamingSourcePlanner::Plan */
    Result<StreamingSourcePlanView> StreamingSourcePlanner::Plan() const {
        if (!state_->OnLane() || state_->closed)
            return Failure<StreamingSourcePlanView>(WorldStreamingErrors::SourceLifecycleUnavailable);
        if (state_->publishedRevision == 0 || state_->publishedRevision != state_->revision ||
            state_->lastServiceTime - state_->publishedTime > state_->config.limits.maximumPlanAgeMilliseconds)
            return Failure<StreamingSourcePlanView>(WorldStreamingErrors::PartitionRegistryUnavailable);
        return Result<StreamingSourcePlanView>::Success(
            {state_->partition.Binding(), state_->planSequence, state_->publishedTime, state_->published, state_->publishedContributions});
    }
}  // namespace Horo::WorldStreaming
