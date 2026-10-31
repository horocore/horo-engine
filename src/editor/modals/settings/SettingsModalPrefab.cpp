#include "SettingsModalInternal.h"

#include <charconv>
#include <format>
#include <imgui.h>

namespace Horo::Editor::SettingsModalInternal {
    namespace {
        /** @brief Resolve a full catalog key through the existing localization service. */
        const std::string &Text(const EditorGuiContext &ctx, const std::string_view key) {
            return ctx.localization.Get("editor", key);
        }

        /** @brief Refresh numeric presentation only after an explicit owner command or external publication. */
        void Refresh(SettingsPrefabDraft &draft, const std::shared_ptr<const PrefabProjectSettingsView> &view) {
            draft.view = view;
            for (std::size_t index = 0; index < draft.text.size(); ++index) {
                auto &buffer = draft.text[index];
                const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size() - 1, view->values[index]);
                *result.ptr = '\0';
            }
        }

        /** @brief Cache formatted captions only when locale messages or immutable derived work change. */
        void Captions(SettingsPrefabDraft &draft, const EditorGuiContext &ctx) {
            const auto &ceiling = Text(ctx, "settings.prefab.ceiling");
            if (draft.ceilingPattern != ceiling) {
                draft.ceilingPattern = ceiling;
                for (const auto &field : Application::PrefabLimitFields())
                    draft.ceilings[static_cast<std::size_t>(field.field)] = std::vformat(ceiling, std::make_format_args(field.hardCeiling));
            }
            const auto &hard = Text(ctx, "settings.prefab.hard_safety");
            if (draft.hardPattern != hard) {
                draft.hardPattern = hard;
                const auto bytes = Prefab::PrefabHardLimits::SourceDocumentBytes;
                const auto depth = Prefab::PrefabHardLimits::SourceJsonDepth;
                draft.hardSafety = std::vformat(hard, std::make_format_args(bytes, depth));
            }
            const auto &profile = draft.view->validation.profile;
            const auto work = profile ? std::optional{profile->MaximumExpansionWorkItems()} : std::nullopt;
            if (work && draft.workValue != work)
                draft.work = std::to_string(*work);
            else if (!work && draft.work != Text(ctx, "settings.prefab.invalid"))
                draft.work = Text(ctx, "settings.prefab.invalid");
            draft.workValue = work;
        }

        /** @brief Keep original typed failures and localized summaries distinct from draft/accepted state. */
        void Command(SettingsPrefabDraft &draft, PrefabProjectSettingsSession &owner, const PrefabSettingsCommandKind kind) {
            auto outcome = owner.Execute({kind, draft.view->revision});
            if (outcome.HasError()) {
                draft.error = std::move(outcome).ErrorValue();
                draft.statusKey = "settings.prefab.error.command";
                return;
            }
            draft.error.reset();
            switch (kind) {
                case PrefabSettingsCommandKind::Save:
                    draft.statusKey = "settings.prefab.saved";
                    break;
                case PrefabSettingsCommandKind::Reload:
                    draft.statusKey = "settings.prefab.reloaded";
                    break;
                case PrefabSettingsCommandKind::Discard:
                    draft.statusKey = "settings.prefab.discarded";
                    break;
                default:
                    draft.statusKey = "settings.prefab.draft_updated";
                    break;
            }
            const auto current = owner.Snapshot();
            if (current.HasValue())
                Refresh(draft, current.Value());
        }

        /** @brief Select stacked layout from actual text and available bounds without changing control identity. */
        bool Stacked(const std::string &label, const EditorGuiContext &ctx) {
            const auto gap = DesignSystem::SpacingFor(Theme::GetActiveTokens(), DesignSystem::SpacingSize::Small);
            Theme::ScopedTextStyle style(ctx.theme.fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans);
            return ImGui::CalcTextSize(label.c_str()).x + Theme::Layout::ControlW + gap > ImGui::GetContentRegionAvail().x;
        }

