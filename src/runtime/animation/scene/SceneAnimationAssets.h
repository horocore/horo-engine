#pragma once
/** @file SceneAnimationAssets.h
 * @brief Private cold realization from actual Scene/group cooked bytes; no loader queue, provider or cache authority.
 */
#include "Horo/Animation/AnimationCookedAssets.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Runtime/Scene/SceneAnimationRuntime.h"

namespace Horo::Runtime::AnimationSceneInternal {
    /** @brief One owner-local nonwrapping publication-generation issuer; never serialized or globally discovered. */
    struct AssetGenerations final {
        std::uint64_t next{1};
        [[nodiscard]] Result<std::uint64_t> Reserve();
    };

    /** @brief Actual validated immutable dependencies retained by a graph instance; no caller span survives preparation. */
    struct PreparedAssets final {
        std::shared_ptr<const Animation::SkeletonAsset> skeleton;
        Animation::SkeletonAssetGeneration skeletonGeneration;
        std::vector<std::shared_ptr<const Animation::AnimationClipAsset>> clips;
        std::shared_ptr<const Animation::AnimationGraphProgram> program;
        Sha256Digest sourcePayloadDigest;

        [[nodiscard]] Animation::GraphInstanceAssets Borrow() const noexcept {
            return {program, skeleton, skeletonGeneration, clips};
        }
    };

    /** @brief Captured cold-operation policy; references live only for the synchronous realization call. */
    struct AssetContext final {
        const AssetCookTargetId &target;
        const SceneAnimationLimits &limits;
        Animation::AnimationCookedContext codec;
        AssetGenerations &generations;
    };

    /** @brief Verifies the actual selected HCAF envelope from same-Scene or exact group pins, with optional content commitment. */
    [[nodiscard]] Result<Assets::AssetCookArtifact> ReadArtifact(const RuntimeSceneView &scene,
                                                                 std::span<const RuntimeGroupAssetLease> group,
                                                                 Assets::AssetDependency expected, const AssetCookTargetId &target,
                                                                 const Animation::AnimationCookedContext &codec,
                                                                 std::optional<Sha256Digest> expectedPayload = {});

    /** @brief Realizes one complete graph/clip binding from already-loaded canonical resources, without I/O or source parsing. */
    [[nodiscard]] Result<PreparedAssets> PrepareAssets(const RuntimeSceneView &scene, std::span<const RuntimeGroupAssetLease> group,
                                                       const Animation::AnimationAuthoringComponent &authoring,
                                                       const AssetContext &context);

    /** @brief Uses only checked real HCAF payload declarations for the existing Scene preparation closure callback. */
    [[nodiscard]] Result<std::vector<SceneAssetDependency>> DiscoverAssets(const RuntimeSceneAssetView &asset,
                                                                           const AssetCookTargetId &target,
                                                                           const CancellationToken &cancellation,
                                                                           std::size_t maximumReturnedDependencies);
}  // namespace Horo::Runtime::AnimationSceneInternal
