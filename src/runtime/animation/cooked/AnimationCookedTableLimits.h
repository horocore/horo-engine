#pragma once
/** @file AnimationCookedTableLimits.h
 * @brief Existing Animation hard-table ceilings charged cumulatively before a binary table is allocated.
 */
#include "Horo/Animation/AnimationCookedAssets.h"

namespace Horo::Animation::CookedInternal {
    inline constexpr std::size_t TableBudgetCount = 26;

    template <typename T> struct TableLimit final {
        static constexpr std::size_t index = 0;
        static constexpr std::uint64_t single = AnimationCookedHardLimits::Values;
        static constexpr std::uint64_t total = AnimationCookedHardLimits::Values;
    };

    /** @brief Existing semantic safety ceilings for SkeletonJoint. */
    template <> struct TableLimit<SkeletonJoint> final {
        static constexpr std::size_t index = 1;
        static constexpr std::uint64_t single = SkeletonAssetHardLimits::Joints;
        static constexpr std::uint64_t total = SkeletonAssetHardLimits::Joints;
    };

    /** @brief Existing semantic safety ceilings for SkeletonSocket. */
    template <> struct TableLimit<SkeletonSocket> final {
        static constexpr std::size_t index = 2;
        static constexpr std::uint64_t single = SkeletonAssetHardLimits::Sockets;
        static constexpr std::uint64_t total = SkeletonAssetHardLimits::Sockets;
    };

    /** @brief Existing semantic safety ceilings for AnimationJointTrack. */
    template <> struct TableLimit<AnimationJointTrack> final {
        static constexpr std::size_t index = 3;
        static constexpr std::uint64_t single = AnimationClipHardLimits::Tracks;
        static constexpr std::uint64_t total = AnimationClipHardLimits::Tracks;
    };

    /** @brief Existing semantic safety ceilings for AnimationTransformKey. */
    template <> struct TableLimit<AnimationTransformKey> final {
        static constexpr std::size_t index = 4;
        static constexpr std::uint64_t single = AnimationClipHardLimits::KeysPerTrack;
        static constexpr std::uint64_t total = AnimationClipHardLimits::TotalKeys;
    };

    /** @brief Existing semantic safety ceilings for AnimationSyncMarker. */
    template <> struct TableLimit<AnimationSyncMarker> final {
        static constexpr std::size_t index = 5;
        static constexpr std::uint64_t single = AnimationClipHardLimits::SyncMarkers;
        static constexpr std::uint64_t total = AnimationClipHardLimits::SyncMarkers;
    };

    /** @brief Existing semantic safety ceilings for AnimationEventSchema. */
    template <> struct TableLimit<AnimationEventSchema> final {
        static constexpr std::size_t index = 6;
        static constexpr std::uint64_t single = AnimationClipHardLimits::EventSchemas;
        static constexpr std::uint64_t total = AnimationClipHardLimits::EventSchemas;
    };

    /** @brief Existing semantic safety ceilings for AnimationEventFieldSchema. */
    template <> struct TableLimit<AnimationEventFieldSchema> final {
        static constexpr std::size_t index = 7;
        static constexpr std::uint64_t single = AnimationEventPayload::MaximumFields;
        static constexpr std::uint64_t total = AnimationClipHardLimits::EventSchemas * AnimationEventPayload::MaximumFields;
    };

    /** @brief Existing semantic safety ceilings for AnimationClipEvent. */
    template <> struct TableLimit<AnimationClipEvent> final {
        static constexpr std::size_t index = 8;
        static constexpr std::uint64_t single = AnimationClipHardLimits::Events;
        static constexpr std::uint64_t total = AnimationClipHardLimits::Events;
    };

    /** @brief Existing semantic safety ceilings for GraphCompiledDefinition. */
    template <> struct TableLimit<GraphCompiledDefinition> final {
        static constexpr std::size_t index = 9;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::Definitions;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::Definitions;
    };

    /** @brief Existing semantic safety ceilings for GraphInstruction. */
    template <> struct TableLimit<GraphInstruction> final {
        static constexpr std::size_t index = 10;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::Nodes;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::Nodes;
    };

