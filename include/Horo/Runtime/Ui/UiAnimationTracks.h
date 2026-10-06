#pragma once

/** @file UiAnimationTracks.h
 * @brief Inert bounded typed property tracks and stable marker descriptors for the actual Runtime UI animation owner.
 */
#include "Horo/Runtime/Ui/UiAnimationTimeline.h"
#include "Horo/Runtime/Ui/UiStyle.h"

#include <vector>

namespace Horo::Runtime::Ui {
    struct UiAnimationMarkerIdentityTag;
    /** @brief Stable authored marker identity; transient crossing evidence is never serialized. */
    using UiAnimationMarkerId = UiStableId<UiAnimationMarkerIdentityTag>;

    /** @brief Versioned keyframe easing; unsupported representations fail admission rather than guessing a native curve. */
    enum class UiAnimationEasing : std::uint8_t {
        Linear
    };

    /** @brief Exact normalized keyframe location in the closed UINT32 interval with an allocation-free typed style literal. */
    struct UiAnimationKeyframe final {
        std::uint32_t position{};
        UiStyleValue value;
        UiAnimationEasing easing{UiAnimationEasing::Linear}; /**< Incoming segment easing; ignored at the first exact endpoint. */
    };

    /**
     * @brief Owned immutable authored property track; admission binds stable target/property to actual retained-tree and registry evidence.
     * @details Keyframes strictly increase, include both exact endpoints, and retain one continuous category and semantic color role.
     *          Registration alone never samples a clock, changes an element, publishes a style or grants route lifecycle authority.
     */
    struct UiAnimationPropertyTrack final {
        UiElementId target;
        UiStylePropertyId property;
        std::vector<UiAnimationKeyframe> keyframes;
    };

    /** @brief Closed projection from an actual computed Dimension property into the sole declarative layout owner. */
    enum class UiAnimationLayoutField : std::uint8_t {
        Width,
        Height,
        MinimumWidth,
        MinimumHeight,
        MaximumWidth,
        MaximumHeight,
        OffsetLeft,
        OffsetTop,
        Count
    };

    /**
     * @brief Inert authored property-to-layout binding, qualified against actual tree and immutable registry at admission.
     * @details The computed style value, including sealing and accessibility precedence, supplies the value. No raw timeline
     *          sample bypasses the computed-style owner. Dimensional values retain canonical 1/64-DIP units without scaling twice.
     */
    struct UiAnimationLayoutBinding final {
        UiElementId target;
        UiStylePropertyId property;
        UiAnimationLayoutField field{UiAnimationLayoutField::Width};
    };

    /** @brief Inert authored marker at one exact normalized position; repeats are handled by the bounded playback owner. */
    struct UiAnimationMarker final {
        UiAnimationMarkerId id;
        std::uint32_t position{};
    };

    /**
     * @brief Complete load-time authored animation value adopted only after actual owner admission.
     * @details Stable animation, target, property and marker IDs are durable. Runtime handles, clock cursors and completion proofs
     *          are issued by the actual owner and never accepted from this descriptor. Resolved accessibility policy belongs to
     *          the application policy owner; its revision and resulting time policy are explicit inputs, not guessed here.
     */
    struct UiAnimationDefinition final {
        UiAnimationId id;
        std::uint16_t schemaVersion{1};
        UiAnimationTimePolicy time;
        UiStylePolicyRevision resolvedMotionPolicy;
        std::vector<UiAnimationPropertyTrack> tracks;
        std::vector<UiAnimationMarker> markers;
    };

    /** @brief Lifetime capacity contract; owner construction allocates all mutable instance/candidate/publication storage. */
    struct UiAnimationLimits final {
        std::uint32_t timelines{64};
        std::uint32_t propertiesPerTimeline{16};
        std::uint32_t keyframesPerProperty{32};
        std::uint32_t markersPerTimeline{32};
        std::uint32_t markerCrossingsPerUpdate{128};
        std::uint32_t retainedSnapshots{3};
        std::uint32_t commands{128};
    };

    /** @brief Copied marker crossing correlated to one exact timeline incarnation and successful aggregate update. */
    struct UiAnimationMarkerCrossing final {
        UiAnimationTimelineId timeline;
        UiAnimationMarkerId marker;
        std::uint64_t iteration{};
        std::uint64_t updateSequence{};
        bool reverse{};
    };
}  // namespace Horo::Runtime::Ui
