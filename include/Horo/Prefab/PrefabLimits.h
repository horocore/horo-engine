#pragma once

/**
 * @file PrefabLimits.h
 * @brief Canonical prefab hard ceilings, project policy and derived expansion budget.
 */

#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <utility>

namespace Horo::Prefab {
    /** @brief Immutable engine safety ceilings for every bounded prefab capability. */
    struct PrefabHardLimits final {
        static constexpr std::size_t SourceDocumentBytes = 32U * 1024U * 1024U; /**< Encoded UTF-8 source-document bytes. */
        static constexpr std::size_t SourceJsonDepth = 32;                      /**< Canonical source JSON nesting depth. */
        static constexpr std::size_t SourceHierarchyDepth = 16;
        static constexpr std::size_t SourceObjectCount = 256;
        static constexpr std::size_t SourcePayloadBytes = 4U * 1024U * 1024U;
        static constexpr std::size_t ExpandedPayloadBytes = 4U * 1024U * 1024U;
        static constexpr std::size_t CookedPayloadBytes = 4U * 1024U * 1024U;
        static constexpr std::size_t ComponentsPerObject = 64;
        static constexpr std::size_t ObjectNameBytes = 256;
        static constexpr std::size_t ReferencedAssets = 256;
        static constexpr std::size_t DirectNestedPlacements = 256;
        static constexpr std::size_t VariantInheritanceDepth = 8;
        static constexpr std::size_t NestedPrefabDepth = 16;
        static constexpr std::size_t OverrideRecords = 16'384;
        static constexpr std::size_t ConflictAndOrphanRecords = 16'384;
        static constexpr std::size_t PropertyPathSegments = 32;
        static constexpr std::size_t OverrideValueBytes = 1U * 1024U * 1024U;
        static constexpr std::size_t OverrideSetBytes = 16U * 1024U * 1024U;
        static constexpr std::size_t BindingSlots = 64;
        static constexpr std::size_t BindingUses = 256;
        static constexpr std::size_t InstanceBindings = 64;
        static constexpr std::size_t RuntimeSpawnDepth = 8;
    };

    /**
     * @brief Portable project policy. Every value may lower, but never raise, its engine hard ceiling.
     *
     * Source, expanded and cooked byte limits are intentionally independent. Expanded object and
     * component limits also bound source documents, so source admission cannot exceed later stages.
     * Project Settings persistence and UI projection are owned by their application-layer workflow.
     */
    struct PrefabProjectPolicy final {
        std::size_t maximumHierarchyDepth{PrefabHardLimits::SourceHierarchyDepth};
        std::size_t maximumObjectCount{PrefabHardLimits::SourceObjectCount};
        std::size_t maximumSourcePayloadBytes{PrefabHardLimits::SourcePayloadBytes};
        std::size_t maximumExpandedPayloadBytes{PrefabHardLimits::ExpandedPayloadBytes};
        std::size_t maximumCookedPayloadBytes{PrefabHardLimits::CookedPayloadBytes};
        std::size_t maximumComponentsPerObject{PrefabHardLimits::ComponentsPerObject};
        std::size_t maximumReferencedAssets{PrefabHardLimits::ReferencedAssets};
        std::size_t maximumDirectNestedPlacements{PrefabHardLimits::DirectNestedPlacements};
        std::size_t maximumVariantInheritanceDepth{PrefabHardLimits::VariantInheritanceDepth};
        std::size_t maximumNestedPrefabDepth{PrefabHardLimits::NestedPrefabDepth};
        std::size_t maximumOverrideRecords{PrefabHardLimits::OverrideRecords};
        std::size_t maximumConflictAndOrphanRecords{PrefabHardLimits::ConflictAndOrphanRecords};
        std::size_t maximumPropertyPathSegments{PrefabHardLimits::PropertyPathSegments};
        std::size_t maximumOverrideValueBytes{PrefabHardLimits::OverrideValueBytes};
        std::size_t maximumOverrideSetBytes{PrefabHardLimits::OverrideSetBytes};
        std::size_t maximumBindingSlots{PrefabHardLimits::BindingSlots};
        std::size_t maximumBindingUses{PrefabHardLimits::BindingUses};
        std::size_t maximumInstanceBindings{PrefabHardLimits::InstanceBindings};
        std::size_t maximumRuntimeSpawnDepth{PrefabHardLimits::RuntimeSpawnDepth};

        [[nodiscard]] bool operator==(const PrefabProjectPolicy &) const noexcept = default;
    };

    /** @brief Immutable validated policy captured by authoring, resolution, cook and runtime operations. */
    class PrefabLimitProfile final {
    public:
        /**
         * @brief Validates a complete project policy and derives its finite expansion work budget.
         * @param policy Candidate project-owned policy.
         * @return Owned immutable profile, or LimitProfileInvalid before any operation activates.
         */
        [[nodiscard]] static Result<PrefabLimitProfile> Create(PrefabProjectPolicy policy);

        /** @brief Returns the captured project policy. @return Borrowed immutable policy. */
        [[nodiscard]] const PrefabProjectPolicy &Policy() const noexcept;
        /**
         * @brief Returns the checked derived maximum work items for one expansion candidate.
         * @return Sum of object, component, graph-edge, dependency, override-path and binding work ceilings.
         */
        [[nodiscard]] std::size_t MaximumExpansionWorkItems() const noexcept;

    private:
        PrefabLimitProfile(PrefabProjectPolicy policy, const std::size_t maximumExpansionWorkItems) noexcept
            : policy_(std::move(policy)), maximumExpansionWorkItems_(maximumExpansionWorkItems) {}

        PrefabProjectPolicy policy_;
        std::size_t maximumExpansionWorkItems_{};
    };

    /** @brief Operation-local derived work counter; it owns no project or runtime state. */
    class PrefabExpansionBudget final {
    public:
        /** @brief Creates a fresh budget from one already validated immutable profile. @param profile Captured profile. */
        explicit PrefabExpansionBudget(const PrefabLimitProfile &profile) noexcept;
        PrefabExpansionBudget(const PrefabExpansionBudget &) = delete;
        PrefabExpansionBudget &operator=(const PrefabExpansionBudget &) = delete;
        /** @brief Transfers the only remaining-work authority and empties the source budget. */
        PrefabExpansionBudget(PrefabExpansionBudget &&other) noexcept;
        /** @brief Replaces this authority with the source authority and empties the source budget. */
        PrefabExpansionBudget &operator=(PrefabExpansionBudget &&other) noexcept;

        /**
         * @brief Charges bounded expansion work before performing it.
         * @param workItems Number of object/component/edge/override/binding items about to be processed.
         * @return Success, or WorkBudgetExceeded without changing the remaining budget.
         */
        [[nodiscard]] Result<void> Consume(std::size_t workItems);
        /** @brief Returns unconsumed work capacity. @return Remaining work items. */
        [[nodiscard]] std::size_t Remaining() const noexcept;

    private:
        std::size_t remaining_{};
    };
}  // namespace Horo::Prefab