    /** @brief Existing semantic safety ceilings for GraphPin. */
    template <> struct TableLimit<GraphPin> final {
        static constexpr std::size_t index = 11;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::Pins;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::Pins;
    };

    /** @brief Existing semantic safety ceilings for GraphCompiledInput. */
    template <> struct TableLimit<GraphCompiledInput> final {
        static constexpr std::size_t index = 12;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::Connections;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::Connections;
    };

    /** @brief Existing semantic safety ceilings for GraphParameter. */
    template <> struct TableLimit<GraphParameter> final {
        static constexpr std::size_t index = 13;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::Parameters;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::Parameters;
    };

    /** @brief Existing semantic safety ceilings for GraphInterfaceInput. */
    template <> struct TableLimit<GraphInterfaceInput> final {
        static constexpr std::size_t index = 14;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::Pins;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::Pins;
    };

    /** @brief Existing semantic safety ceilings for GraphBoneWeight. */
    template <> struct TableLimit<GraphBoneWeight> final {
        static constexpr std::size_t index = 15;
        static constexpr std::uint64_t single = SkeletonAssetHardLimits::Joints;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::PoseTransforms;
    };

    /** @brief Existing semantic safety ceilings for GraphStateCondition. */
    template <> struct TableLimit<GraphStateCondition> final {
        static constexpr std::size_t index = 16;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::Conditions;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::Conditions;
    };

    /** @brief Existing semantic safety ceilings for GraphState. */
    template <> struct TableLimit<GraphState> final {
        static constexpr std::size_t index = 17;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::States;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::States;
    };

    /** @brief Existing semantic safety ceilings for GraphStateTransition. */
    template <> struct TableLimit<GraphStateTransition> final {
        static constexpr std::size_t index = 18;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::Transitions;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::Transitions;
    };

    /** @brief Existing semantic safety ceilings for GraphActionSlot. */
    template <> struct TableLimit<GraphActionSlot> final {
        static constexpr std::size_t index = 19;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::ActionSlots;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::ActionSlots;
    };

    /** @brief Existing semantic safety ceilings for GraphActionDefinition. */
    template <> struct TableLimit<GraphActionDefinition> final {
        static constexpr std::size_t index = 20;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::Actions;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::Actions;
    };

    /** @brief Existing semantic safety ceilings for GraphActionSection. */
    template <> struct TableLimit<GraphActionSection> final {
        static constexpr std::size_t index = 21;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::ActionSections;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::ActionSections;
    };

    /** @brief Existing semantic safety ceilings for GraphActionLoopRegion. */
    template <> struct TableLimit<GraphActionLoopRegion> final {
        static constexpr std::size_t index = 22;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::ActionLoops;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::ActionLoops;
    };

    /** @brief Existing semantic safety ceilings for GraphBlendSample. */
    template <> struct TableLimit<GraphBlendSample> final {
        static constexpr std::size_t index = 23;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::BlendSamplesPerTree;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::BlendMembers;
    };

    /** @brief Existing semantic safety ceilings for GraphBlendTriangle. */
    template <> struct TableLimit<GraphBlendTriangle> final {
        static constexpr std::size_t index = 24;
        static constexpr std::uint64_t single = AnimationGraphHardLimits::BlendTrianglesPerTree;
        static constexpr std::uint64_t total = AnimationGraphHardLimits::BlendMembers * AnimationGraphHardLimits::BlendTrianglesPerTree;
    };

    /** @brief Existing semantic safety ceilings for std::uint64_t. */
    template <> struct TableLimit<std::uint64_t> final {
        static constexpr std::size_t index = 25;
        static constexpr std::uint64_t single = AnimationClipHardLimits::AllowedEventValues;
        static constexpr std::uint64_t total =
            AnimationClipHardLimits::EventSchemas * AnimationEventPayload::MaximumFields * AnimationClipHardLimits::AllowedEventValues;
    };
}  // namespace Horo::Animation::CookedInternal
