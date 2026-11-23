#pragma once

#include "SdlInputBackend.h"

namespace Horo::Tests {
    /** @brief Delivers the window-state observation a synthetic SDL surface cannot query natively. */
    inline void SendSdlWindowEvent(Input::SdlInputBackend &backend, SDL_WindowID window, SDL_EventType type) {
        const SDL_Event event{.window = {.type = type, .windowID = window}};
        backend.ProcessEvent(event);
    }

    /** @brief Binds a synthetic surface and observes both keyboard and pointer focus. */
    inline void BindFocusedSdlTestWindow(Input::SdlInputBackend &backend, SDL_WindowID window) {
        backend.BindWindow(window);
        SendSdlWindowEvent(backend, window, SDL_EVENT_WINDOW_FOCUS_GAINED);
        SendSdlWindowEvent(backend, window, SDL_EVENT_WINDOW_MOUSE_ENTER);
    }
}  // namespace Horo::Tests
