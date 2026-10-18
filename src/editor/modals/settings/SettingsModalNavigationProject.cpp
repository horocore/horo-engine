#include "NavigationSettingsDraft.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <imgui.h>

namespace Horo::Editor {
    namespace {
        namespace Nav = Horo::Navigation;

        /** @brief Resolves one localized navigation settings label. */
        std::string Label(const EditorGuiContext &ctx, const std::string_view key) {
            return ctx.localization.Get("editor", key);
        }

        /** @brief Parses a complete nonzero stable identity; malformed text never becomes a default. */
        std::optional<std::uint64_t> Number(const std::string_view text) {
            std::uint64_t value{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size() ? std::optional{value}
                                                                                                        : std::nullopt;
        }

        /** @brief Shared labelled bounded text control. */
        void TextField(const std::string_view key, std::string &text, const EditorGuiContext &ctx) {
            const auto label = Label(ctx, key);
            Ui::SettingRow(label.c_str(), "", ctx.theme.fonts, [&label, &text, &ctx] {
                static_cast<void>(Ui::InputTextControl(label.c_str(), text, 256, ctx.theme.fonts));
            });
        }

        /** @brief Labels a detached finite scalar; the owning source validator remains the publication gate. */
        void FloatField(const std::string_view key, float &value, const EditorGuiContext &ctx) {
            const auto label = Label(ctx, key);
            Ui::SettingRow(label.c_str(), "", ctx.theme.fonts, [&label, &value, &ctx] {
                Ui::InputFloatControl(label.c_str(), &value, ctx.theme.fonts);
            });
        }

        /** @brief Captures one displayed typed gesture before authoring can change. */
        void Preview(NavigationDefinitionEditorState &state, const Assets::AssetId asset, std::vector<NavigationDefinitionEdit> edits) {
            auto result = state.session.Resolve()->Preview(asset, edits);
            if (result.HasError()) {
                state.status = result.ErrorValue().message;
                return;
            }
            state.draft->review = std::make_shared<NavigationSettingsReview>(std::move(result).Value());
            state.status.clear();
        }

        /** @brief Forms one create/update or explicit identity-rename gesture from typed controls. */
        void PutDraft(NavigationDefinitionEditorState &state, const NavigationSettingsDraft &draft,
                      const NavigationDefinitionSnapshot &definition, const bool rename) {
            const auto id = Number(rename ? draft.replacement : draft.identity);
            const auto source = Number(draft.sourceIdentity);
            const auto old = Number(draft.identity);
            const auto flags = Number(draft.flags);
            const auto excluded = Number(draft.excludedFlags);
            if (!id.has_value() || !*id || !old.has_value() || !*old || !flags.has_value() || !excluded.has_value() ||
                (draft.catalog != 0 && (!source.has_value() || !*source))) {
                state.status = MakeError(Nav::NavigationSettingsError(Nav::NavigationSettingsFailure::Invalid)).message;
                return;
            }
            std::vector<NavigationDefinitionEdit> edits;
            if (draft.catalog == 0) {
                const auto profile = Nav::NavigationAgentProfileId::Create(*id).Value();
                edits.emplace_back(PutNavigationProfile{{profile, draft.name, draft.geometry}});
                if (rename)
                    edits.emplace_back(RemoveNavigationProfile{Nav::NavigationAgentProfileId::Create(*old).Value(), profile});
            } else if (draft.catalog == 1) {
                const auto area = Nav::NavigationAreaId::Create(*id).Value();
                edits.emplace_back(PutNavigationArea{
                    {area,
                     {Nav::NavigationDescriptorSourceKind::Project, Nav::NavigationDescriptorSourceId::Create(*source).Value()},
                     draft.traversalCost,
                     {*flags}}});
                if (rename)
                    edits.emplace_back(RemoveNavigationArea{Nav::NavigationAreaId::Create(*old).Value(), area});
            } else {
                const auto filter = Nav::NavigationFilterId::Create(*id).Value();
                edits.emplace_back(PutNavigationFilter{
                    {filter,
                     {Nav::NavigationDescriptorSourceKind::Project, Nav::NavigationDescriptorSourceId::Create(*source).Value()},
                     {*flags},
                     {*excluded},
                     draft.overrides}});
                if (rename)
                    edits.emplace_back(RemoveNavigationFilter{Nav::NavigationFilterId::Create(*old).Value(), filter});
            }
            Preview(state, definition.asset, std::move(edits));
        }

        /** @brief Captures deletion and optional surviving replacement without mutating an owner. */
        void DeleteDraft(NavigationDefinitionEditorState &state, const NavigationSettingsDraft &draft,
                         const NavigationDefinitionSnapshot &definition) {
            const auto id = Number(draft.identity);
            const auto replacement = draft.replacement.empty() ? std::optional<std::uint64_t>{0} : Number(draft.replacement);
            if (!id.has_value() || !*id || !replacement.has_value()) {
                state.status = MakeError(Nav::NavigationSettingsError(Nav::NavigationSettingsFailure::Invalid)).message;
                return;
            }
            std::vector<NavigationDefinitionEdit> edits;
            if (draft.catalog == 0)
                edits.emplace_back(RemoveNavigationProfile{Nav::NavigationAgentProfileId::Create(*id).Value(),
                                                           *replacement
                                                               ? std::optional{Nav::NavigationAgentProfileId::Create(*replacement).Value()}
                                                               : std::nullopt});
            else if (draft.catalog == 1)
                edits.emplace_back(
                    RemoveNavigationArea{Nav::NavigationAreaId::Create(*id).Value(),
                                         *replacement ? std::optional{Nav::NavigationAreaId::Create(*replacement).Value()} : std::nullopt});
            else
                edits.emplace_back(RemoveNavigationFilter{Nav::NavigationFilterId::Create(*id).Value(),
                                                          *replacement
                                                              ? std::optional{Nav::NavigationFilterId::Create(*replacement).Value()}
                                                              : std::nullopt});
            Preview(state, definition.asset, std::move(edits));
        }

        /** @brief Displays exact catalog data; selecting an entry copies a draft rather than lending mutable source. */
        void Catalog(NavigationSettingsDraft &draft, const NavigationDefinitionSnapshot &snapshot, const EditorGuiContext &ctx) {
            const auto profiles = Label(ctx, "settings.navigation.profiles");
            const auto areas = Label(ctx, "settings.navigation.areas");
            const auto filters = Label(ctx, "settings.navigation.filters");
            if (const std::array names{profiles.c_str(), areas.c_str(), filters.c_str()};
                Ui::ComboControl("##navigation-catalog", &draft.catalog, names.data(), static_cast<int>(names.size()), ctx.theme.fonts))
                draft.review.reset();
            const auto choose = [&draft](const auto &value) {
                draft.identity = std::to_string(value.id.Value());
                draft.replacement.clear();
                draft.review.reset();
            };
            if (draft.catalog == 0)
                for (const auto &value : snapshot.definition->Profiles()) {
                    const auto text = std::format("{} [{}]", value.displayName, value.id.Value());
                    if (Ui::Button({.label = text.c_str(), .variant = Ui::ButtonVariant::Secondary})) {
                        choose(value);
                        draft.name = value.displayName;
                        draft.geometry = value.buildGeometry;
                    }
                }
            if (draft.catalog == 1)
                for (const auto &value : snapshot.definition->Registry().Areas()) {
                    const auto text = std::to_string(value.id.Value());
                    if (Ui::Button({.label = text.c_str(), .variant = Ui::ButtonVariant::Secondary})) {
                        choose(value);
                        draft.sourceIdentity = std::to_string(value.source.id.Value());
                        draft.traversalCost = value.traversalCost;
                        draft.flags = std::to_string(value.flags.bits);
                    }
                }
            if (draft.catalog == 2)
                for (const auto &value : snapshot.definition->Registry().Filters()) {
                    const auto text = std::to_string(value.id.Value());
                    if (Ui::Button({.label = text.c_str(), .variant = Ui::ButtonVariant::Secondary})) {
                        choose(value);
                        draft.sourceIdentity = std::to_string(value.source.id.Value());
                        draft.flags = std::to_string(value.includedFlags.bits);
                        draft.excludedFlags = std::to_string(value.excludedFlags.bits);
                        draft.overrides = value.costOverrides;
                    }
                }
        }

        /** @brief Draws exact per-occurrence confirmation; every row is visible before Commit is enabled. */
        void Review(NavigationDefinitionEditorState &state, NavigationSettingsDraft &draft, const EditorGuiContext &ctx) {
            if (!draft.review)
                return;
            const auto title = Label(ctx, "settings.navigation.references");
            Ui::SectionTitle(title.c_str(), ctx.theme.fonts);
            for (const auto &impact : draft.review->Impacts()) {
                const auto field = Label(ctx, std::format("settings.navigation.reference.{}", static_cast<unsigned>(impact.field)));
                const auto text = std::format("{} / {} / {} / {} -> {}", impact.asset.ToString(), impact.object, field, impact.identity,
                                              impact.replacement.has_value() ? std::to_string(*impact.replacement)
                                                                             : Label(ctx, "settings.navigation.unrepaired"));
                ImGui::TextWrapped("%s", text.c_str());
            }
            for (const auto &owner : draft.review->UnsupportedOwners()) {
                const auto kind =
                    Label(ctx, owner.kind == NavigationUnsupportedOwnerKind::PrefabSource ? "settings.navigation.unsupported_prefab"
                                                                                          : "settings.navigation.unsupported_instances");
                ImGui::TextWrapped("%s: %s", kind.c_str(), owner.asset.ToString().c_str());
            }
            const auto confirm = Label(ctx, "settings.navigation.confirm");
            if (Ui::Button({.label = confirm.c_str(), .enabled = draft.review->CanCommit()})) {
                auto result = state.session.Resolve()->Commit(*draft.review);
                state.status = result.HasError() ? result.ErrorValue().message : Label(ctx, "settings.navigation.committed");
                draft.review.reset();
            }
        }

        /** @brief Presents all seven geometry values without nesting control lifetime inside catalog dispatch. */
        void GeometryFields(NavigationSettingsDraft &draft, const EditorGuiContext &ctx) {
            TextField("settings.navigation.name", draft.name, ctx);
            const std::array fields{&draft.geometry.radiusMeters,           &draft.geometry.heightMeters,
                                    &draft.geometry.maxSlopeDegrees,        &draft.geometry.stepHeightMeters,
                                    &draft.geometry.cellSizeMeters,         &draft.geometry.cellHeightMeters,
                                    &draft.geometry.minimumRegionSizeMeters};
            for (std::size_t index = 0; index < fields.size(); ++index) {
                const auto label = Label(ctx, std::format("settings.navigation.geometry.{}", index));
                Ui::SettingRow(label.c_str(), "", ctx.theme.fonts, [&label, &fields, index, &ctx] {
                    Ui::InputFloatControl(label.c_str(), fields[index], ctx.theme.fonts);
                });
            }
        }

        /** @brief Edits a bounded list of exact filter area-cost overrides; invalid IDs remain detached. */
        void FilterFields(NavigationSettingsDraft &draft, const EditorGuiContext &ctx) {
            TextField("settings.navigation.excluded", draft.excludedFlags, ctx);
            TextField("settings.navigation.override_area", draft.overrideArea, ctx);
            FloatField("settings.navigation.override_cost", draft.overrideCost, ctx);
            if (Ui::Button({.label = Label(ctx, "settings.navigation.add_override").c_str()})) {
                if (const auto area = Number(draft.overrideArea); area.has_value() && *area && draft.overrides.size() < 256)
                    draft.overrides.emplace_back(Nav::NavigationAreaId::Create(*area).Value(), draft.overrideCost);
            }
            for (const auto &cost : draft.overrides)
                ImGui::Text("%llu : %.3f", static_cast<unsigned long long>(cost.area.Value()), cost.traversalCost);
            if (Ui::Button({.label = Label(ctx, "settings.navigation.clear_overrides").c_str()}))
                draft.overrides.clear();
        }

        /** @brief Edits detached typed catalog fields; publication remains a separate reviewed gesture. */
        void Fields(NavigationSettingsDraft &draft, const EditorGuiContext &ctx) {
            TextField("settings.navigation.identity", draft.identity, ctx);
            TextField("settings.navigation.replacement", draft.replacement, ctx);
            if (draft.catalog == 0) {
                GeometryFields(draft, ctx);
                return;
            }
            TextField("settings.navigation.source_identity", draft.sourceIdentity, ctx);
            TextField("settings.navigation.flags", draft.flags, ctx);
            if (draft.catalog == 1)
                FloatField("settings.navigation.area_cost", draft.traversalCost, ctx);
            else
                FilterFields(draft, ctx);
        }

        /** @brief Retains the final actual per-file error, including errors following an earlier durable save. */
        void SaveDocuments(NavigationDefinitionEditorState &state, const EditorGuiContext &ctx) {
            auto result = state.session.Resolve()->SaveDocuments();
            state.status = result.HasError() ? result.ErrorValue().message : Label(ctx, "settings.navigation.saved");
            if (result.HasError())
                return;
            for (const auto &outcome : result.Value())
                if (outcome.error)
                    state.status = outcome.error->message;
        }

        /** @brief Routes complete-group history and explicit per-file source save through the owning session. */
        void HistoryActions(NavigationDefinitionEditorState &state, const EditorGuiContext &ctx) {
            if (Ui::Button({.label = Label(ctx, "settings.navigation.undo").c_str()})) {
                auto result = state.session.Resolve()->Replay(false);
                if (result.HasError())
                    state.status = result.ErrorValue().message;
            }
            if (Ui::Button({.label = Label(ctx, "settings.navigation.redo").c_str()})) {
                auto result = state.session.Resolve()->Replay(true);
                if (result.HasError())
                    state.status = result.ErrorValue().message;
            }
            if (Ui::Button({.label = Label(ctx, "settings.navigation.save_documents").c_str()}))
                SaveDocuments(state, ctx);
        }
    }  // namespace

