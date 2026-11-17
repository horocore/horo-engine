#pragma once

#include "Horo/Cinematic/CameraCutRuntime.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Tests::CameraCuts {
    using namespace Runtime;
    using namespace Cinematic;

    template <typename T> T Take(Result<T> result) {
        if (result.HasError()) {
            INFO("domain=" << result.ErrorValue().domain.Value() << " code=" << result.ErrorValue().code.Value());
            REQUIRE(result.HasValue());
        }
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline RuntimeSceneDefinition SceneDefinition() {
        SceneDefinitionBuilder builder{SceneDefinitionId{1}, SceneDefinitionRevision{100}};
        for (std::uint64_t id = 1; id <= 3; ++id) {
            RuntimeEntityDefinition entity;
            entity.object = SceneObjectId{id};
            entity.localTransform.translation = {static_cast<float>(id * 10), 0.0F, 0.0F};
            entity.components.camera = CameraComponent{};
            builder.Add(entity);
        }
        return Take(std::move(builder).Build());
    }

    inline SequencePlaybackActivation Activation(const SequenceLoopMode mode = SequenceLoopMode::Once) {
        const std::array keys{SequenceFrameCameraCutKey{{1, 1}, {1, 1}, {1, 1}, 2}, SequenceFrameCameraCutKey{{1, 1}, {2, 1}, {2, 1}, 5}};
        return {{{{1, 1}, {1, 1}}, 10},
                Take(SequenceFrameEvaluationPlan::Create(10, mode, 4, {}, {}, keys)),
                {{SequenceBlendMode::Blend, 9}, {SequenceBlendMode::Blend, 9}}};
    }

    inline SequencePlaybackActivation TransitionActivation(const CameraTransitionTier tier = CameraTransitionTier::SingleViewBlend,
                                                           const CameraBlendCurve curve = CameraBlendCurve::Linear,
                                                           const CameraTransitionFallback fallback = CameraTransitionFallback::Fail,
                                                           const SequenceTime duration = 2) {
        auto activation = Activation();
        std::array keys{activation.plan.CameraKeys()[0], activation.plan.CameraKeys()[1]};
        keys[1].transitionTier = tier;
        keys[1].fallback = fallback;
        keys[1].transitionDuration = duration;
        keys[1].transitionCurve = curve;
        activation.plan = Take(SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 4, {}, {}, keys));
        return activation;
    }

}  // namespace Horo::Tests::CameraCuts
