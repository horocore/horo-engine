#pragma once

#include "CameraCutTestSupport.h"
#include "Horo/Runtime/Render/RenderFrontend.h"

#include <SDL3/SDL.h>
#include <iostream>
#include <memory>

namespace Horo::Tests::CameraCuts {
    /** @brief Native presentation observations applied before cinematic camera publication. */
    enum class NativeOutputChange {
        None,
        Fullscreen,
        SecondDisplay
    };

    /** @brief Requires a real second display before allocating any native test resources. */
    inline void RequireDisplayConfiguration(const NativeOutputChange change) {
        if (change != NativeOutputChange::SecondDisplay)
            return;
        int count = 0;
        const std::unique_ptr<SDL_DisplayID, decltype(&SDL_free)> displays{SDL_GetDisplays(&count), SDL_free};
        REQUIRE(displays);
        if (count < 2) {
            SDL_Quit();
            SKIP("Cinematic display migration requires two native displays");
        }
    }

    /** @brief Observes an actual native output transition before the next cinematic frame. */
    inline Render::FramebufferExtent ChangeNativeOutput(SDL_Window &window, Render::RenderFrontend &frontend,
                                                        const NativeOutputChange change) {
        if (change == NativeOutputChange::None)
            return {640, 480};
        REQUIRE(SDL_ShowWindow(&window));
        REQUIRE(SDL_SyncWindow(&window));
        if (change == NativeOutputChange::Fullscreen) {
            REQUIRE(SDL_SetWindowFullscreen(&window, true));
            REQUIRE(SDL_SyncWindow(&window));
            CHECK((SDL_GetWindowFlags(&window) & SDL_WINDOW_FULLSCREEN) != 0);
        } else {
            int count = 0;
            const std::unique_ptr<SDL_DisplayID, decltype(&SDL_free)> displays{SDL_GetDisplays(&count), SDL_free};
            REQUIRE(displays);
            REQUIRE(count >= 2);
            const auto current = SDL_GetDisplayForWindow(&window);
            REQUIRE(current != 0);
            const auto destination = displays.get()[0] == current ? displays.get()[1] : displays.get()[0];
            REQUIRE(
                SDL_SetWindowPosition(&window, SDL_WINDOWPOS_CENTERED_DISPLAY(destination), SDL_WINDOWPOS_CENTERED_DISPLAY(destination)));
            REQUIRE(SDL_SyncWindow(&window));
            REQUIRE(SDL_GetDisplayForWindow(&window) == destination);
        }
        SDL_PumpEvents();
        int width = 0;
        int height = 0;
        REQUIRE(SDL_GetWindowSizeInPixels(&window, &width, &height));
        REQUIRE(width > 0);
        REQUIRE(height > 0);
        const Render::FramebufferExtent extent{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        REQUIRE(frontend.Resize(extent).HasValue());
        std::cout << "Cinematic native output change " << static_cast<int>(change) << ", display " << SDL_GetDisplayForWindow(&window)
                  << ", pixels " << extent.width << 'x' << extent.height << '\n';
        return extent;
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
            : playback_(Take(CinematicCameraPlayback::Activate(runtime_, camera_,
                                                               TransitionActivation(CameraTransitionTier::SingleViewBlend, curve,
                                                                                    CameraTransitionFallback::Fail, 3),
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
