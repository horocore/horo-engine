#include "UiAnimationTrackSampling.h"

#include "UiAnimationInterpolation.h"
#include "UiStyleInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime::Ui::AnimationInternal {
    /** @copydoc ValidatePropertyTrack */
    Result<UiElementHandle> ValidatePropertyTrack(const UiAnimationPropertyTrack &track, const UiElementTree &tree,
                                                  const RuntimeStyleRegistry &registry, const std::uint32_t maximumKeyframes) {
        if (tree.State() != UiElementTreeState::Active || registry.State() != RuntimeStyleRegistryState::Active)
            return Result<UiElementHandle>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        const auto target = tree.Find(track.target);
        if (target.HasError())
            return Result<UiElementHandle>::Failure(target.ErrorValue());
        const auto properties = registry.Properties();
        const auto property = std::ranges::lower_bound(properties, track.property, {}, &UiStylePropertyDescriptor::id);
        if (property == properties.end() || property->id != track.property || track.keyframes.size() < 2 ||
            track.keyframes.size() > maximumKeyframes || track.keyframes.front().position != 0 ||
            track.keyframes.back().position != std::numeric_limits<std::uint32_t>::max())
            return Result<UiElementHandle>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        std::uint32_t previous{};
        bool first = true;
        for (const auto &keyframe : track.keyframes) {
            if ((!first && keyframe.position <= previous) || keyframe.easing != UiAnimationEasing::Linear ||
                !StyleInternal::IsValueCompatible(*property, keyframe.value))
                return Result<UiElementHandle>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            if (const auto category = InterpolateValue(track.keyframes.front().value, keyframe.value, {}); category.HasError())
                return Result<UiElementHandle>::Failure(category.ErrorValue());
            first = false;
            previous = keyframe.position;
        }
        return Result<UiElementHandle>::Success(target.Value());
    }

    /** @copydoc SamplePropertyTrack */
    Result<UiStyleValue> SamplePropertyTrack(const UiAnimationPropertyTrack &track, const std::uint32_t progress) {
        if (track.keyframes.size() < 2)
            return Result<UiStyleValue>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        const auto next = std::ranges::upper_bound(track.keyframes, progress, {}, &UiAnimationKeyframe::position);
        if (next == track.keyframes.begin())
            return Result<UiStyleValue>::Success(next->value);
        if (next == track.keyframes.end())
            return Result<UiStyleValue>::Success(track.keyframes.back().value);
        const auto &previous = *std::prev(next);
        const auto width = next->position - previous.position;
        if (width == 0 || next->easing != UiAnimationEasing::Linear)
            return Result<UiStyleValue>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        const auto numerator = static_cast<std::uint64_t>(progress - previous.position) * std::numeric_limits<std::uint32_t>::max();
        const auto fraction = static_cast<std::uint32_t>(numerator / width);
        return InterpolateValue(previous.value, next->value, {fraction});
    }
}  // namespace Horo::Runtime::Ui::AnimationInternal
