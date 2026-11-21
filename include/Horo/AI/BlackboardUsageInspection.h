#pragma once

/** @file BlackboardUsageInspection.h @brief Detached provider and typed Blackboard evidence from Decision source. */

#include "Horo/AI/BehaviorTree.h"
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

    /** @brief Why an authored binding has no matching provider descriptor in the observed catalog. */
    enum class DecisionProviderBindingIssueKind : std::uint8_t {
        CatalogUnavailable,
        DescriptorMissing,
        DescriptorVersionMismatch,
    };

    /** @brief One diagnostic attached to the visible authored node that owns the provider binding. */
    struct DecisionProviderBindingIssue final {
        DecisionNodeId node; /**< Authored tree node, Utility option, or state owning a provider action. */
        DecisionProviderBindingIssueKind kind;
    };

    /** @brief Inspects direct authored provider bindings even when another node is an unknown placeholder.
     * @param source The decoded source being presented.
     * @param providers Detached descriptors from the current Project registry, or null if the catalog is unavailable.
     * @return At most one finding per visible node in stable node order. No properties, dependencies or factories are evaluated.
     */
    [[nodiscard]] std::vector<DecisionProviderBindingIssue> InspectDecisionProviderBindings(
        const DecisionGraphSource &source, const DecisionBlackboardProviderMetadata *providers);

    /** @brief One statically declared key use in a source node or its current provider contract. */
    struct DecisionBlackboardSourceUse final {
        DecisionNodeId node;
        DecisionBlackboardRequirement requirement;
        std::optional<DecisionDescriptorOrigin> provider;
    };

    /** @brief A typed authored query result written to the selected Blackboard key. */
    struct DecisionBlackboardQueryUse final {
        DecisionNodeId node;
        BehaviorTreeQueryOutput output;
    };

    /** @brief Owned metadata projection. Unresolved providers make the source inventory incomplete. */
    struct DecisionBlackboardSourceUses final {
        std::vector<DecisionBlackboardSourceUse> uses;
        std::vector<DecisionNodeId> unresolvedProviders;
        std::vector<DecisionBlackboardQueryUse> queryOutputs;
    };

    /** @brief Finds exact typed requirements through the canonical Decision lowering path.
     * @param source Decoded graph compatible with the caller's exact schema.
     * @param key Stable key identity to match. @param providers Optional detached metadata from the exact observed generation.
     * @return Declared uses, authored query outputs and unresolved providers, or a typed source/limit error.
     * No executable provider code is invoked.
     */
    [[nodiscard]] Result<DecisionBlackboardSourceUses> InspectDecisionBlackboardUses(const DecisionGraphSource &source, BlackboardKeyId key,
                                                                                     const DecisionBlackboardProviderMetadata *providers);

    /** @brief Checks whether distinct admitted keys have compatible declaration contracts for retargeting.
     * @param original Key being retired. @param replacement Key selected as the destination.
     * @return Whether type, access, presence, capacity and default availability are compatible.
     */
    [[nodiscard]] bool IsCompatibleBlackboardReplacement(const BlackboardKeyDescriptor &original,
                                                         const BlackboardKeyDescriptor &replacement) noexcept;

    /** @brief Retargets typed Blackboard references in one detached Decision source.
     * @param source Authored graph using the exact schema and an admitted version range.
     * @param schema Current immutable Blackboard schema containing both keys.
     * @param from Key being retired. @param to Compatible key selected as its replacement.
     * @param providers Detached declarations from the observed provider catalog.
     * @return True when a source changed, false when no typed reference matches, or a typed failure for an unsafe replacement.
     * @post A failure preserves the original source. The caller validates the entire changed closure before publishing it.
     */
    [[nodiscard]] Result<bool> RetargetDecisionBlackboardKey(DecisionGraphSource &source, const BlackboardSchema &schema,
                                                             BlackboardKeyId from, BlackboardKeyId to,
                                                             const DecisionBlackboardProviderMetadata *providers);

}  // namespace Horo::AI
