#include "ViewportPlayCaptions.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Runtime/Ui/UiCaptionSettings.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <limits>

namespace Horo::Editor {
    namespace {
        using namespace Runtime::Ui;
        constexpr float LogicalUnits = 64.0F;

        /** @brief Resolves an exact identity in a pinned catalog without creating a missing-key placeholder. */
        const std::string *FindText(const std::shared_ptr<const LocalizationCatalog> &catalog, MessageKeyView key) {
            if (!catalog)
                return nullptr;
            const auto found = catalog->messages.find(key);
            return found == catalog->messages.end() ? nullptr : &found->second;
        }

        /** @brief Resolves the captured locale, its source locale, then the project-authored fallback. */
        std::string_view ResolveText(const LocalizedMessageRef &message, const LocalizationSnapshot &locale) {
            const MessageKeyView key{message.Key().namespaceId, {}, message.Key().localKey};
            if (const auto *text = FindText(locale.catalog, key))
                return *text;
            if (const auto *text = FindText(locale.fallbackCatalog, key))
                return *text;
            return message.FailurePolicy() == UiLocalizedTextFailurePolicy::UseFallback ? message.FallbackText() : std::string_view{};
        }

        /** @brief Rejects unrepresentable viewport or text extents before conversion to integer logical units. */
        bool FitsLogical(ImVec2 extent) noexcept {
            constexpr auto max = std::numeric_limits<std::int32_t>::max();
            return extent.x > 0 && extent.y > 0 && std::ceil(static_cast<double>(extent.x) * LogicalUnits) <= max &&
                   std::ceil(static_cast<double>(extent.y) * LogicalUnits) <= max;
        }

        /** @brief Converts an already validated extent to 1/64 DIP. */
        UiLogicalExtent LogicalExtent(ImVec2 extent) noexcept {
            return {static_cast<std::int32_t>(std::ceil(static_cast<double>(extent.x) * LogicalUnits)),
                    static_cast<std::int32_t>(std::ceil(static_cast<double>(extent.y) * LogicalUnits))};
        }

        /** @brief Selects only current dialogue from this semantic timeline. */
        bool VisibleDialogue(const CaptionActiveCue &active, std::uint64_t tick) noexcept {
            return active.producer == CaptionProducerKind::Semantic && active.cue.kind == CaptionCueKind::Dialogue &&
                   active.cue.startTick <= tick;
        }

        struct LabelStyle final {
            ImFont &font;
            float size;
            float wrap;
            ImVec2 padding;
        };

        struct MeasuredLabels final {
            std::array<CaptionMeasuredCue, MaximumCaptionCues> cues{};
            std::array<std::string_view, MaximumCaptionCues> text{};
            std::size_t count{};
        };

        /** @brief Measures the same wrapped labels the desktop font will paint. */
        MeasuredLabels Measure(const CaptionTimeline &timeline, const LocalizationSnapshot &locale, const LabelStyle &style) {
            MeasuredLabels labels;
            for (const auto &active : timeline.Cues()) {
                if (!VisibleDialogue(active, timeline.Tick()))
                    continue;
                const auto text = ResolveText((*active.catalog)[active.cue.text], locale);
                if (text.empty())
                    continue;
                const auto measured = style.font.CalcTextSizeA(style.size, FLT_MAX, style.wrap, text.data(), text.data() + text.size());
                const ImVec2 extent{measured.x + 2.0F * style.padding.x, measured.y + 2.0F * style.padding.y};
                if (!FitsLogical(extent))
                    continue;
                labels.cues[labels.count] = {active.cue.id, LogicalExtent(extent), {}};
                labels.text[labels.count++] = text;
            }
            return labels;
        }

        /** @brief Paints the accepted placements using captured colors and the editor's font. */
        void Paint(ImDrawList &draw, ImVec2 origin, const LabelStyle &style, const CaptionAppearance &appearance,
                   const CaptionLayout &layout, const MeasuredLabels &labels) {
            const auto &text = appearance.textColor;
            const auto &background = appearance.backgroundColor;
            const auto foreground = ImGui::ColorConvertFloat4ToU32({text[0], text[1], text[2], text[3]});
            const auto backdrop = ImGui::ColorConvertFloat4ToU32({background[0], background[1], background[2],
                                                                 background[3] * appearance.backgroundOpacity});
            const auto measured = std::span{labels.cues}.first(labels.count);
            for (const auto &caption : std::span{layout.placements}.first(layout.count)) {
                const auto label = std::ranges::find(measured, caption.cue, &CaptionMeasuredCue::cue);
                if (label == measured.end())
                    continue;
                const auto content = labels.text[static_cast<std::size_t>(label - measured.begin())];
                const ImVec2 topLeft{origin.x + caption.bounds.origin.x / LogicalUnits, origin.y + caption.bounds.origin.y / LogicalUnits};
                const ImVec2 bottomRight{topLeft.x + caption.bounds.extent.width / LogicalUnits,
                                         topLeft.y + caption.bounds.extent.height / LogicalUnits};
                draw.AddRectFilled(topLeft, bottomRight, backdrop, ImGui::GetStyle().FrameRounding);
                const ImVec4 clip{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
                draw.AddText(&style.font, style.size, {topLeft.x + style.padding.x, topLeft.y + style.padding.y}, foreground,
                             content.data(), content.data() + content.size(), style.wrap, &clip);
            }
        }
    }  // namespace

    /** @copydoc DrawViewportPlayCaptions */
    void DrawViewportPlayCaptions(ImDrawList &draw, const ImVec2 &origin, const ImVec2 &extent, const Theme::Fonts &fonts,
                                  const EditorPlayCaptionPreview &preview) {
        if (preview.timeline.Cues().empty() || !FitsLogical(extent))
            return;
        const auto settings = CaptionSettingsSnapshot::Capture(preview.configuration.Snapshot());
        if (settings.HasError() || !settings.Value().Appearance().enabled)
            return;
        const auto &appearance = settings.Value().Appearance();
        const auto padding = ImGui::GetStyle().FramePadding;
        const float wrap = extent.x * (1.0F - 2.0F * appearance.safeAreaMarginPercent / 100.0F) - 2.0F * padding.x;
        if (wrap <= 0)
            return;
        const LabelStyle style{*(fonts.sans ? fonts.sans : ImGui::GetFont()),
                               std::max(Theme::TextPx::Caption(), Theme::TextPx::Body() * appearance.fontScale), wrap, padding};
        const auto locale = preview.localization.Snapshot();
        const auto labels = Measure(preview.timeline, locale, style);
        const auto arranged = ArrangeCaptions({{}, LogicalExtent(extent)}, appearance, std::span{labels.cues}.first(labels.count));
        if (arranged.HasValue())
            Paint(draw, origin, style, appearance, arranged.Value(), labels);
    }
}  // namespace Horo::Editor