    /** @copydoc DrawNavigationDefinitionEditor */
    void DrawNavigationDefinitionEditor(NavigationDefinitionEditorState &state, const EditorGuiContext &ctx, const Assets::AssetId asset) {
        if (!state.session.Resolve()) {
            ImGui::TextWrapped("%s", Label(ctx, "settings.navigation.unavailable").c_str());
            if (!state.status.empty())
                ImGui::TextWrapped("%s", state.status.c_str());
            return;
        }
        if (!state.draft)
            state.draft = std::make_shared<NavigationSettingsDraft>();
        auto &draft = *state.draft;
        auto definitions = state.session.Resolve()->Definitions();
        if (definitions.HasError()) {
            state.status = definitions.ErrorValue().message;
            return;
        }
        if (definitions.Value().empty()) {
            ImGui::TextWrapped("%s", Label(ctx, "settings.navigation.no_definition").c_str());
            return;
        }
        const auto selected = std::ranges::find(definitions.Value(), asset, &NavigationDefinitionSnapshot::asset);
        if (selected == definitions.Value().end()) {
            state.status = Label(ctx, "settings.navigation.unavailable");
            return;
        }
        const auto &definition = *selected;
        Catalog(draft, definition, ctx);
        Fields(draft, ctx);
        if (Ui::Button({.label = Label(ctx, "settings.navigation.put").c_str()}))
            PutDraft(state, draft, definition, false);
        if (Ui::Button({.label = Label(ctx, "settings.navigation.rename_identity").c_str(), .enabled = draft.catalog != 0}))
            PutDraft(state, draft, definition, true);
        if (Ui::Button({.label = Label(ctx, "settings.navigation.delete").c_str()}))
            DeleteDraft(state, draft, definition);
        Review(state, draft, ctx);
        HistoryActions(state, ctx);
        if (!state.status.empty())
            ImGui::TextWrapped("%s", state.status.c_str());
    }
}  // namespace Horo::Editor
