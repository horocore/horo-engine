#pragma once
/** @file AnimationClipPayloadBinding.h
 * @brief One cold field handoff from portable semantics to the canonical Clip owner; no generation issuer or publication authority.
 */
#include "Horo/Animation/AnimationCookedAssets.h"

namespace Horo::Animation::CookedInternal {
    inline Result<AnimationClipAsset> BindClipPayload(AnimationClipCookedData portable, const SkeletonAsset &skeleton,
                                                      SkeletonAssetGeneration skeletonGeneration, AnimationClipGeneration clipGeneration,
                                                      AnimationReferencePoseGeneration referenceGeneration,
                                                      const AnimationClipBuildContext &context = {}) {
        AnimationClipData data;
        std::optional<AnimationReferencePoseBinding> reference;
        if (portable.referencePose)
            reference = AnimationReferencePoseBinding{*portable.referencePose, referenceGeneration};
        data.descriptor = {portable.contractVersion, portable.id,        clipGeneration,
                           portable.skeleton,        skeletonGeneration, portable.duration,
                           portable.sampleRate,      portable.wrapMode,  portable.kind,
                           portable.compression,     reference,          portable.hasRootMotion};
        data.tracks = std::move(portable.tracks);
        data.syncMarkers = std::move(portable.syncMarkers);
        data.eventSchemas = std::move(portable.eventSchemas);
        data.events = std::move(portable.events);
        return AnimationClipAsset::Create(std::move(data), skeleton, context);
    }
}  // namespace Horo::Animation::CookedInternal
