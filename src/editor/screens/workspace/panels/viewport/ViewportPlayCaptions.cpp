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

        /** @brief Reads only current cues accepted by the timeline and the captured display preferences. */
        bool Visible(const CaptionActiveCue &active, std::uint64_t tick, const CaptionAppearance &appearance) noexcept {
            return active.cue.startTick <= tick && (active.cue.kind == CaptionCueKind::Dialogue || appearance.showSoundDescriptions);
        }

        /** @brief Resolves an optional speaker from the same pinned catalog and locale as the caption. */
        std::string_view ResolveSpeaker(const CaptionActiveCue &active, const LocalizationSnapshot &locale, bool show) {
            if (!show || !active.cue.speaker)
                return {};
            return ResolveText((*active.catalog)[active.cue.speaker - 1], locale);
        }

        struct LabelStyle final {
            ImFont &font;
            float size;
            float wrap;
            ImVec2 padding;
        };

        struct MeasuredLabels final {
            struct Label final {
                std::string_view text;
                std::string_view speaker;
                float speakerHeight{};
            };

            std::array<CaptionMeasuredCue, MaximumCaptionCues> cues{};
            std::array<Label, MaximumCaptionCues> labels{};
            std::size_t count{};
        };

        /** @brief Measures one wrapped part with the same font parameters used for painting. */
        ImVec2 MeasureText(const LabelStyle &style, std::string_view text) {
            return text.empty() ? ImVec2{}
                                : style.font.CalcTextSizeA(style.size, FLT_MAX, style.wrap, text.data(), text.data() + text.size());
        }

        /** @brief Measures the same wrapped labels the desktop font will paint. */
        MeasuredLabels Measure(const CaptionTimeline &timeline, const LocalizationSnapshot &locale, const LabelStyle &style,
                               const CaptionAppearance &appearance) {
            MeasuredLabels labels;
            for (const auto &active : timeline.Cues()) {
                if (!Visible(active, timeline.Tick(), appearance))
                    continue;
                const auto text = ResolveText((*active.catalog)[active.cue.text], locale);
                if (text.empty())
                    continue;
                const auto speaker = ResolveSpeaker(active, locale, appearance.showSpeaker);
                const auto body = MeasureText(style, text);
                const auto name = MeasureText(style, speaker);
                const ImVec2 extent{std::max(body.x, name.x) + 2.0F * style.padding.x, body.y + name.y + 2.0F * style.padding.y};
                if (!FitsLogical(extent))
                    continue;
                labels.cues[labels.count] = {active.cue.id, LogicalExtent(extent), {}};
                labels.labels[labels.count++] = {text, speaker, name.y};
            }
            return labels;
        }

        /** @brief Draws one measured part using the shared placement clip. */
        void PaintText(ImDrawList &draw, const LabelStyle &style, ImVec2 origin, ImU32 foreground, const ImVec4 &clip,
                       std::string_view text) {
            if (!text.empty())
                draw.AddText(&style.font, style.size, origin, foreground, text.data(), text.data() + text.size(), style.wrap, &clip);
        }

        /** @brief Paints the accepted placements using captured colors and the editor's font. */
        void Paint(ImDrawList &draw, ImVec2 origin, const LabelStyle &style, const CaptionAppearance &appearance,
                   const CaptionLayout &layout, const MeasuredLabels &labels) {
            const auto &text = appearance.textColor;
            const auto &background = appearance.backgroundColor;
            const auto foreground = ImGui::ColorConvertFloat4ToU32({text[0], text[1], text[2], text[3]});
            const auto backdrop =
                ImGui::ColorConvertFloat4ToU32({background[0], background[1], background[2], background[3] * appearance.backgroundOpacity});
            const auto measured = std::span{labels.cues}.first(labels.count);
            for (const auto &caption : std::span{layout.placements}.first(layout.count)) {
                const auto label = std::ranges::find(measured, caption.cue, &CaptionMeasuredCue::cue);
                if (label == measured.end())
                    continue;
                const auto &content = labels.labels[static_cast<std::size_t>(label - measured.begin())];
                const ImVec2 topLeft{origin.x + caption.bounds.origin.x / LogicalUnits, origin.y + caption.bounds.origin.y / LogicalUnits};
                const ImVec2 bottomRight{topLeft.x + caption.bounds.extent.width / LogicalUnits,
                                         topLeft.y + caption.bounds.extent.height / LogicalUnits};
                draw.AddRectFilled(topLeft, bottomRight, backdrop, ImGui::GetStyle().FrameRounding);
                const ImVec4 clip{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
                const ImVec2 textOrigin{topLeft.x + style.padding.x, topLeft.y + style.padding.y};
                PaintText(draw, style, textOrigin, foreground, clip, content.speaker);
                PaintText(draw, style, {textOrigin.x, textOrigin.y + content.speakerHeight}, foreground, clip, content.text);
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
        const auto labels = Measure(preview.timeline, locale, style, appearance);
        const auto arranged = ArrangeCaptions({{}, LogicalExtent(extent)}, appearance, std::span{labels.cues}.first(labels.count));
        if (arranged.HasValue())
            Paint(draw, origin, style, appearance, arranged.Value(), labels);
    }
}  // namespace Horo::Editor
