#pragma once

#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorTheme.h"

#include <imgui.h>
#include <optional>
#include <span>
#include <string_view>

namespace Horo::Editor {
    /** @brief Semantic tone shared by bottom-dock status text and controls. */
    enum class GlobalDockTone {
        Neutral,
        Accent,
        Positive,
        Warning,
        Error,
    };

    /** @brief Presentation input for a shared bottom-dock toolbar chip. */
    struct GlobalDockToolbarChipProps {
        const char *id;
        std::string_view label;
        std::optional<std::size_t> count;
        GlobalDockTone tone{GlobalDockTone::Neutral};
        bool active{false};
        bool toneLabel{false};
        bool disabled{false};
        Ui::UiIcon icon{Ui::UiIcon::None};
    };

    /** @brief Presentation input for a metric card in telemetry-oriented dock panes. */
    struct GlobalDockMetricCardProps {
        std::string_view label;
        std::string_view value;
        std::string_view note;
        GlobalDockTone valueTone{GlobalDockTone::Neutral};
        GlobalDockTone noteTone{GlobalDockTone::Neutral};
        std::span<const float> samples{};
        std::optional<float> budget{};
    };

    /** @brief Resolves a semantic bottom-dock tone through the active theme. */
    [[nodiscard]] ImVec4 GlobalDockToneColor(GlobalDockTone tone) noexcept;

    /** @brief Draws the canonical elevated toolbar surface and bottom divider. */
    void DrawGlobalDockToolbarSurface(ImVec2 origin, float width, float height);

    /** @brief Performs the canonical case-insensitive text match used by searchable dock panes. */
    [[nodiscard]] bool GlobalDockContainsCaseInsensitive(std::string_view value, std::string_view query);

    /** @brief Measures one bottom-dock text run with the provided or fallback font. */
    [[nodiscard]] float MeasureGlobalDockTextWidth(ImFont *font, float size, std::string_view text);

    /**
     * @brief Resolves the search width left after exact trailing toolbar controls and canonical padding.
     * @param toolbarWidth Available toolbar width.
     * @param trailingWidth Exact width reserved for every control following the search field.
     * @return Search width clamped to a drawable positive extent.
     */
    [[nodiscard]] float ResolveGlobalDockSearchWidth(float toolbarWidth, float trailingWidth) noexcept;

    /** @brief Draws the canonical bottom-dock search field and returns the next horizontal position. */
    [[nodiscard]] float DrawGlobalDockSearchControl(ImVec2 origin, float width, std::string_view id, std::span<char> buffer,
                                                    std::string_view hint, const Theme::Fonts &fonts);

    /** @brief Draws the darker table-header surface below a bottom-dock toolbar. */
    void DrawGlobalDockTableHeaderSurface(ImVec2 origin, float width, float height);

    /** @brief Draws the canonical bottom-dock row hover and divider treatment. */
    void DrawGlobalDockTableRowSurface(ImVec2 origin, float width, float height, bool hovered, bool selected = false);

    /** @brief Draws one canonical bottom-dock metric card, including an optional sparkline. */
    void DrawGlobalDockMetricCard(ImVec2 origin, ImVec2 size, const GlobalDockMetricCardProps &props, const Theme::Fonts &fonts);

    /** @brief Draws a responsive grid of canonical metric cards and returns its height. */
    [[nodiscard]] float DrawGlobalDockMetricGrid(ImVec2 origin, float width, std::span<const GlobalDockMetricCardProps> cards,
                                                 const Theme::Fonts &fonts);

    /** @brief Draws a canonical value meter used by audio and network panes. */
    void DrawGlobalDockMeter(ImVec2 origin, float width, float progress);

    /** @brief Draws the canonical footer surface and top divider. */
    void DrawGlobalDockFooterSurface(ImVec2 origin, float width, float height);

    /** @brief Draws canonical left-aligned footer segments and one right-aligned status. */
    void DrawGlobalDockStatusFooter(ImVec2 origin, float width, std::span<const std::string_view> segments, std::string_view status,
                                    const Theme::Fonts &fonts);

    /** @brief Measures a toolbar chip using the active typography and layout tokens. */
    [[nodiscard]] float MeasureGlobalDockToolbarChip(const GlobalDockToolbarChipProps &props, const Theme::Fonts &fonts);

    /** @brief Draws one canonical bottom-dock toolbar chip at an exact screen-space position. */
    [[nodiscard]] bool DrawGlobalDockToolbarChip(ImVec2 origin, float width, const GlobalDockToolbarChipProps &props,
                                                 const Theme::Fonts &fonts);

    /** @brief Draws a toolbar separator using the canonical control height. */
    void DrawGlobalDockToolbarSeparator(float x, float y);

    /** @brief Draws the canonical rounded state pill and returns its measured width. */
    [[nodiscard]] float DrawGlobalDockStatePill(ImVec2 origin, std::string_view label, GlobalDockTone tone, const Theme::Fonts &fonts);

    /** @brief Draws the canonical five-pixel progress track. */
    void DrawGlobalDockProgressBar(ImVec2 origin, float width, float progress);

    /** @brief Draws clipped, single-line text without leaking feature-local typography. */
    void DrawGlobalDockClippedText(ImDrawList &drawList, ImFont *font, float fontSize, ImVec2 minimum, ImVec2 maximum, ImVec4 color,
                                   std::string_view text);

    /** @brief Draws a single line with an ellipsis when the available width is too small. */
    void DrawGlobalDockEllipsizedText(ImDrawList &drawList, ImFont *font, float fontSize, ImVec2 minimum, ImVec2 maximum, ImVec4 color,
                                      std::string_view text);
}  // namespace Horo::Editor
