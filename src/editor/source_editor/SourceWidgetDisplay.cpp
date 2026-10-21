#include "Horo/Editor/EditorTheme.h"
#include "SourceWidgetPlatform.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace Horo::Editor {
    namespace {
        /** @brief Resolves the populated host atlas before the first GUI frame, preserving the widget host precondition. */
        [[nodiscard]] ImFont *ActiveAtlasFont() {
            auto *font = ImGui::GetFont();
            const auto &io = ImGui::GetIO();
            if (!font)
                font = io.FontDefault;
            if (!font && !io.Fonts->Fonts.empty())
                font = io.Fonts->Fonts.front();
            assert(font);
            return font;
        }

        /** @brief Resolves valid initial metrics before ImGui has started its first frame. */
        [[nodiscard]] int InitialFontPixels(const ImFont &font) {
            const float pixels = ImGui::GetFontSize() > 0 ? ImGui::GetFontSize() : font.FontSize * ImGui::GetIO().FontGlobalScale;
            return std::max(1, static_cast<int>(pixels));
        }

        /** @brief Converts Horo's source color only at the target-private widget boundary. */
        [[nodiscard]] Zep::NVec4f NativeColor(const SourceColor &color) noexcept {
            return {color[0], color[1], color[2], color[3]};
        }
    }  // namespace

    /** @copydoc WidgetDisplay::Configure */
    void WidgetDisplay::Configure(const SourceEditorTheme &theme, const Theme::Fonts *fonts) {
        ImFont *regular = fonts && fonts->monospace ? fonts->monospace : ActiveAtlasFont();
        ImFont *emphasis = fonts && fonts->monospaceEmphasis ? fonts->monospaceEmphasis : regular;
        const int pixels = static_cast<int>(std::lround(theme.fontPixels));
        if (regular_ != regular || fontPixels_ != pixels)
            SetLayoutDirty();
        regular_ = regular;
        fontPixels_ = pixels;
        theme_ = theme;
        if (!emphasis_ || emphasisFace_ != emphasis) {
            emphasis_ = std::make_shared<Zep::ZepFont_ImGui>(*this, emphasis, pixels);
            emphasisFace_ = emphasis;
        }
        if (emphasis_->GetPixelHeight() != pixels)
            emphasis_->SetPixelHeight(pixels);
    }

    /** @copydoc WidgetDisplay::Bind */
    void WidgetDisplay::Bind(const Zep::ZepBuffer *buffer, const SourceSyntax *syntax) noexcept {
        buffer_ = buffer;
        syntax_ = syntax;
    }

    /** @copydoc WidgetDisplay::GetFont */
    Zep::ZepFont &WidgetDisplay::GetFont(const Zep::ZepTextType type) {
        const auto index = static_cast<std::size_t>(type);
        auto *font = regular_ ? regular_ : ActiveAtlasFont();
        const int pixels = fontPixels_ > 0 ? fontPixels_ : InitialFontPixels(*font);
        if (!m_fonts[index] || fonts_[index] != font) {
            m_fonts[index] = std::make_shared<Zep::ZepFont_ImGui>(*this, font, pixels);
            fonts_[index] = font;
            ++fontGeneration;
        }
        if (m_fonts[index]->GetPixelHeight() != pixels) {
            m_fonts[index]->SetPixelHeight(pixels);
            ++fontGeneration;
        }
        return *m_fonts[index];
    }

    /** @copydoc WidgetDisplay::SourceOffset */
    std::optional<std::size_t> WidgetDisplay::SourceOffset(const uint8_t *begin) const noexcept {
        if (!buffer_)
            return std::nullopt;
        const auto &gap = buffer_->GetWorkingBuffer();
        const auto address = reinterpret_cast<std::uintptr_t>(begin);
        const auto start = reinterpret_cast<std::uintptr_t>(gap.m_pStart);
        const auto gapStart = reinterpret_cast<std::uintptr_t>(gap.m_pGapStart);
        const auto gapEnd = reinterpret_cast<std::uintptr_t>(gap.m_pGapEnd);
        const auto end = reinterpret_cast<std::uintptr_t>(gap.m_pEnd);
        if (address < start || address >= end)
            return std::nullopt;
        if (address < gapStart)
            return address - start;
        if (address < gapEnd)
            return std::nullopt;
        return (gapStart - start) + (address - gapEnd);
    }

    /** @copydoc WidgetDisplay::Selected */
    bool WidgetDisplay::Selected(const std::size_t offset) const noexcept {
        if (!buffer_ || !buffer_->HasSelection())
            return false;
        const auto range = buffer_->GetInclusiveSelection();
        return offset >= static_cast<std::size_t>(range.first.Index()) && offset <= static_cast<std::size_t>(range.second.Index());
    }

    /** @copydoc WidgetDisplay::DrawChars */
    void WidgetDisplay::DrawChars(Zep::ZepFont &font, const Zep::NVec2f &position, const Zep::NVec4f &color, const uint8_t *begin,
                                  const uint8_t *end) const {
        if (const auto offset = SourceOffset(begin)) {
            const auto role = syntax_ ? syntax_->RoleAt(*offset) : SourceSyntaxRole::Text;
            const auto &style = theme_.styles[static_cast<std::size_t>(role)];
            Zep::ZepFont &face = style.font == SourceFontRole::MonospaceEmphasis && emphasis_ ? *emphasis_ : font;
            const auto &foreground = Selected(*offset) ? style.selectedColor : style.color;
            auto *drawList = ImGui::GetWindowDrawList();
            const bool clipped = clip_.Width() != 0;
            if (clipped)
                drawList->PushClipRect(Zep::toImVec2(clip_.topLeftPx), Zep::toImVec2(clip_.bottomRightPx));
            const auto glyphColor = ImGui::GetColorU32(ImVec4{foreground[0], foreground[1], foreground[2], foreground[3]});
            drawList->AddText(static_cast<Zep::ZepFont_ImGui &>(face).GetImFont(), static_cast<float>(face.GetPixelHeight()),
                              Zep::toImVec2(position), glyphColor, reinterpret_cast<const char *>(begin),
                              reinterpret_cast<const char *>(end));
            if (clipped)
                drawList->PopClipRect();
        } else {
            Zep::ZepDisplay_ImGui::DrawChars(font, position, color, begin, end);
        }
    }

    /** @copydoc WidgetDisplay::SetClipRect */
    void WidgetDisplay::SetClipRect(const Zep::NRectf &rect) {
        clip_ = rect;
        Zep::ZepDisplay_ImGui::SetClipRect(rect);
    }

    /** @copydoc WidgetDisplay::DrawRectFilled */
    void WidgetDisplay::DrawRectFilled(const Zep::NRectf &rect, const Zep::NVec4f &color) const {
        if (color == cursorColor && rect.Width() <= GetPixelScale().x && rect.Height() >= static_cast<float>(fontPixels_))
            cursor = rect;
        Zep::ZepDisplay_ImGui::DrawRectFilled(rect, color);
    }

    /** @copydoc UpdateSourceWidgetTheme */
    void UpdateSourceWidgetTheme(Zep::ZepEditor &editor, const SourceEditorTheme &styles) {
        auto &theme = editor.GetTheme();
        const auto text = NativeColor(styles.styles[static_cast<std::size_t>(SourceSyntaxRole::Text)].color);
        for (int value = 1; value < static_cast<int>(Zep::ThemeColor::UniqueColor0); ++value)
            theme.SetColor(static_cast<Zep::ThemeColor>(value), text);
        for (const auto color : {Zep::ThemeColor::Background, Zep::ThemeColor::LineNumberBackground, Zep::ThemeColor::AirlineBackground,
                                 Zep::ThemeColor::TabInactive})
            theme.SetColor(color, NativeColor(styles.background));
        for (const auto color : {Zep::ThemeColor::TextDim, Zep::ThemeColor::LineNumber, Zep::ThemeColor::HiddenText})
            theme.SetColor(color, NativeColor(styles.styles[static_cast<std::size_t>(SourceSyntaxRole::Comment)].color));
        theme.SetColor(Zep::ThemeColor::CursorInsert, NativeColor(styles.cursor));
        theme.SetColor(Zep::ThemeColor::CursorNormal, NativeColor(styles.cursor));
        theme.SetColor(Zep::ThemeColor::VisualSelectBackground, NativeColor(styles.selection));
        theme.SetColor(Zep::ThemeColor::CursorLineBackground, NativeColor(styles.background));
        const auto &colors = Theme::GetActiveTokens().colors;
        theme.SetColor(Zep::ThemeColor::Error, Zep::toNVec4f(colors.statusError));
        theme.SetColor(Zep::ThemeColor::Warning, Zep::toNVec4f(colors.statusWarn));
        theme.SetColor(Zep::ThemeColor::Info, Zep::toNVec4f(colors.actionPrimary));
    }
}  // namespace Horo::Editor
