#pragma once

/** @file AnimationPoseComposition.h
 * @brief Target-private bounded local-pose composition kernels.
 */
#include "Horo/Animation/AnimationGraphInstance.h"

namespace Horo::Animation::Detail {
    /** @brief Complete dense views resolved at preparation; optional coverage preserves absent overlay joints. */
    struct PoseCompositionInput final {
        std::span<const Math::Transform> base;
        std::span<const Math::Transform> overlay;
        std::span<const Math::Transform> reference;
        std::span<const float> mask;
        std::span<const std::uint8_t> coverage;
        GraphBlendMode mode{GraphBlendMode::Normalized};
        float weight{};
    };

    /** @brief Composes at most one admitted skeleton into candidate-only, distinct storage.
     * @param input Complete base/overlay, additive reference, optional dense mask and overlay coverage.
     * @param output Complete candidate storage; must not overlap any input.
     * @return Typed malformed/type/capacity failure or success; no allocation or publication.
     * @details Finite weights are clamped to [0,1]. Coverage zero and effective weight zero
     * preserve the exact base transform. Additive translation uses the weighted reference
     * difference, scale uses a weighted reference ratio, and rotation uses a shortest-arc
     * identity-to-delta slerp. Local deltas postmultiply the base; parent deltas premultiply it.
     * Failure can leave candidate writes, which the transaction owner must discard.
     */
    [[nodiscard]] GraphInstanceFailure ComposePose(const PoseCompositionInput &input, std::span<Math::Transform> output) noexcept;

    /** @brief Checks finite local values and nondegenerate rotation/scale without allocating.
     * @param transform Candidate local transform.
     * @return Whether transform can be admitted to graph pose storage.
     */
    [[nodiscard]] bool ValidPoseTransform(const Math::Transform &transform) noexcept;
}  // namespace Horo::Animation::Detail
