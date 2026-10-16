#include "SettingsModalCollisionInternal.h"

#include <algorithm>
#include <array>
#include <imgui.h>

namespace Horo::Editor::SettingsModalInternal {
    namespace {
        const char *Text(const EditorGuiContext &ctx, const char *key) {
            return ctx.localization.Get("editor", key).c_str();
        }

        template <typename Id> const char *Name(const std::vector<Application::CollisionDisplayMetadata<Id>> &items, const Id id) {
            const auto found = std::ranges::find(items, id, &Application::CollisionDisplayMetadata<Id>::id);
            return found == items.end() ? "" : found->name.c_str();
        }

        template <typename Response>
        void ResponseControl(const char *id, Response &response, SettingsCollisionDraft &draft, const EditorGuiContext &ctx) {
            const std::array<const char *, 3> labels{Text(ctx, "settings.collision.ignore"), Text(ctx, "settings.collision.overlap"),
                                                     Text(ctx, "settings.collision.block")};
            int selected = static_cast<int>(response);
            if (Ui::ComboControl(id, &selected, labels.data(), 3, ctx.theme.fonts)) {
                response = static_cast<Response>(selected);
                CollisionEdited(draft);
            }
        }

        void ProfileLayer(SettingsCollisionDraft &draft, Physics::CollisionProfileDefinition &profile, const EditorGuiContext &ctx) {
            const auto &layers = draft.input->layers;
            const auto found =
                std::ranges::find(layers, profile.layer, &Application::CollisionDisplayMetadata<Physics::CollisionLayerId>::id);
            int selected = found == layers.end() ? -1 : static_cast<int>(found - layers.begin());
            const Ui::ComboItemSource source{.label = [&layers](int index) {
                return layers[static_cast<std::size_t>(index)].name.c_str();
            }};
            Ui::SettingRow(Text(ctx, "settings.collision.layer"), nullptr, ctx.theme.fonts, [&] {
                if (Ui::ComboControl("##profile-layer", &selected, static_cast<int>(layers.size()), source, ctx.theme.fonts)) {
                    profile.layer = layers[static_cast<std::size_t>(selected)].id;
                    CollisionEdited(draft);
                }
            });
        }

        void ProfileResponses(SettingsCollisionDraft &draft, Physics::CollisionProfileDefinition &profile, const EditorGuiContext &ctx) {
            for (std::size_t index = 0; index < profile.queryResponses.size(); ++index) {
                auto &entry = profile.queryResponses[index];
                ImGui::PushID(static_cast<int>(index));
                Ui::SettingRow(Name(draft.input->channels, entry.channel), nullptr, ctx.theme.fonts, [&] {
                    ResponseControl("##query-response", entry.response, draft, ctx);
                });
                ImGui::PopID();
            }
        }
    }  // namespace

    void DrawCollisionMatrix(SettingsCollisionDraft &draft, const EditorGuiContext &ctx) {
        Ui::SettingGroup(Text(ctx, "settings.collision.matrix"), ctx.theme.fonts);
        for (std::size_t index = 0; index < draft.input->schema.pairs.size(); ++index) {
            auto &pair = draft.input->schema.pairs[index];
            ImGui::PushID(static_cast<int>(index));
            ImGui::TextWrapped("%s / %s", Name(draft.input->layers, pair.first), Name(draft.input->layers, pair.second));
            ResponseControl("##pair", pair.response, draft, ctx);
            ImGui::PopID();
        }
    }

    void DrawCollisionProfiles(SettingsCollisionDraft &draft, const EditorGuiContext &ctx) {
        if (static_cast<CollisionItemKind>(draft.kind) != CollisionItemKind::Profile || draft.selected < 0 ||
            static_cast<std::size_t>(draft.selected) >= draft.input->profiles.size())
            return;
        const auto id = draft.input->profiles[static_cast<std::size_t>(draft.selected)].id;
        auto found = std::ranges::find(draft.input->schema.profiles, id, &Physics::CollisionProfileDefinition::id);
        if (found == draft.input->schema.profiles.end())
            return;
        ProfileLayer(draft, *found, ctx);
        if (Ui::CheckboxControl(Text(ctx, "settings.collision.simulation"), &found->simulationEnabled, ctx.theme.fonts))
            CollisionEdited(draft);
        if (Ui::CheckboxControl(Text(ctx, "settings.collision.queries"), &found->queryEnabled, ctx.theme.fonts))
            CollisionEdited(draft);
        if (Ui::CheckboxControl(Text(ctx, "settings.collision.events"), &found->lifecycleEventsEnabled, ctx.theme.fonts))
            CollisionEdited(draft);
        bool isDefault = draft.input->schema.defaultProfile == id;
        if (Ui::CheckboxControl(Text(ctx, "settings.collision.default"), &isDefault, ctx.theme.fonts) && isDefault) {
            draft.input->schema.defaultProfile = id;
            CollisionEdited(draft);
        }
        ProfileResponses(draft, *found, ctx);
    }
}  // namespace Horo::Editor::SettingsModalInternal
