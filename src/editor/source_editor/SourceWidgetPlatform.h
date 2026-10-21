#pragma once

/** @file SourceWidgetPlatform.h
 * @brief Target-private Zep/ImGui bridge. No third-party declarations cross SourceEditorWidget.h.
 */
#include <algorithm>
#include <array>
#include <cassert>
#include <memory>
#include <optional>
#include <zep/imgui/display_imgui.h>
#include <zep/mode_standard.h>

namespace Horo::Editor {
    /** @brief Uses the host font and records the insertion caret for platform IME placement. */
    class WidgetDisplay final : public Zep::ZepDisplay_ImGui {
    public:
        Zep::NVec4f cursorColor;
        mutable std::optional<Zep::NRectf> cursor;

        Zep::ZepFont &GetFont(const Zep::ZepTextType type) override {
            const auto index = static_cast<std::size_t>(type);
            auto *font = ImGui::GetFont();
            const auto &io = ImGui::GetIO();
            if (!font)
                font = io.FontDefault;
            if (!font && !io.Fonts->Fonts.empty())
                font = io.Fonts->Fonts.front();
            assert(font);
            const float pixels = ImGui::GetFontSize() > 0 ? ImGui::GetFontSize() : font->FontSize * io.FontGlobalScale;
            if (!m_fonts[index] || fonts_[index] != font) {
                m_fonts[index] = std::make_shared<Zep::ZepFont_ImGui>(*this, font, std::max(1, static_cast<int>(pixels)));
                fonts_[index] = font;
            }
            return *m_fonts[index];
        }

        void DrawRectFilled(const Zep::NRectf &rect, const Zep::NVec4f &color) const override {
            if (color == cursorColor && rect.Width() <= GetPixelScale().x && rect.Height() >= ImGui::GetFontSize())
                cursor = rect;
            Zep::ZepDisplay_ImGui::DrawRectFilled(rect, color);
        }

    private:
        std::array<ImFont *, static_cast<std::size_t>(Zep::ZepTextType::Count)> fonts_{};
    };

    /** @brief Keeps externally restored half-open selections consistent with Zep's standard interaction mode. */
    class WidgetMode final : public Zep::ZepMode_Standard {
    public:
        explicit WidgetMode(Zep::ZepEditor &editor) : ZepMode_Standard(editor) {}

        /** @brief Restores the mode's anchor/caret together with the visible selection. */
        void Select(Zep::ZepBuffer &buffer, Zep::GlyphIterator anchor, Zep::GlyphIterator caret);
    };

    /** @brief Creates a synchronous editor with an owned denied filesystem and private GUI display.
     * @return Isolated widget core; never reads config or source files.
     */
    [[nodiscard]] std::unique_ptr<Zep::ZepEditor> MakeSourceWidgetEditor();
    /** @brief Refreshes the widget's private palette from the current semantic GUI style.
     * @param editor This view's private editor instance.
     */
    void UpdateSourceWidgetTheme(Zep::ZepEditor &editor);
}  // namespace Horo::Editor
