#include "editor/source_editor/SourceEditorTheme.h"

#include "Horo/Editor/DesignSystem/DesignTokens.h"

#include <algorithm>
#include <cmath>

namespace Horo::Editor {
    namespace {
        /** @brief Keeps extension-supplied channels within the source renderer's sRGB contract. */
        [[nodiscard]] float Channel(const float value) noexcept {
            return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F;
        }

        /** @brief Converts the editor's color carrier at the private GUI boundary. */
        [[nodiscard]] SourceColor Color(const ImVec4 &color) noexcept {
            return {Channel(color.x), Channel(color.y), Channel(color.z), Channel(color.w)};
        }

        /** @brief Linearizes one sRGB channel for the WCAG relative-luminance formula. */
        [[nodiscard]] float Linear(const float channel) noexcept {
            return channel <= 0.04045F ? channel / 12.92F : std::pow((channel + 0.055F) / 1.055F, 2.4F);
        }

        /** @brief Computes relative luminance of opaque sRGB. */
        [[nodiscard]] float Luminance(const SourceColor &color) noexcept {
            return 0.2126F * Linear(color[0]) + 0.7152F * Linear(color[1]) + 0.0722F * Linear(color[2]);
        }

        /** @brief Composes the active selection overlay over the opaque source surface. */
        [[nodiscard]] SourceColor SelectionSurface(const SourceColor &overlay, const SourceColor &background) noexcept {
            SourceColor result = background;
            for (std::size_t channel = 0; channel < 3; ++channel)
                result[channel] = overlay[channel] * overlay[3] + background[channel] * (1.0F - overlay[3]);
            result[3] = 1.0F;
            return result;
        }

        /** @brief Preserves the theme hue where possible, moving only toward the nearer readable luminance endpoint. */
        [[nodiscard]] SourceColor Readable(SourceColor color, const SourceColor &background) noexcept {
            color[3] = 1.0F;
            if (SourceContrast(color, background) >= 4.5F)
                return color;
            const float endpoint = Luminance(background) > 0.179F ? 0.0F : 1.0F;
            const auto original = color;
            // Fixed work bound; endpoints are contrast correction limits, never a second theme palette.
            for (int step = 1; step <= 20; ++step) {
                const float amount = static_cast<float>(step) / 20.0F;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    color[channel] = original[channel] + (endpoint - original[channel]) * amount;
                if (SourceContrast(color, background) >= 4.5F)
                    break;
            }
            return color;
        }
    }  // namespace

    /** @copydoc SourceContrast */
    float SourceContrast(const SourceColor &foreground, const SourceColor &background) noexcept {
        const auto a = Luminance(foreground);
        const auto b = Luminance(background);
        return (std::max(a, b) + 0.05F) / (std::min(a, b) + 0.05F);
    }

    /** @copydoc ResolveSourceEditorTheme */
    SourceEditorTheme ResolveSourceEditorTheme(const DesignSystem::DesignTokens &tokens) noexcept {
        const auto &colors = tokens.colors;
        const auto &syntax = tokens.sourceSyntax;
        SourceEditorTheme result{
            .background = Color(colors.surfaceWindow),
            .selection = Color(colors.actionPrimarySoft),
            .cursor = Readable(Color(colors.textPrimary), Color(colors.surfaceWindow)),
            .fontPixels = tokens.typography.body,
        };
        result.background[3] = 1.0F;
        result.selectionSurface = SelectionSurface(result.selection, result.background);
        const std::array palette{
            syntax.text.value_or(colors.textPrimary),        syntax.keyword.value_or(colors.actionPrimary),
            syntax.type.value_or(colors.statusWarn),         syntax.string.value_or(colors.statusOk),
            syntax.number.value_or(colors.statusWarn),       syntax.comment.value_or(colors.textMuted),
            syntax.directive.value_or(colors.actionPrimary), syntax.property.value_or(colors.textPrimary),
        };
        for (std::size_t index = 0; index < palette.size(); ++index) {
            const auto role = static_cast<SourceSyntaxRole>(index);
            result.styles[index] = {.color = Readable(Color(palette[index]), result.background),
                                    .selectedColor = Readable(Color(palette[index]), result.selectionSurface),
                                    .font = role == SourceSyntaxRole::Keyword || role == SourceSyntaxRole::Directive
                                                ? SourceFontRole::MonospaceEmphasis
                                                : SourceFontRole::Monospace};
        }
        return result;
    }
}  // namespace Horo::Editor
