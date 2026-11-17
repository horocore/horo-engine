#pragma once
#include "Horo/Animation/AnimationAssetCook.h"

namespace Horo::Animation {
    struct PreparedAnimationCook::State final {
        struct Entry final {
            Assets::AssetId id;
            Assets::AssetTypeId type;
            AnimationCookedPayload payload;
        };

        struct Skeleton final {
            std::shared_ptr<const SkeletonAsset> owner;
            SkeletonAssetGeneration generation;
        };
        class Strategy;
        State(const Assets::AssetCookInputSnapshot &capture, AssetCookTargetId target, AnimationAssetCookLimits limits);
        [[nodiscard]] Result<void> Build(const CancellationToken &cancellation);
        [[nodiscard]] Result<void> PrepareSkeletons(const CancellationToken &cancellation);
        [[nodiscard]] Result<void> PrepareClips(const CancellationToken &cancellation);
        [[nodiscard]] Result<void> PrepareGraphs(const CancellationToken &cancellation);
        [[nodiscard]] Result<std::vector<AnimationClipAsset>> SelectClips(const AnimationGraphData &graph) const;
        [[nodiscard]] Result<void> Add(AnimationCookedPayload payload, Assets::AssetTypeId type);
        [[nodiscard]] const Entry *Find(Assets::AssetId id) const noexcept;
        [[nodiscard]] const Skeleton *FindSkeleton(SkeletonId id) const noexcept;
        [[nodiscard]] bool Matches(const Assets::CookSourceView &source, const Entry &entry) const noexcept;
        [[nodiscard]] Result<void> ValidateArtifact(const Entry &entry, const Assets::AssetCookCandidateArtifactView &artifact) const;
        [[nodiscard]] Result<void> ValidateEmitted(std::span<const Assets::AssetCookCandidateArtifactView> artifacts,
                                                   const CancellationToken &cancellation) const;
        [[nodiscard]] Result<std::vector<Assets::AssetDependency>> Expand(std::span<const Assets::AssetDependency> roots,
                                                                          std::span<const Assets::AssetCookCandidateArtifactView> artifacts,
                                                                          std::size_t maximumAssets,
                                                                          const CancellationToken &cancellation) const;
        [[nodiscard]] Result<std::vector<Assets::AssetDependency>> BeginClosure(std::span<const Assets::AssetDependency> roots,
                                                                                std::size_t maximumAssets) const;
        [[nodiscard]] Result<void> Merge(std::vector<Assets::AssetDependency> &closure, const Assets::AssetDependency &role,
                                         std::size_t maximumAssets) const;
        [[nodiscard]] Result<void> AppendRoles(std::vector<Assets::AssetDependency> &closure, const Entry &entry, std::size_t maximumAssets,
                                               std::size_t &charged) const;
        Assets::AssetCookInputSnapshot capture;
        std::shared_ptr<const Assets::AssetCookKeyProjection>
            captureIdentity; /**< Empty selection: exact capture matching only, never used for keys. */
        AssetCookTargetId target;
        AnimationAssetCookLimits limits;
        std::vector<Entry> entries;
        std::vector<Skeleton> skeletons;
        std::vector<AnimationClipAsset> clips;
        std::uint64_t nextGeneration{1};
        std::uint64_t preparedBytes{};
        std::size_t preparedReferences{};
    };
}  // namespace Horo::Animation
