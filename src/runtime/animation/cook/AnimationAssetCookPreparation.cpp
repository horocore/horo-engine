#include "AnimationAssetCookState.h"
#include "AnimationClipPayloadBinding.h"
#include "AnimationGraphSourceInternal.h"

#include <algorithm>

namespace Horo::Animation {
    namespace {
        std::string_view Text(const Assets::AssetCookPinnedSource &source) noexcept {
            return {reinterpret_cast<const char *>(source.bytes.data()), source.bytes.size()};
        }

        AnimationCookedContext Context(const AnimationAssetCookLimits &limits, const CancellationToken &cancellation) {
            return {limits.codec, cancellation, true};
        }
    }  // namespace

    PreparedAnimationCook::State::State(const Assets::AssetCookInputSnapshot &source, AssetCookTargetId selected,
                                        AnimationAssetCookLimits policy)
        : capture(source), target(std::move(selected)), limits(std::move(policy)) {}

    const PreparedAnimationCook::State::Entry *PreparedAnimationCook::State::Find(Assets::AssetId id) const noexcept {
        const auto found = std::ranges::find(entries, id, &Entry::id);
        return found == entries.end() ? nullptr : &*found;
    }

    const PreparedAnimationCook::State::Skeleton *PreparedAnimationCook::State::FindSkeleton(SkeletonId id) const noexcept {
        const auto found = std::ranges::find_if(skeletons, [&](const auto &value) {
            return value.owner->Data().skeleton == id;
        });
        return found == skeletons.end() ? nullptr : &*found;
    }

