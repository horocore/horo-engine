#pragma once

#include "Horo/Animation/AnimationComponents.h"

namespace Horo::Tests::SceneAnimation {
    /** @brief Stable authored values only; this fixture does not claim native asset loading or runtime activation. */
    inline Animation::AnimationAuthoringComponent Binding(bool clip = false) {
        const auto skeleton = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001").Value();
        const auto source = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000002").Value();
        const auto retarget = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000003").Value();
        Animation::AnimationAuthoringComponent value;
        value.component = Animation::AnimationComponentId::Create(7).Value();
        value.skeleton = Animation::SkeletonId::Create(skeleton).Value();
        value.source.kind = clip ? Animation::AnimationSourceKind::Clip : Animation::AnimationSourceKind::Graph;
        if (clip)
            value.source.clip = Animation::AnimationClipId::Create(source).Value();
        else
            value.source.graph = Animation::AnimationGraphId::Create(source).Value();
        value.retarget = Animation::RetargetProfileId::Create(retarget).Value();
        value.rootMotion = Animation::RootMotionPolicy::RequestCharacterMovement;
        value.startsPlaying = false;
        return value;
    }
}  // namespace Horo::Tests::SceneAnimation
