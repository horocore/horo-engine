#pragma once
/** @file EditorWindowChrome.h @brief Host-private native window decoration adapter; no native handles escape executable composition. */
#include <array>

struct SDL_Window;

namespace Horo::Editor {
    /** @brief Native decoration projection copied from the active editor theme. */
    struct EditorWindowColors {
        std::array<float, 3> surface;
        std::array<float, 3> text;
        bool operator==(const EditorWindowColors &) const = default;
    };

    /** @brief Main-thread cache for the native decoration surface; the SDL window remains borrowed. */
    class EditorWindowChrome final {
    public:
        /** @brief Applies a changed theme surface before GUI frame traversal.
         * @param window Live primary host window. @param colors Active theme surface and text in sRGB [0,1].
         */
        void Update(SDL_Window &window, const EditorWindowColors &colors);

    private:
        EditorWindowColors applied_{{-1, -1, -1}, {-1, -1, -1}};
    };

    /** @brief Applies owner-thread native decoration styling using only a borrowed SDL host window.
     * @param window Live primary host window. @param colors Normalized active theme surface and text.
     */
    void ApplyNativeEditorWindowChrome(SDL_Window &window, const EditorWindowColors &colors);
}  // namespace Horo::Editor