    Result<void> PreparedAnimationCook::State::Add(AnimationCookedPayload payload, Assets::AssetTypeId type) {
        if (entries.size() >= limits.maximumAssets || payload.bytes.size() > limits.maximumPreparedBytes - preparedBytes)
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Capacity));
        if (payload.dependencies.size() > limits.maximumReferences - preparedReferences)
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Capacity));
        if (Find(payload.asset))
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Malformed));
        preparedBytes += payload.bytes.size();
        preparedReferences += payload.dependencies.size();
        entries.emplace_back(payload.asset, std::move(type), std::move(payload));
        return Result<void>::Success();
    }

    Result<void> PreparedAnimationCook::State::PrepareSkeletons(const CancellationToken &cancellation) {
        for (const auto &source : capture.Sources()) {
            if (source.record.type.Value() != "core.animation.skeleton")
                continue;
            const auto context = Context(limits, cancellation);
            auto skeleton =
                DeserializeSkeletonAnimationSource(Text(source), SkeletonId::Create(source.record.id).Value(), context, limits.source);
            if (skeleton.HasError())
                return Result<void>::Failure(std::move(skeleton).ErrorValue());
            auto payload = EncodeSkeletonAnimationPayload(skeleton.Value(), context);
            if (payload.HasError())
                return Result<void>::Failure(std::move(payload).ErrorValue());
            if (const auto added = Add(std::move(payload).Value(), source.record.type); added.HasError())
                return added;
            auto owner = std::make_shared<const SkeletonAsset>(std::move(skeleton).Value());
            const auto generation = SkeletonAssetGeneration::Create(nextGeneration++).Value();
            skeletons.emplace_back(std::move(owner), generation);
        }
        return Result<void>::Success();
    }

    Result<void> PreparedAnimationCook::State::PrepareClips(const CancellationToken &cancellation) {
        for (const auto &source : capture.Sources()) {
            if (source.record.type.Value() != "core.animation.clip")
                continue;
            const auto context = Context(limits, cancellation);
            auto portable =
                DeserializeClipAnimationSource(Text(source), AnimationClipId::Create(source.record.id).Value(), context, limits.source);
            if (portable.HasError())
                return Result<void>::Failure(std::move(portable).ErrorValue());
            const auto *skeleton = FindSkeleton(portable.Value().skeleton);
            if (!skeleton)
                return Result<void>::Failure(MakeError(AnimationAssetErrors::Unavailable));
            const auto generation = nextGeneration++;
            auto clip = CookedInternal::BindClipPayload(std::move(portable).Value(), *skeleton->owner, skeleton->generation,
                                                        AnimationClipGeneration::Create(generation).Value(),
                                                        AnimationReferencePoseGeneration::Create(generation).Value());
            if (clip.HasError())
                return Result<void>::Failure(std::move(clip).ErrorValue());
            const auto *skeletonPayload = Find(skeleton->owner->Data().skeleton.Asset());
            auto payload = EncodeClipAnimationPayload(clip.Value(), skeletonPayload->payload.bytes, context);
            if (payload.HasError())
                return Result<void>::Failure(std::move(payload).ErrorValue());
            if (const auto added = Add(std::move(payload).Value(), source.record.type); added.HasError())
                return added;
            clips.push_back(std::move(clip).Value());
        }
        return Result<void>::Success();
    }

    Result<std::vector<AnimationClipAsset>> PreparedAnimationCook::State::SelectClips(const AnimationGraphData &graph) const {
        using Return = Result<std::vector<AnimationClipAsset>>;
        const auto references = GraphSourceDetail::ProjectDependencies(graph);
        std::vector<AnimationClipAsset> selected;
        for (const auto id : references.clips) {
            const auto found = std::ranges::find_if(clips, [&](const auto &clip) {
                return clip.Data().descriptor.id == id;
            });
            if (found == clips.end())
                return Return::Failure(MakeError(AnimationAssetErrors::Unavailable));
            selected.push_back(*found);
        }
        return Return::Success(std::move(selected));
    }

    Result<void> PreparedAnimationCook::State::PrepareGraphs(const CancellationToken &cancellation) {
        for (const auto &source : capture.Sources()) {
            if (source.record.type.Value() != "core.animation.graph")
                continue;
            AnimationGraphCompileContext compile;
            compile.cancellation = cancellation;
            auto graph = DeserializeAnimationGraphSource(Text(source), AnimationGraphId::Create(source.record.id).Value(), compile);
            if (graph.HasError())
                return Result<void>::Failure(std::move(graph).ErrorValue());
            const auto *skeleton = FindSkeleton(graph.Value().skeleton);
            if (!skeleton)
                return Result<void>::Failure(MakeError(AnimationAssetErrors::Unavailable));
            const auto references = GraphSourceDetail::ProjectDependencies(graph.Value());
            auto selected = SelectClips(graph.Value());
            if (selected.HasError())
                return Result<void>::Failure(std::move(selected).ErrorValue());
            compile.skeleton = skeleton->owner.get();
            compile.skeletonGeneration = skeleton->generation;
            compile.clips = selected.Value();
            auto program = AnimationGraphProgram::Compile(graph.Value(), compile);
            if (program.HasError())
                return Result<void>::Failure(std::move(program).ErrorValue());
            std::vector<AnimationCookedDependencyInput> inputs;
            const auto *skeletonEntry = Find(graph.Value().skeleton.Asset());
            inputs.push_back({{skeletonEntry->id, skeletonEntry->type}, skeletonEntry->payload.bytes});
            for (const auto id : references.clips) {
                const auto *entry = Find(id.Asset());
                inputs.push_back({{entry->id, entry->type}, entry->payload.bytes});
            }
            std::ranges::sort(inputs, {}, [](const auto &value) {
                return value.metadata.id;
            });
            auto payload = EncodeGraphAnimationPayload(program.Value(), inputs, Context(limits, cancellation));
            if (payload.HasError())
                return Result<void>::Failure(std::move(payload).ErrorValue());
            const auto added = Add(std::move(payload).Value(), source.record.type);
            if (added.HasError())
                return added;
        }
        return Result<void>::Success();
    }

    Result<void> PreparedAnimationCook::State::Build(const CancellationToken &cancellation) {
        entries.reserve(limits.maximumAssets);
        if (auto made = PrepareSkeletons(cancellation); made.HasError())
            return made;
        if (auto made = PrepareClips(cancellation); made.HasError())
            return made;
        if (auto made = PrepareGraphs(cancellation); made.HasError())
            return made;
        std::ranges::sort(entries, {}, &Entry::id);
        return Result<void>::Success();
    }
}  // namespace Horo::Animation
