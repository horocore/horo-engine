#pragma once

#include "CameraCutTestSupport.h"

namespace Horo::Tests::CameraCuts {
    /** @brief Authors a three-tick native blend whose sampled Linear and SmoothStep poses differ. */
    inline Cinematic::SequencePlaybackActivation NativeCameraActivation(const Cinematic::CameraBlendCurve curve) {
        auto activation = TransitionActivation(CameraTransitionTier::SingleViewBlend, curve);
        std::array keys{activation.plan.CameraKeys()[0], activation.plan.CameraKeys()[1]};
        keys[1].transitionDuration = 3;
        activation.plan = Take(Cinematic::SequenceFrameEvaluationPlan::Create(10, Cinematic::SequenceLoopMode::Once, 4, {}, {}, keys));
        return activation;
    }

    /** @brief Builds the exact scene retained through native camera playback qualification. */
    inline std::unique_ptr<Runtime::RuntimeScene> NativeCameraScene() {
        Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
        for (std::uint64_t id = 1; id <= 2; ++id) {
            Runtime::RuntimeEntityDefinition entity;
            entity.object = {id};
            entity.localTransform.translation.z = id == 1 ? 3.0F : 5.0F;
            entity.components.camera = Runtime::CameraComponent{};
            builder.Add(entity);
        }
        return Take(Runtime::RuntimeScene::Create(Take(std::move(builder).Build()), {1}));
    }

    /** @brief Retains the real player and camera owner while native frames advance and resize. */
    class NativeCameraPlayback final {
    public:
        explicit NativeCameraPlayback(const Cinematic::CameraBlendCurve curve)
            : playback_(Take(CinematicCameraPlayback::Activate(runtime_, camera_, NativeCameraActivation(curve),
                                                               {bindings_, 8, CameraCutEndPolicy::HoldLastUntilPlayerEnd, 1, {1, 1}},
                                                               scene_->View()))) {
            REQUIRE(runtime_.Play(playback_.Player()).HasValue());
            REQUIRE(runtime_.Seek(playback_.Player(), 6).HasValue());
        }

        NativeCameraPlayback(const NativeCameraPlayback &) = delete;
        NativeCameraPlayback(NativeCameraPlayback &&) = delete;
        NativeCameraPlayback &operator=(const NativeCameraPlayback &) = delete;
        NativeCameraPlayback &operator=(NativeCameraPlayback &&) = delete;

        Render::RenderCameraView Advance(const Cinematic::SequenceTime delta) {
            REQUIRE(playback_.Evaluate(delta, scene_->View(), {{}, {}, crossings_}, {}).HasValue());
            return Take(camera_.Commit(++frame_, scene_->View(), std::nullopt)).selected.values;
        }

        Cinematic::SequenceTime Position() const {
            return Take(runtime_.Snapshot(playback_.Player())).position;
        }

    private:
        std::unique_ptr<Runtime::RuntimeScene> scene_{NativeCameraScene()};
        Cinematic::CinematicRuntimeService runtime_{Take(Cinematic::CinematicRuntimeService::Create({{1, 1}}))};
        Runtime::CameraService camera_{Take(Runtime::CameraService::Create({{1, 1}, {1}, Runtime::CameraViewDomain::Runtime}))};
        std::array<Cinematic::CameraCutBinding, 2> bindings_{Cinematic::CameraCutBinding{{1, 1}, {1}},
                                                             Cinematic::CameraCutBinding{{2, 1}, {2}}};
        Cinematic::CinematicCameraPlayback playback_;
        std::array<Cinematic::SequenceFrameCameraCutRequest, 64> crossings_{};
        std::uint64_t frame_{};
    };
}  // namespace Horo::Tests::CameraCuts
