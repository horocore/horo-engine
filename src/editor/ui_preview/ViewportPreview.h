#pragma once

#include <string_view>

namespace Horo::Editor {
    class ILocalizationService;

    namespace Theme {
        struct Fonts;
    }

    /** @brief Draws an in-memory viewport scene using the production overlay. */
    void DrawViewportPreview(const Theme::Fonts &fonts, const ILocalizationService &localization);
}  // namespace Horo::Editor
