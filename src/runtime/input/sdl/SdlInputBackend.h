#pragma once

#include "Horo/Runtime/Input.h"

#include <SDL3/SDL.h>
#include <memory>

namespace Horo::Input {
    /** @brief Window-local candidate position in SDL logical coordinates. */
    struct SdlTextInputArea {
        int x{0};
        int y{0};
        int width{0};
        int height{0};
    };

    /** @brief SDL3 platform collector and baseline haptics adapter. Native types remain private to this target. */
    class SdlInputBackend final : public IGamepadHaptics {
    public:
        SdlInputBackend();
        ~SdlInputBackend() override;
        SdlInputBackend(const SdlInputBackend &) = delete;
        SdlInputBackend &operator=(const SdlInputBackend &) = delete;

        void BeginFrame(FrameNumber frame);
        /** @brief Requests controller gyro collection at the next BeginFrame boundary; disabled by default.
         * @param enabled Host-derived interest from validated effective bindings, never a player assignment.
         * @details The owner serializes this call with polling. Only focused, bound-window collection enables native sensors.
         * Focus loss, disablement and reconnect clear old samples; the next fresh native event must pass both clock fences.
         * Unsupported/native-failed devices remain explicit in their copied sample availability. No sensor fallback occurs.
         */
        void SetGamepadGyroscopeEnabled(bool enabled) noexcept;
        /** @brief Selects the exact window supplying keyboard, pointer, and text events; zero disables window-local input. */
        void BindWindow(SDL_WindowID windowId) noexcept;
        void ProcessEvent(const SDL_Event &event);
        void PollEvents();
        [[nodiscard]] const RawInputSnapshot &Commit();
        [[nodiscard]] RawInputCollector &Collector() noexcept;
        [[nodiscard]] IGamepadHaptics *Haptics() noexcept;

        /** @brief Enables native text/IME for a window after positioning its candidate area. */
        [[nodiscard]] bool StartTextInput(SDL_Window *window, SdlTextInputArea area) const noexcept;
        /** @brief Repositions the native candidate area while a text surface is active. */
        [[nodiscard]] bool SetTextInputArea(SDL_Window *window, SdlTextInputArea area) const noexcept;
        /** @brief Ends native text/IME for a window; safe for a null window. */
        void StopTextInput(SDL_Window *window) const noexcept;

        [[nodiscard]] Result<void> PlayRumble(GamepadDeviceId id, RumbleEffect effect) override;

        [[nodiscard]] Result<void> Stop(GamepadDeviceId id) override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Input
