#include "SceneAnimationAssets.h"

#include "AnimationClipPayloadBinding.h"
#include "AnimationClipSceneProgramAccess.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime::AnimationSceneInternal {
    namespace {
        namespace A = Animation;
        using Artifact = Assets::AssetCookArtifact;
        using Bindings = std::vector<A::AnimationCookedDependency>;

        Assets::AssetDependency Requirement(Assets::AssetId id, std::string_view type) {
            return {id, Assets::AssetTypeId::Parse(type).Value()};
        }

        Result<Artifact> CheckArtifact(Result<Artifact> artifact, const Assets::AssetDependency &expected, const AssetCookTargetId &target,
                                       std::optional<Sha256Digest> digest) {
            if (artifact.HasError())
                return artifact;
            const auto &value = artifact.Value();
            if (value.id != expected.id || value.type != expected.expectedType || value.target != target)
                return Result<Artifact>::Failure(MakeError(A::AnimationAssetErrors::Malformed));
            if (digest && value.payloadDigest != *digest)
                return Result<Artifact>::Failure(MakeError(A::AnimationAssetErrors::Stale));
            return artifact;
        }

        std::optional<A::AnimationCookedKind> Kind(std::string_view type) {
            using enum Horo::Animation::AnimationCookedKind;
            if (type == "core.animation.skeleton")
                return Skeleton;
            if (type == "core.animation.clip")
                return Clip;
            if (type == "core.animation.graph")
                return Graph;
            return {};
        }

        Result<Artifact> LoadSkeleton(const RuntimeSceneView &scene, std::span<const RuntimeGroupAssetLease> group,
                                      const Bindings &bindings, A::SkeletonId skeleton, const AssetContext &context) {
            const auto bound = std::ranges::find(bindings, skeleton.Asset(), [](const auto &binding) {
                return binding.metadata.id;
            });
            if (bound == bindings.end() || bound->metadata.expectedType.Value() != "core.animation.skeleton")
                return Result<Artifact>::Failure(MakeError(A::AnimationAssetErrors::Malformed));
            return ReadArtifact(scene, group, bound->metadata, context.target, context.codec, bound->payloadDigest);
        }

        Result<A::AnimationClipAsset> PrepareClip(const Artifact &artifact, const Artifact &skeletonArtifact,
                                                  const A::SkeletonAsset &skeleton, A::SkeletonAssetGeneration skeletonGeneration,
                                                  const AssetContext &context) {
            using Return = Result<A::AnimationClipAsset>;
            const auto id = A::AnimationClipId::Create(artifact.id).Value();
            auto portable = A::DecodeClipAnimationPayload(artifact.payload, id, context.codec);
            if (portable.HasError())
                return Return::Failure(portable.ErrorValue());
            auto bindings = A::InspectAnimationPayloadBindings(artifact.payload, A::AnimationCookedKind::Clip, artifact.id,
                                                               context.codec.limits.dependencies, context.codec);
            if (bindings.HasError())
                return Return::Failure(bindings.ErrorValue());
            if (bindings.Value().size() != 1 || bindings.Value().front().payloadDigest != skeletonArtifact.payloadDigest)
                return Return::Failure(MakeError(A::AnimationAssetErrors::Stale));
            auto generation = context.generations.Reserve();
            if (generation.HasError())
                return Return::Failure(generation.ErrorValue());
            auto clip = A::CookedInternal::BindClipPayload(std::move(portable).Value(), skeleton, skeletonGeneration,
                                                           A::AnimationClipGeneration::Create(generation.Value()).Value(),
                                                           A::AnimationReferencePoseGeneration::Create(generation.Value()).Value(),
                                                           {.limits = context.limits.clip});
            if (clip.HasError())
                return clip;
            const auto canonical = A::EncodeClipAnimationPayload(clip.Value(), skeletonArtifact.payload, context.codec);
            if (canonical.HasError())
                return Return::Failure(canonical.ErrorValue());
            if (canonical.Value().bytes != artifact.payload)
                return Return::Failure(MakeError(A::AnimationAssetErrors::Malformed));
            return clip;
        }

        Result<std::vector<A::AnimationClipAsset>> PrepareClips(const RuntimeSceneView &scene,
                                                                std::span<const RuntimeGroupAssetLease> group, const Bindings &bindings,
                                                                const Artifact &source, const Artifact &skeletonArtifact,
                                                                const PreparedAssets &assets, bool directClip,
                                                                const AssetContext &context) {
            using Return = Result<std::vector<A::AnimationClipAsset>>;
            std::vector<A::AnimationClipAsset> clips;
            if (directClip) {
                auto clip = PrepareClip(source, skeletonArtifact, *assets.skeleton, assets.skeletonGeneration, context);
                if (clip.HasError())
                    return Return::Failure(clip.ErrorValue());
                clips.push_back(std::move(clip).Value());
                return Return::Success(std::move(clips));
            }
            for (const auto &binding : bindings) {
                if (binding.metadata.expectedType.Value() != "core.animation.clip")
                    continue;
                auto artifact = ReadArtifact(scene, group, binding.metadata, context.target, context.codec, binding.payloadDigest);
                if (artifact.HasError())
                    return Return::Failure(artifact.ErrorValue());
                auto clip = PrepareClip(artifact.Value(), skeletonArtifact, *assets.skeleton, assets.skeletonGeneration, context);
                if (clip.HasError())
                    return Return::Failure(clip.ErrorValue());
                clips.push_back(std::move(clip).Value());
            }
            return Return::Success(std::move(clips));
        }

        Result<A::AnimationGraphProgram> PrepareProgram(const Artifact &source, const PreparedAssets &assets,
                                                        std::span<const A::AnimationClipAsset> clips,
                                                        const A::AnimationAuthoringComponent &authoring, const AssetContext &context) {
            const A::AnimationGraphCompileContext compile{.cancellation = context.codec.cancellation,
                                                          .skeleton = assets.skeleton.get(),
                                                          .skeletonGeneration = assets.skeletonGeneration,
                                                          .clips = clips};
            if (authoring.source.kind == A::AnimationSourceKind::Clip)
                return A::Detail::ClipSceneProgramAccess::Prepare(authoring.source.clip, compile);
            auto data = A::DecodeGraphAnimationPayload(source.payload, authoring.source.graph, context.codec);
            if (data.HasError())
                return Result<A::AnimationGraphProgram>::Failure(data.ErrorValue());
            return A::AnimationGraphProgram::FromCooked(data.Value(), compile);
        }
    }  // namespace

    /** @copydoc AssetGenerations::Reserve */
    Result<std::uint64_t> AssetGenerations::Reserve() {
        if (next == 0)
            return Result<std::uint64_t>::Failure(MakeError(Animation::AnimationErrors::GenerationExhausted));
        const auto issued = next;
        next = next == std::numeric_limits<std::uint64_t>::max() ? 0 : next + 1;
        return Result<std::uint64_t>::Success(issued);
    }

    /** @copydoc ReadArtifact */
    Result<Artifact> ReadArtifact(const RuntimeSceneView &scene, std::span<const RuntimeGroupAssetLease> group,
                                  Assets::AssetDependency expected, const AssetCookTargetId &target, const A::AnimationCookedContext &codec,
                                  std::optional<Sha256Digest> expectedPayload) {
        Assets::AssetCookLimits envelopeLimits;
        envelopeLimits.maximumArtifactBytes = static_cast<std::size_t>(codec.limits.bytes) + 4096;
        if (const auto found = std::ranges::find(group, expected.id,
                                                 [](const auto &pin) {
            return pin.metadata.id;
        });
            found != group.end()) {
            if (found->metadata.expectedType != expected.expectedType)
                return Result<Artifact>::Failure(MakeError(A::AnimationAssetErrors::Malformed));
            return CheckArtifact(Assets::DecodeCookedArtifactBytes(found->artifact.Bytes(), envelopeLimits), expected, target,
                                 expectedPayload);
        }
        const auto view = scene.FindAsset(expected.id);
        if (!view)
            return Result<Artifact>::Failure(MakeError(A::AnimationAssetErrors::Unavailable));
        if (!view->type || *view->type != expected.expectedType)
            return Result<Artifact>::Failure(MakeError(A::AnimationAssetErrors::Malformed));
        return CheckArtifact(Assets::DecodeCookedArtifact(view->bytes, envelopeLimits), expected, target, expectedPayload);
    }

    namespace {
        struct PreparedRoot final {
            Artifact source;
            Bindings bindings;
            Artifact skeletonArtifact;
            PreparedAssets assets;
        };

        Result<void> SupportedAuthoring(const A::AnimationAuthoringComponent &authoring) {
            if (auto valid = A::ValidateAnimationAuthoringComponent(authoring); valid.HasError())
                return valid;
            if (authoring.retarget.IsValid() || authoring.rootMotion != A::RootMotionPolicy::Ignore)
                return Result<void>::Failure(MakeError(A::AnimationAssetErrors::Unsupported));
            return Result<void>::Success();
        }

        Result<PreparedRoot> PrepareRoot(const RuntimeSceneView &scene, std::span<const RuntimeGroupAssetLease> group,
                                         const A::AnimationAuthoringComponent &authoring, const AssetContext &context) {
            using Return = Result<PreparedRoot>;
            if (auto valid = SupportedAuthoring(authoring); valid.HasError())
                return Return::Failure(valid.ErrorValue());
            const bool clip = authoring.source.kind == A::AnimationSourceKind::Clip;
            const auto id = clip ? authoring.source.clip.Asset() : authoring.source.graph.Asset();
            auto source = ReadArtifact(scene, group, Requirement(id, clip ? "core.animation.clip" : "core.animation.graph"), context.target,
                                       context.codec);
            if (source.HasError())
                return Return::Failure(source.ErrorValue());
            auto bindings = A::InspectAnimationPayloadBindings(source.Value().payload,
                                                               clip ? A::AnimationCookedKind::Clip : A::AnimationCookedKind::Graph, id,
                                                               context.codec.limits.dependencies, context.codec);
            if (bindings.HasError())
                return Return::Failure(bindings.ErrorValue());
            auto skeletonArtifact = LoadSkeleton(scene, group, bindings.Value(), authoring.skeleton, context);
            if (skeletonArtifact.HasError())
                return Return::Failure(skeletonArtifact.ErrorValue());
            auto skeleton = A::DecodeSkeletonAnimationPayload(skeletonArtifact.Value().payload, authoring.skeleton, context.codec);
            if (skeleton.HasError())
                return Return::Failure(skeleton.ErrorValue());
            auto generation = context.generations.Reserve();
            if (generation.HasError())
                return Return::Failure(generation.ErrorValue());
            PreparedRoot root;
            root.source = std::move(source).Value();
            root.bindings = std::move(bindings).Value();
            root.skeletonArtifact = std::move(skeletonArtifact).Value();
            root.assets.skeleton = std::make_shared<const A::SkeletonAsset>(std::move(skeleton).Value());
            root.assets.skeletonGeneration = A::SkeletonAssetGeneration::Create(generation.Value()).Value();
            return Return::Success(std::move(root));
        }
    }  // namespace

    /** @copydoc PrepareAssets */
    Result<PreparedAssets> PrepareAssets(const RuntimeSceneView &scene, std::span<const RuntimeGroupAssetLease> group,
                                         const A::AnimationAuthoringComponent &authoring, const AssetContext &context) {
        using Return = Result<PreparedAssets>;
        auto rootResult = PrepareRoot(scene, group, authoring, context);
        if (rootResult.HasError())
            return Return::Failure(rootResult.ErrorValue());
        auto root = std::move(rootResult).Value();
        auto clipResult = PrepareClips(scene, group, root.bindings, root.source, root.skeletonArtifact, root.assets,
                                       authoring.source.kind == A::AnimationSourceKind::Clip, context);
        if (clipResult.HasError())
            return Return::Failure(clipResult.ErrorValue());
        auto clips = std::move(clipResult).Value();
        auto program = PrepareProgram(root.source, root.assets, clips, authoring, context);
        if (program.HasError())
            return Return::Failure(program.ErrorValue());
        if (program.Value().Source() != authoring.source)
            return Return::Failure(MakeError(A::AnimationAssetErrors::Malformed));
        root.assets.program = std::make_shared<const A::AnimationGraphProgram>(std::move(program).Value());
        for (auto &value : clips)
            root.assets.clips.push_back(std::make_shared<const A::AnimationClipAsset>(std::move(value)));
        root.assets.sourcePayloadDigest = root.source.payloadDigest;
        return Return::Success(std::move(root.assets));
    }

    /** @copydoc DiscoverAssets */
    Result<std::vector<SceneAssetDependency>> DiscoverAssets(const RuntimeSceneAssetView &asset, const AssetCookTargetId &target,
                                                             const CancellationToken &cancellation,
                                                             std::size_t maximumReturnedDependencies) {
        using Return = Result<std::vector<SceneAssetDependency>>;
        if (!asset.type)
            return Return::Failure(MakeError(A::AnimationAssetErrors::Malformed));
        const auto kind = Kind(asset.type->Value());
        if (!kind) {
            if (asset.type->Value() == "core.animation.retarget_profile")
                return Return::Failure(MakeError(A::AnimationAssetErrors::Unsupported));
            return Return::Success({});
        }
        Assets::AssetCookLimits envelopeLimits;
        envelopeLimits.maximumArtifactBytes = 64U * 1024U * 1024U + 4096U;
        auto artifact = CheckArtifact(Assets::DecodeCookedArtifact(asset.bytes, envelopeLimits), {asset.id, *asset.type}, target, {});
        if (artifact.HasError())
            return Return::Failure(artifact.ErrorValue());
        return A::InspectAnimationPayloadDependencies(artifact.Value().payload, *kind, asset.id, maximumReturnedDependencies,
                                                      {.cancellation = cancellation});
    }
}  // namespace Horo::Runtime::AnimationSceneInternal
