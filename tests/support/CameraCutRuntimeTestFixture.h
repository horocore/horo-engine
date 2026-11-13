#pragma once

#include "CameraCutTestSupport.h"

#include <array>
#include <memory>
#include <optional>
#include <utility>

namespace Horo::Tests::CameraCuts {
    /** @brief Retains camera/runtime owners and current Scene publication through each regression. */
    struct RuntimeCameraFixture final {
        std::unique_ptr<RuntimeScene> scene{Take(RuntimeScene::Create(SceneDefinition(), SceneRuntimeId{1}))};
        CinematicRuntimeService runtime{Take(CinematicRuntimeService::Create({{1, 1}}))};
        CameraService camera{Take(CameraService::Create({{1, 1}, scene->View().RuntimeId(), CameraViewDomain::Runtime}))};
        std::array<CameraCutBinding, 2> bindings{CameraCutBinding{{1, 1}, {1}}, CameraCutBinding{{2, 1}, {2}}};
        std::array<SequenceFrameCameraCutRequest, 64> crossings{};
        std::optional<CinematicCameraPlayback> playback;
        std::uint64_t frame{};

        CameraCutActivation Settings(const CameraCutEndPolicy policy = CameraCutEndPolicy::HoldLastUntilPlayerEnd) const {
            return {bindings, 8, policy, 1, {1, 1}};
        }

        void Start(const SequenceLoopMode loop = SequenceLoopMode::Once,
                   const CameraCutEndPolicy policy = CameraCutEndPolicy::HoldLastUntilPlayerEnd) {
            playback.emplace(Take(CinematicCameraPlayback::Activate(runtime, camera, Activation(loop), Settings(policy), scene->View())));
            REQUIRE(runtime.Play(playback->Player()).HasValue());
            REQUIRE(playback->Publish(scene->View()).HasValue());
        }

        void Advance(const SequenceTime delta) {
            REQUIRE(playback->Evaluate(delta, scene->View(), {{}, {}, crossings}, {}).HasValue());
        }

        void StartTransition(SequencePlaybackActivation activation = TransitionActivation()) {
            playback.emplace(Take(CinematicCameraPlayback::Activate(runtime, camera, std::move(activation), Settings(), scene->View())));
            REQUIRE(runtime.Play(playback->Player()).HasValue());
        }

        void Seek(const SequenceTime position) {
            REQUIRE(runtime.Seek(playback->Player(), position).HasValue());
            REQUIRE(playback->Publish(scene->View()).HasValue());
        }

        CameraSelectionSnapshot Commit() {
            const auto base = Take(ResolveSceneCamera(scene->View(), *scene->View().Find({3})));
            return Take(camera.Commit(++frame, scene->View(), base));
        }
    };
}  // namespace Horo::Tests::CameraCuts
