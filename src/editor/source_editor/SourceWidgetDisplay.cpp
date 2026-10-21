#include "Horo/Editor/EditorTheme.h"
#include "SourceWidgetPlatform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Horo::Editor {
    namespace {
        /** @brief Converts Horo's source color only at the target-private widget boundary. */
        [[nodiscard]] Zep::NVec4f NativeColor(const SourceColor &color) noexcept {
            return {color[0], color[1], color[2], color[3]};
        }
    }  // namespace

    /** @copydoc WidgetDisplay::Configure */
    void WidgetDisplay::Configure(const SourceEditorTheme &theme, const Theme::Fonts *fonts) {
        ImFont *regular = fonts && fonts->monospace ? fonts->monospace : ImGui::GetFont();
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
        auto *font = regular_ ? regular_ : ImGui::GetFont();
        const int pixels = fontPixels_ > 0 ? fontPixels_ : static_cast<int>(ImGui::GetFontSize());
        if (!m_fonts[index] || fonts_[index] != font) {
            m_fonts[index] = std::make_shared<Zep::ZepFont_ImGui>(*this, font, pixels);
            fonts_[index] = font;
        }
        if (m_fonts[index]->GetPixelHeight() != pixels)
            m_fonts[index]->SetPixelHeight(pixels);
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

    /** @copydoc WidgetDisplay::DrawChars */
    void WidgetDisplay::DrawChars(Zep::ZepFont &font, const Zep::NVec2f &position, const Zep::NVec4f &color, const uint8_t *begin,
                                  const uint8_t *end) const {
        if (const auto offset = SourceOffset(begin)) {
            const auto role = syntax_ ? syntax_->RoleAt(*offset) : SourceSyntaxRole::Text;
            const auto &style = theme_.styles[static_cast<std::size_t>(role)];
            Zep::ZepFont &face = style.font == SourceFontRole::MonospaceEmphasis && emphasis_ ? *emphasis_ : font;
            Zep::ZepDisplay_ImGui::DrawChars(face, position, NativeColor(style.color), begin, end);
        } else {
            Zep::ZepDisplay_ImGui::DrawChars(font, position, color, begin, end);
        }
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
