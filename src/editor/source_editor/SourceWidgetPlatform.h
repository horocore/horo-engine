#pragma once

/** @file SourceWidgetPlatform.h
 * @brief Target-private Zep/ImGui bridge. No third-party declarations cross SourceEditorWidget.h.
 */
#include "SourceEditorTheme.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <zep/imgui/display_imgui.h>
#include <zep/mode_standard.h>

namespace Horo::Editor::Theme {
    struct Fonts;
}

namespace Horo::Editor {
    class SourceDocumentSnapshot;
    struct SourceWidgetDecoration;
    struct SourceWidgetFrame;

    /** @brief Validates and prepares every external marker before replacing the current private projection.
     * @param buffer This view's LF presentation mirror. @param source Exact authoritative publication.
     * @param decorations Bounded document byte ranges admitted by the widget's revision fence.
     * @return False without replacing markers when any decoration is invalid.
     */
    [[nodiscard]] bool PublishSourceWidgetDecorations(Zep::ZepBuffer &buffer, const SourceDocumentSnapshot &source,
                                                      std::span<const SourceWidgetDecoration> decorations);

    /** @brief Rejects invalid GUI layout geometry. @param frame Requested viewport and scale.
     * @return Whether dimensions and scale are finite and positive.
     */
    [[nodiscard]] bool ValidSourceWidgetFrame(const SourceWidgetFrame &frame) noexcept;

    /** @brief Uses the host font and records the insertion caret for platform IME placement. */
    class WidgetDisplay final : public Zep::ZepDisplay_ImGui {
    public:
        /** @brief Returns the atlas generation used to invalidate Zep's layout cache. */
        [[nodiscard]] std::uint64_t FontGeneration() const noexcept {
            return fontGeneration;
        }

        /** @brief Returns the caret recorded by this frame's synchronous drawing. */
        [[nodiscard]] const std::optional<Zep::NRectf> &Cursor() const noexcept {
            return cursor;
        }

        /** @brief Sets the palette color used to identify insertion-caret rectangles. @param color Active cursor color. */
        void SetCursorColor(const Zep::NVec4f &color) noexcept {
            cursorColor = color;
        }

        /** @brief Applies immutable active-frame styles and borrowed host-owned atlas faces.
         * @param theme Current source theme. @param fonts Optional host atlas; null uses the active GUI font.
         */
        void Configure(const SourceEditorTheme &theme, const Theme::Fonts *fonts);
        /** @brief Binds the exact current mirror and its optional validated syntax during synchronous drawing.
         * @param buffer Current private mirror. @param syntax Normalized spans, or null while an edit awaits publication.
         */
        void Bind(const Zep::ZepBuffer *buffer, const SourceSyntax *syntax) noexcept;
        /** @brief Returns the source body's stable, equal-cell layout font. */
        Zep::ZepFont &GetFont(Zep::ZepTextType type) override;
        /** @brief Draws source glyphs using semantic color/font roles; non-source chrome retains its supplied style. */
        void DrawChars(Zep::ZepFont &font, const Zep::NVec2f &position, const Zep::NVec4f &color, const uint8_t *begin,
                       const uint8_t *end) const override;
        /** @brief Retains the exact Zep clip for canonical ImGui glyph emission. @param rect Current private draw clip. */
        void SetClipRect(const Zep::NRectf &rect) override;
        /** @brief Records the insertion caret for the platform IME bridge. */
        void DrawRectFilled(const Zep::NRectf &rect, const Zep::NVec4f &color) const override;

    private:
        Zep::NVec4f cursorColor;
        mutable std::optional<Zep::NRectf> cursor;
        std::uint64_t fontGeneration{};
        /** @brief Maps only pointers inside the current mirror allocation. @param begin Drawn glyph pointer. @return Normalized byte
         * offset, or no source glyph. */
        [[nodiscard]] std::optional<std::size_t> SourceOffset(const uint8_t *begin) const noexcept;
        /** @brief Checks the current normalized inclusive selection. @param offset Display byte offset. @return Whether the source glyph is
         * selected. */
        [[nodiscard]] bool Selected(std::size_t offset) const noexcept;
        std::array<ImFont *, static_cast<std::size_t>(Zep::ZepTextType::Count)> fonts_{};
        ImFont *regular_{};
        ImFont *emphasisFace_{};
        std::shared_ptr<Zep::ZepFont_ImGui> emphasis_;
        int fontPixels_{};
        SourceEditorTheme theme_;
        Zep::NRectf clip_;
        const Zep::ZepBuffer *buffer_{}; /**< Borrowed only by the owning synchronous Draw call. */
        const SourceSyntax *syntax_{};   /**< Borrowed validated display coordinates for the same mirror. */
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
     * @param editor This view's private editor instance. @param theme Active semantic source styles.
     */
    void UpdateSourceWidgetTheme(const Zep::ZepEditor &editor, const SourceEditorTheme &theme);
}  // namespace Horo::Editor
