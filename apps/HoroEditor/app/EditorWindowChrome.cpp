#include "EditorWindowChrome.h"

#if defined(_WIN32)
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <windows.h>
#endif

namespace Horo::Editor {
    /** @copydoc EditorWindowChrome::Update */
    void EditorWindowChrome::Update(SDL_Window &window, const EditorWindowColors &colors) {
        if (applied_ == colors)
            return;
        ApplyNativeEditorWindowChrome(window, colors);
        applied_ = colors;
    }

#if !defined(__APPLE__)
    /** @copydoc ApplyNativeEditorWindowChrome */
    void ApplyNativeEditorWindowChrome(SDL_Window &window, const EditorWindowColors &colors) {
#if defined(_WIN32)
        const auto &surface = colors.surface;
        const auto native =
            static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(&window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (!native)
            return;
        const auto byte = [](const float value) {
            return static_cast<BYTE>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
        };
        const COLORREF caption = RGB(byte(surface[0]), byte(surface[1]), byte(surface[2]));
        const BOOL dark = surface[0] * 0.2126F + surface[1] * 0.7152F + surface[2] * 0.0722F < 0.5F;
        const COLORREF text = RGB(byte(colors.text[0]), byte(colors.text[1]), byte(colors.text[2]));
        // Unsupported decoration attributes on older Windows versions leave native defaults intact.
        static_cast<void>(DwmSetWindowAttribute(native, 20, &dark, sizeof(dark)));
        static_cast<void>(DwmSetWindowAttribute(native, 35, &caption, sizeof(caption)));
        static_cast<void>(DwmSetWindowAttribute(native, 36, &text, sizeof(text)));
#else
        // Linux window-manager decorations do not expose a portable per-window color API.
        static_cast<void>(window);
        static_cast<void>(colors);
#endif
    }
#endif
}  // namespace Horo::Editor
