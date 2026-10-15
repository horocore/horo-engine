#include "Horo/WorldStreaming/StreamingDesiredStateReduction.h"

#include "Horo/WorldStreaming/WorldStreamingErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        [[nodiscard]] bool SourceLess(const StreamingSourceDesiredState *left, const StreamingSourceDesiredState *right) noexcept {
            return left->Source().id < right->Source().id;
        }

        [[nodiscard]] Result<void> ValidateContributor(const StreamingSourceDesiredState &desiredState,
                                                       const StreamingDesiredStateReductionContext &context) {
            if (const auto valid = ValidateStreamingSourceDescriptor(desiredState.Source()); valid.HasError())
                return valid;
            if (desiredState.Source().owner.partition != context.partition || desiredState.Source().owner.epoch != context.epoch)
                return Result<void>::Failure(MakeError(WorldStreamingErrors::SourceOwnerStale));
            return Result<void>::Success();
        }

        [[nodiscard]] StreamingDesiredResidency Stronger(const StreamingDesiredResidency left,
                                                         const StreamingDesiredResidency right) noexcept {
            return std::max(left, right);
        }

        void ApplyContribution(const StreamingSourceDesiredState &desiredState, StreamingDesiredResidency &effectiveResidency,
                               std::optional<StreamingDesiredResidency> &pinnedResidencyFloor,
                               std::vector<StreamingDesiredStateContribution> &contributors) {
            const auto summary = CombineStreamingDesiredState({effectiveResidency, pinnedResidencyFloor}, desiredState);
            effectiveResidency = summary.effectiveResidency;
            pinnedResidencyFloor = summary.pinnedResidencyFloor;
            const auto &source = desiredState.Source();
            contributors.emplace_back(source.id, source.owner, source.revision, desiredState.Residency(), desiredState.Retention());
        }
    }  // namespace

    /** @copydoc CombineStreamingDesiredState */
    StreamingDesiredStateSummary CombineStreamingDesiredState(StreamingDesiredStateSummary summary,
                                                              const StreamingSourceDesiredState &desiredState) noexcept {
        summary.effectiveResidency = Stronger(summary.effectiveResidency, desiredState.Residency());
        if (desiredState.IsPinned())
            summary.pinnedResidencyFloor =
                Stronger(summary.pinnedResidencyFloor.value_or(StreamingDesiredResidency::Unloaded), desiredState.Residency());
        return summary;
    }

    /** @copydoc StreamingDesiredStateReduction::StreamingDesiredStateReduction */
    StreamingDesiredStateReduction::StreamingDesiredStateReduction(const StreamingDesiredStateReductionContext &context,
                                                                   const StreamingDesiredResidency effectiveResidency,
                                                                   const std::optional<StreamingDesiredResidency> pinnedResidencyFloor,
                                                                   std::vector<StreamingDesiredStateContribution> contributors) noexcept
        : context_(context), effectiveResidency_(effectiveResidency), pinnedResidencyFloor_(pinnedResidencyFloor),
          contributors_(std::move(contributors)) {}

    /** @copydoc StreamingDesiredStateReduction::Create */
    Result<StreamingDesiredStateReduction> StreamingDesiredStateReduction::Create(
        const StreamingDesiredStateReductionContext &context, const std::span<const StreamingSourceDesiredState> desiredStates,
        const StreamingDesiredStateReductionLimits limits) {
        if (!context.IsValid() || limits.maximumContributors == 0)
            return Result<StreamingDesiredStateReduction>::Failure(MakeError(WorldStreamingErrors::SourceReductionInvalid));
        if (desiredStates.size() > limits.maximumContributors)
            return Result<StreamingDesiredStateReduction>::Failure(MakeError(WorldStreamingErrors::SourceReductionCapacityExceeded));

        std::vector<const StreamingSourceDesiredState *> ordered;
        ordered.reserve(desiredStates.size());
        for (const auto &desiredState : desiredStates) {
            if (const auto valid = ValidateContributor(desiredState, context); valid.HasError())
                return Result<StreamingDesiredStateReduction>::Failure(valid.ErrorValue());
            ordered.push_back(&desiredState);
        }
        std::ranges::sort(ordered, SourceLess);
        if (std::ranges::adjacent_find(ordered, [](const auto *left, const auto *right) {
            return left->Source().id == right->Source().id;
        }) != ordered.end())
            return Result<StreamingDesiredStateReduction>::Failure(MakeError(WorldStreamingErrors::SourceReductionIdentityConflict));

        auto effectiveResidency = StreamingDesiredResidency::Unloaded;
        std::optional<StreamingDesiredResidency> pinnedResidencyFloor;
        std::vector<StreamingDesiredStateContribution> contributors;
        contributors.reserve(ordered.size());
        for (const auto *desiredState : ordered)
            ApplyContribution(*desiredState, effectiveResidency, pinnedResidencyFloor, contributors);

        return Result<StreamingDesiredStateReduction>::Success(
            StreamingDesiredStateReduction{context, effectiveResidency, pinnedResidencyFloor, std::move(contributors)});
    }
}  // namespace Horo::WorldStreaming
