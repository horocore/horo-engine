#pragma once
/** @file AnimationAssetSource.h
 * @brief Offline-only strict Skeleton/Clip source transport. Runtime loading uses AnimationCookedAssets exclusively.
 */
#include "Horo/Animation/AnimationCookedAssets.h"

namespace Horo::Animation {
    /** @brief Positive caller-lowerable source DOM limits; no caller can raise the implementation ceilings. */
    struct AnimationAssetSourceLimits final {
        std::size_t bytes{16U * 1024U * 1024U};
        std::size_t values{2U * 1024U * 1024U};
        std::size_t depth{32};
    };

    /** @brief Reads schema-1 source through the canonical immutable Skeleton owner.
     * @param source Complete bounded UTF-8 JSON. @param identity Exact sidecar-owned identity, absent from source payload.
     * @param context Cold policy/cancellation. @param limits Source parser limits.
     * @return Valid immutable semantics; no filesystem/cache/publication mutation.
     */
    [[nodiscard]] Result<SkeletonAsset> DeserializeSkeletonAnimationSource(std::string_view source, SkeletonId identity,
                                                                           const AnimationCookedContext &context = {},
                                                                           const AnimationAssetSourceLimits &limits = {});
    /** @brief Writes complete schema-1 Skeleton semantics with exact explicit dependency declarations.
     * @param source Immutable validated asset. @param context Cold policy. @param limits Encoded source limits.
     * @return Canonical JSON plus newline, excluding sidecar identity and runtime generations.
     */
    [[nodiscard]] Result<std::string> SerializeSkeletonAnimationSource(const SkeletonAsset &source,
                                                                       const AnimationCookedContext &context = {},
                                                                       const AnimationAssetSourceLimits &limits = {});
    /** @brief Reads detached schema-1 Clip semantics, including the stable graph-local additive reference-pose ID.
     * @param source Complete bounded UTF-8 JSON. @param identity Exact sidecar-owned clip ID.
     * @param context Cold policy. @param limits Parser limits.
     * @return Detached portable fields, not an accepted live clip; cook must bind the actual skeleton through AnimationClipAsset::Create.
     */
    [[nodiscard]] Result<AnimationClipCookedData> DeserializeClipAnimationSource(std::string_view source, AnimationClipId identity,
                                                                                 const AnimationCookedContext &context = {},
                                                                                 const AnimationAssetSourceLimits &limits = {});
    /** @brief Writes the complete semantics of an actual validated Clip without any live publication generation.
     * @param source Actual immutable validated clip. @param context Cold policy. @param limits Encoded source limits.
     * @return Canonical source JSON plus newline; packaged runtime never parses these bytes.
     */
    [[nodiscard]] Result<std::string> SerializeClipAnimationSource(const AnimationClipAsset &source,
                                                                   const AnimationCookedContext &context = {},
                                                                   const AnimationAssetSourceLimits &limits = {});
}  // namespace Horo::Animation
