/** @file SourceEditorTheme.h
 * @brief Per-frame semantic source styles, independent of the embedded widget library.
 */
#pragma once
#include "editor/source_editor/SourceSyntax.h"

#include <array>

namespace Horo::Editor::DesignSystem {
    struct DesignTokens;
}

namespace Horo::Editor {
    /** @brief Equal-cell source typography roles; neither role reduces the body text size. */
    enum class SourceFontRole {
        Monospace,
        MonospaceEmphasis
    };
    /** @brief RGBA value owned by Horo; no widget or renderer-native color identity. */
    using SourceColor = std::array<float, 4>;

    /** @brief Resolved style for a lexical role. */
    struct SourceSyntaxStyle {
        SourceColor color{};
        SourceFontRole font{SourceFontRole::Monospace};
    };

    /** @brief Small frame value rebuilt from active theme tokens, never cached with document syntax. */
    struct SourceEditorTheme {
        SourceColor background{};
        SourceColor selection{};
        SourceColor cursor{};
        float fontPixels{};
        std::array<SourceSyntaxStyle, static_cast<std::size_t>(SourceSyntaxRole::Count)> styles{};
    };

    /** @brief Resolves source roles and enforces 4.5:1 foreground contrast against the source background.
     * @param tokens Current immutable frame tokens, including scale and custom source overrides.
     * @return Complete source style value to apply to every presented view in the current frame.
     */
    [[nodiscard]] SourceEditorTheme ResolveSourceEditorTheme(const DesignSystem::DesignTokens &tokens) noexcept;
    /** @brief Computes WCAG contrast for opaque sRGB source colors.
     * @param foreground Opaque foreground. @param background Opaque source background. @return Ratio in [1,21].
     */
    [[nodiscard]] float SourceContrast(const SourceColor &foreground, const SourceColor &background) noexcept;
}  // namespace Horo::Editor
