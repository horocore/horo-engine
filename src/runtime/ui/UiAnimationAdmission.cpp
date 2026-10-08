#include "UiAnimationAdmission.h"

#include "UiAnimationPlayback.h"
#include "UiAnimationTrackSampling.h"

#include <algorithm>

namespace Horo::Runtime::Ui::AnimationInternal {
    namespace {
        /** @brief Qualifies stable layout bindings against the same actual registry consumed by computed style. */
        Result<void> ValidateLayout(const UiAnimationCanvasDefinition &definition, const UiElementTree &tree,
                                    const RuntimeStyleRegistry &registry) {
            const auto properties = registry.Properties();
            for (std::size_t index = 0; index < definition.layoutBindings.size(); ++index) {
                const auto &binding = definition.layoutBindings[index];
                if (const auto property = std::ranges::find(properties, binding.property, &UiStylePropertyDescriptor::id);
                    binding.field >= UiAnimationLayoutField::Count || tree.Find(binding.target).HasError() ||
                    property == properties.end() || property->category != UiStyleValueCategory::Dimension || !property->effects.measure)
                    return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
                const auto prior = std::span(definition.layoutBindings).first(index);
                if (std::ranges::any_of(prior, [&](const auto &other) {
                    return other.target == binding.target && other.field == binding.field;
                }))
                    return Result<void>::Failure(MakeError(UiErrors::AnimationConflict));
            }
            return Result<void>::Success();
        }

        /** @brief Rejects duplicated authored track targets and marker IDs before finite instance storage is adopted. */
        Result<void> ValidateAnimation(const UiAnimationDefinition &animation, const UiElementTree &tree,
                                       const RuntimeStyleRegistry &registry, const UiAnimationLimits &limits) {
            if (!animation.id.IsValid() || animation.schemaVersion != 1 || animation.tracks.empty() ||
                animation.tracks.size() > limits.propertiesPerTimeline || animation.markers.size() > limits.markersPerTimeline)
                return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            if (const auto time = ValidatePlaybackPolicy(animation.time); time.HasError())
                return time;
            for (std::size_t index = 0; index < animation.tracks.size(); ++index) {
                const auto &track = animation.tracks[index];
                if (const auto valid = ValidatePropertyTrack(track, tree, registry, limits.keyframesPerProperty); valid.HasError())
                    return Result<void>::Failure(valid.ErrorValue());
                const auto prior = std::span(animation.tracks).first(index);
                if (std::ranges::any_of(prior, [&](const auto &other) {
                    return other.target == track.target && other.property == track.property;
                }))
                    return Result<void>::Failure(MakeError(UiErrors::AnimationConflict));
            }
            for (std::size_t index = 0; index < animation.markers.size(); ++index) {
                const auto &marker = animation.markers[index];
                const auto prior = std::span(animation.markers).first(index);
                if (!marker.id.IsValid() || (index != 0 && marker.position < animation.markers[index - 1].position) ||
                    std::ranges::find(prior, marker.id, &UiAnimationMarker::id) != prior.end())
                    return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Requires exactly one immutable authored style/layout declaration per actual retained element. */
        Result<void> ValidateElements(const UiAnimationCanvasDefinition &definition, const UiElementTree &tree,
                                      const RuntimeStyleRegistry &registry) {
            if (definition.elements.size() != tree.Size())
                return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            for (std::size_t index = 0; index < definition.elements.size(); ++index) {
                const auto &element = definition.elements[index];
                if (tree.Find(element.element).HasError() || !registry.HasAsset(element.asset) || !element.layout.IsValid() ||
                    !element.intrinsic.IsValid() || (element.typeClass.IsValid() && !registry.HasClass(element.typeClass)))
                    return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
                if (std::ranges::any_of(element.classes, [&](const auto &styleClass) {
                    return !registry.HasClass(styleClass);
                }))
                    return Result<void>::Failure(MakeError(UiErrors::StyleReferenceInvalid));
                const auto prior = std::span(definition.elements).first(index);
                if (std::ranges::find(prior, element.element, &UiAnimationElementDefinition::element) != prior.end())
                    return Result<void>::Failure(MakeError(UiErrors::AnimationConflict));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidLimits */
    bool ValidLimits(const UiAnimationLimits &limits) noexcept {
        return limits.timelines > 0 && limits.timelines <= 1'024 && limits.propertiesPerTimeline > 0 &&
               limits.propertiesPerTimeline <= MaximumUiStyleProperties && limits.keyframesPerProperty >= 2 &&
               limits.keyframesPerProperty <= 1'024 && limits.markersPerTimeline <= 256 && limits.markerCrossingsPerUpdate > 0 &&
               limits.markerCrossingsPerUpdate <= 65'536 && limits.retainedSnapshots >= 2 &&
               limits.retainedSnapshots <= MaximumUiStyleSnapshotsInFlight && limits.commands > 0 && limits.commands <= 4'096;
    }

    /** @copydoc ValidateDefinitions */
    Result<void> ValidateDefinitions(const UiAnimationCanvasDefinition &definition, const UiReloadCanvas &canvas,
                                     const RuntimeStyleRegistry &registry, const UiAnimationLimits &limits) {
        if (definition.canvas != canvas.id || !definition.content.IsValid() || !definition.resolvedPolicy.IsValid() ||
            !canvas.layoutEngine || definition.elements.size() > MaximumUiTreeElements || definition.animations.size() > limits.timelines ||
            definition.layoutBindings.size() > definition.elements.size() * static_cast<std::size_t>(UiAnimationLayoutField::Count))
            return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        if (const auto elements = ValidateElements(definition, canvas.tree, registry); elements.HasError())
            return elements;
        if (const auto layout = ValidateLayout(definition, canvas.tree, registry); layout.HasError())
            return layout;
        for (std::size_t index = 0; index < definition.animations.size(); ++index) {
            const auto &animation = definition.animations[index];
            const auto prior = std::span(definition.animations).first(index);
            if (animation.resolvedMotionPolicy != definition.resolvedPolicy ||
                std::ranges::find(prior, animation.id, &UiAnimationDefinition::id) != prior.end())
                return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            if (const auto valid = ValidateAnimation(animation, canvas.tree, registry, limits); valid.HasError())
                return valid;
        }
        return ValidateRouteBindings(definition, canvas);
    }
}  // namespace Horo::Runtime::Ui::AnimationInternal
