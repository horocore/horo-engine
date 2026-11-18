#pragma once

/** @file BlackboardUsageInspection.h @brief Typed Blackboard requirement evidence from Decision source. */

#include "Horo/AI/DecisionAssetValidation.h"

namespace Horo::AI {
    class DecisionNodeGeneration;
    struct DecisionGraphSource;

    /** @brief Deep-owned review metadata from one exact admitted provider catalog.
     * @details This value owns no native image, VM, runtime generation, executable callback or admission token.
     */
    class DecisionBlackboardProviderMetadata final {
    public:
        /** @brief Copies only validated descriptors from the caller's observed generation. @param generation Active provider metadata.
         * @return Detached immutable metadata or a typed stale/limit failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<const DecisionBlackboardProviderMetadata>> Capture(
            const DecisionNodeGeneration &generation);

        /** @brief Returns the copied, type-ordered native descriptor declarations. @return Borrowed declarations. */
        [[nodiscard]] std::span<const DecisionNodeDescriptor> Descriptors() const noexcept {
            return descriptors_;
        }

    private:
        explicit DecisionBlackboardProviderMetadata(std::vector<DecisionNodeDescriptor> descriptors);
        std::vector<DecisionNodeDescriptor> descriptors_;
    };

    /** @brief One statically declared key use in a source node or its current provider contract. */
    struct DecisionBlackboardSourceUse final {
        DecisionNodeId node;
        DecisionBlackboardRequirement requirement;
        std::optional<DecisionDescriptorOrigin> provider;
    };

    /** @brief Owned metadata projection. Unresolved providers make the source inventory incomplete. */
    struct DecisionBlackboardSourceUses final {
        std::vector<DecisionBlackboardSourceUse> uses;
        std::vector<DecisionNodeId> unresolvedProviders;
    };

    /** @brief Finds exact typed requirements through the canonical Decision lowering path.
     * @param source Decoded graph compatible with the caller's exact schema.
     * @param key Stable key identity to match. @param providers Optional detached metadata from the exact observed generation.
     * @return Declared uses and unresolved providers, or a typed source/limit error. No compiler, factory or arbitrary property is invoked.
     */
    [[nodiscard]] Result<DecisionBlackboardSourceUses> InspectDecisionBlackboardUses(const DecisionGraphSource &source, BlackboardKeyId key,
                                                                                     const DecisionBlackboardProviderMetadata *providers);

}  // namespace Horo::AI