        /** @brief Draw one stable field without copying payloads, parsing files or allocating temporary input buffers. */
        void Field(SettingsPrefabDraft &draft, PrefabProjectSettingsSession &owner, const Application::PrefabLimitFieldDescriptor &field,
                   const EditorGuiContext &ctx) {
            const auto index = static_cast<std::size_t>(field.field);
            const auto &label = Text(ctx, field.localizationKey);
            const auto &fonts = ctx.theme.fonts;
            const auto available = ImGui::GetContentRegionAvail().x;
            bool changed{};
            const auto control = [&] {
                auto &buffer = draft.text[index];
                changed = Ui::InputTextControl("##prefab-value", buffer.data(), buffer.size(), fonts,
                                               {.error = draft.view->validation.fields[index] != Application::PrefabPolicyFinding::None});
            };
            ImGui::PushID(static_cast<int>(field.field));
            Ui::SettingRow(label.c_str(), nullptr, fonts, control, "prefab-policy-row",
                           Stacked(label, ctx) ? Ui::SettingRowLayout::Stacked : Ui::SettingRowLayout::Columns);
            if (changed) {
                std::int64_t value{};
                const auto text = std::string_view{draft.text[index].data()};
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                const bool valid = !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
                auto outcome = owner.Execute({valid ? PrefabSettingsCommandKind::Edit : PrefabSettingsCommandKind::InvalidNumber,
                                              draft.view->revision, field.field, value});
                if (outcome.HasError()) {
                    draft.error = std::move(outcome).ErrorValue();
                    draft.statusKey = "settings.prefab.error.command";
                } else {
                    draft.error.reset();
                    draft.statusKey.clear();
                }
                const auto current = owner.Snapshot();
                if (current.HasValue())
                    draft.view = current.Value();
            }
            Ui::WrappedText(available, draft.ceilings[index], Theme::Dim(), fonts);
            const auto finding = draft.view->validation.fields[index];
            if (finding != Application::PrefabPolicyFinding::None) {
                const auto key = finding == Application::PrefabPolicyFinding::InvalidNumber     ? "settings.prefab.error.number"
                                 : finding == Application::PrefabPolicyFinding::ValueExceedsSet ? "settings.prefab.error.value_set"
                                                                                                : "settings.prefab.error.range";
                Ui::WrappedText(available, Text(ctx, key), Theme::Err(), fonts);
            }
            ImGui::PopID();
        }

        /** @brief Reuse shared full-width action controls with persistent stable IDs independent of locale. */
        void Action(SettingsPrefabDraft &draft, PrefabProjectSettingsSession &owner, const EditorGuiContext &ctx, const std::size_t index,
                    const char *key, const char *id, const PrefabSettingsCommandKind kind, const bool enabled) {
            const auto &text = Text(ctx, key);
            if (draft.actionText[index] != text) {
                draft.actionText[index] = text;
                draft.actionLabels[index] = text + id;
            }
            if (Ui::Button(
                    {.label = draft.actionLabels[index].c_str(), .enabled = enabled, .style = {.width = Ui::StyleWidth::FillAvailable}}))
                Command(draft, owner, kind);
        }
    }  // namespace

    /** @copydoc DrawPrefabSettings */
    void DrawPrefabSettings(SettingsState &state, const EditorGuiContext &ctx) {
        auto &draft = state.prefab;
        const auto owner = draft.owner.lock();
        if (!owner) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(ctx, "settings.prefab.unavailable"), Theme::Dim(), ctx.theme.fonts);
            return;
        }
        const auto current = owner->Snapshot();
        if (current.HasError()) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(ctx, "settings.prefab.unavailable"), Theme::Dim(), ctx.theme.fonts);
            return;
        }
        if (!draft.view || draft.view->revision != current.Value()->revision)
            Refresh(draft, current.Value());
        Captions(draft, ctx);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        Ui::SectionTitle(Text(ctx, "settings.prefab.title").c_str(), ctx.theme.fonts);
        ImGui::PopTextWrapPos();
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(ctx, "settings.prefab.description"), Theme::Dim(), ctx.theme.fonts);
        for (const auto &field : Application::PrefabLimitFields())
            Field(draft, *owner, field, ctx);
        Captions(draft, ctx);
        const auto &label = Text(ctx, "settings.prefab.derived_work");
        const auto &help = Text(ctx, "settings.prefab.derived_help");
        Ui::SettingRow(label.c_str(), help.c_str(), ctx.theme.fonts, [&] {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, draft.work, Theme::Text(), ctx.theme.fonts);
        }, "prefab-derived-work", Stacked(label, ctx) ? Ui::SettingRowLayout::Stacked : Ui::SettingRowLayout::Columns);
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, draft.hardSafety, Theme::Dim(), ctx.theme.fonts);
        if (!draft.view->validation.profile)
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(ctx, "settings.prefab.error.summary"), Theme::Err(), ctx.theme.fonts);
        if (draft.view->durabilityError)
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(ctx, "settings.prefab.error.durability"), Theme::Err(), ctx.theme.fonts);
        Action(draft, *owner, ctx, 0, "settings.prefab.reset", "###prefab-reset", PrefabSettingsCommandKind::ResetDefaults, true);
        Action(draft, *owner, ctx, 1, "settings.prefab.discard", "###prefab-discard", PrefabSettingsCommandKind::Discard,
               draft.view->dirty);
        Action(draft, *owner, ctx, 2, "settings.prefab.reload", "###prefab-reload", PrefabSettingsCommandKind::Reload, !draft.view->dirty);
        Action(draft, *owner, ctx, 3, "settings.prefab.save", "###prefab-save", PrefabSettingsCommandKind::Save,
               draft.view->dirty && draft.view->validation.profile.has_value());
        if (!draft.statusKey.empty())
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(ctx, draft.statusKey), Theme::Dim(), ctx.theme.fonts);
    }
}  // namespace Horo::Editor::SettingsModalInternal
