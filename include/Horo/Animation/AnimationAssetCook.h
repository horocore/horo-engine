#pragma once
/** @file AnimationAssetCook.h
 * @brief Offline canonical-prepared contribution from one actual Assets capture; no cache/publication authority.
 */
#include "Horo/Animation/AnimationAssetSource.h"
#include "Horo/Assets/AssetCookInputSnapshot.h"
#include "Horo/Assets/AssetCookService.h"

namespace Horo::Animation {
    /** @brief Whole-operation limits, independent of lower host capture/cook/resource budgets. */
    struct AnimationAssetCookLimits final {
        std::size_t maximumAssets{4096};
        std::uint64_t maximumPreparedBytes{256ULL * 1024ULL * 1024ULL};
        std::size_t maximumReferences{65536};
        AnimationCookedLimits codec;
        AnimationAssetSourceLimits source;
    };

    /** @brief Immutable canonical payloads and real cold native owners derived only from the same sealed source capture.
     * @details All methods are cold owner/composition work. Strategies emit no envelopes and own no cache; Assets constructs,
     * validates and publishes all HCAF. The existing host retains its project lease, source rechecks and final stale fences.
     */
    class PreparedAnimationCook final {
    public:
        /** @brief Parses and validates captured Animation sources once, prepares canonical payloads and exact semantic roles.
         * @param capture Actual source/sidecar capture used by the SAME eventual AST request.
         * @param target Exact selected cook target. @param limits Lowerable finite safety policy.
         * @param cancellation Actual operation token. @return Immutable preparation or original typed failure.
         */
        [[nodiscard]] static Result<PreparedAnimationCook> Create(const Assets::AssetCookInputSnapshot &capture, AssetCookTargetId target,
                                                                  AnimationAssetCookLimits limits = {},
                                                                  const CancellationToken &cancellation = {});
        /** @brief Supplies all three immutable strategies for the existing first resource phase.
         * @return Contributions with exact cold source identity and strict fresh/cache payload equality.
         */
        [[nodiscard]] std::vector<Assets::CookerContribution> Contributions() const;
        /** @brief Tests the original opaque capture identity through Assets' existing exact-match projection contract.
         * @param capture Actual eventual AST capture. @return True only for the same sealed source/sidecar pin, never merely equal digests.
         */
        [[nodiscard]] bool UsesCapture(const Assets::AssetCookInputSnapshot &capture) const;
        /** @brief Verifies complete actually emitted/cached Animation HCAF before the existing prefab callback consumes it.
         * @param artifacts Exact joined first-phase views. @param cancellation Actual operation token.
         * @return Success only for exact IDs/types/target/source and canonical dependency/output bytes; no publication authority.
         */
        [[nodiscard]] Result<void> ValidateEmitted(std::span<const Assets::AssetCookCandidateArtifactView> artifacts,
                                                   const CancellationToken &cancellation) const;
        /** @brief Projects the one true required resource closure for the selected prefab root's direct resource roles.
         * @param roots Canonical direct resource requirements from the SAME prefab graph/root.
         * @param artifacts Exact joined envelopes, verified in full before traversal. @param maximumAssets Lower host distinct bound.
         * @param cancellation Actual operation token.
         * @return Bounded canonical roles used ONCE for selecting envelopes AND the existing prefab expected dependency closure.
         * @details Unrelated non-Animation resources remain leaves. These are inert cold host inputs, never runtime capabilities.
         */
        [[nodiscard]] Result<std::vector<Assets::AssetDependency>> ExpandDependencies(
            std::span<const Assets::AssetDependency> roots, std::span<const Assets::AssetCookCandidateArtifactView> artifacts,
            std::size_t maximumAssets, const CancellationToken &cancellation) const;

    private:
        struct State;

        explicit PreparedAnimationCook(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<const State> state_;
    };
}  // namespace Horo::Animation
