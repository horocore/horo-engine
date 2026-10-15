#include "Horo/WorldStreaming/StreamingSourcePlanner.h"
#include "StreamingSourcePlannerInternal.h"

#include <algorithm>

namespace Horo::WorldStreaming {
    using SourcePlanning::Failure;

    namespace {
        /** @brief Validates one bounded canonical projection before reading destination facts. */
        Result<void> ValidateEvidence(const StreamingSourcePlannerState &state, const StreamingSourceOwnerToken &source,
                                      const StreamingTravelResidencyEvidence &evidence, const std::uint64_t serviceTimeMilliseconds,
                                      const std::uint64_t maximumEvidenceAgeMilliseconds) {
            if (evidence.partition != state.partition.Binding())
                return Failure<void>(WorldStreamingErrors::PartitionRegistryStale);
            if (evidence.revision == 0 || maximumEvidenceAgeMilliseconds == 0 || evidence.cells.size() > state.config.limits.cells)
                return Failure<void>(WorldStreamingErrors::SourceDescriptorInvalid);
            if (evidence.sampledAtServiceMilliseconds > serviceTimeMilliseconds ||
                serviceTimeMilliseconds - evidence.sampledAtServiceMilliseconds > maximumEvidenceAgeMilliseconds)
                return Failure<void>(WorldStreamingErrors::PartitionRegistryStale);
            std::optional<StreamingCellId> previous;
            for (const auto &fact : evidence.cells) {
                if (!fact.fence.IsValid() || fact.state > StreamingCellState::Failed ||
                    (previous && !StreamingCellCanonicalLess{}(*previous, fact.fence.cell)))
                    return Failure<void>(WorldStreamingErrors::SourceDescriptorInvalid);
                if (fact.fence.partition != source.partition || fact.fence.epoch != source.epoch)
                    return Failure<void>(WorldStreamingErrors::PartitionRegistryStale);
                const auto membership = state.partition.Find(fact.fence.cell);
                if (membership.HasError())
                    return Result<void>::Failure(membership.ErrorValue());
                previous = fact.fence.cell;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc StreamingSourcePlanner::PollTravel */
    Result<StreamingTravelReadiness> StreamingSourcePlanner::PollTravel(const StreamingSourceRegistration &registration,
                                                                        const StreamingSourceRevision revision,
                                                                        const StreamingTravelResidencyEvidence &evidence,
                                                                        const std::uint64_t serviceTimeMilliseconds,
                                                                        const std::uint64_t maximumEvidenceAgeMilliseconds) const {
        if (registration.state_.get() != state_.get())
            return Failure<StreamingTravelReadiness>(WorldStreamingErrors::SourceOwnerStale);
        const auto &source = registration.Owner();
        if (const auto valid = state_->ValidateOwner(source); valid.HasError())
            return Result<StreamingTravelReadiness>::Failure(valid.ErrorValue());
        const auto &request = *state_->sources[source.slot].request;
        if (request.revision != revision)
            return Failure<StreamingTravelReadiness>(WorldStreamingErrors::SourceRevisionStale);
        if (request.intent != StreamingSourceIntent::Preload)
            return Failure<StreamingTravelReadiness>(WorldStreamingErrors::SourceIntentUnsupported);
        if (const auto valid = ValidateEvidence(*state_, source, evidence, serviceTimeMilliseconds, maximumEvidenceAgeMilliseconds);
            valid.HasError())
            return Result<StreamingTravelReadiness>::Failure(valid.ErrorValue());
        if (!state_->Selected(request) || request.residency == StreamingDesiredResidency::Unloaded)
            return Failure<StreamingTravelReadiness>(WorldStreamingErrors::PartitionRegistryUnavailable);
        if (state_->publishedRevision != state_->revision || serviceTimeMilliseconds < state_->publishedTime ||
            serviceTimeMilliseconds - state_->publishedTime > state_->config.limits.maximumPlanAgeMilliseconds)
            return Result<StreamingTravelReadiness>::Success(StreamingTravelReadiness::Pending);
        bool destinationFound = false;
        bool pending = false;
        for (const auto &contribution : state_->publishedContributions) {
            if (contribution.source.owner != source || contribution.source.revision != revision || contribution.predicted)
                continue;
            destinationFound = true;
            const auto fact = std::ranges::lower_bound(evidence.cells, contribution.cell.cell, StreamingCellCanonicalLess{},
                                                       [](const StreamingTravelResidencyFact &value) {
                return value.fence.cell;
            });
            if (fact == evidence.cells.end() || fact->fence.cell != contribution.cell.cell) {
                pending = true;
                continue;
            }
            if (fact->state == StreamingCellState::Failed)
                return Result<StreamingTravelReadiness>::Success(StreamingTravelReadiness::Failed);
            const bool sufficient = fact->state == StreamingCellState::Active ||
                                    (request.residency == StreamingDesiredResidency::Loaded && fact->state == StreamingCellState::Resident);
            pending = pending || !sufficient;
        }
        if (!destinationFound)
            return Failure<StreamingTravelReadiness>(WorldStreamingErrors::PartitionRegistryUnavailable);
        return Result<StreamingTravelReadiness>::Success(pending ? StreamingTravelReadiness::Pending : StreamingTravelReadiness::Ready);
    }
}  // namespace Horo::WorldStreaming
