#include "SettingsModalCollisionInternal.h"

#include <algorithm>
#include <array>
#include <imgui.h>

namespace Horo::Editor::SettingsModalInternal {
    namespace {
        const char *Text(const EditorGuiContext &ctx, const char *key) {
            return ctx.localization.Get("editor", key).c_str();
        }

        template <typename Id>
        void DrawMetadata(SettingsCollisionDraft &draft, std::vector<Application::CollisionDisplayMetadata<Id>> &items,
                          const EditorGuiContext &ctx) {
            if (items.empty())
                return;
            draft.selected = std::clamp(draft.selected, 0, static_cast<int>(items.size()) - 1);
            const Ui::ComboItemSource source{.label = [&items](int index) {
                return items[static_cast<std::size_t>(index)].name.c_str();
            }};
            (void)Ui::ComboControl("##definition", &draft.selected, static_cast<int>(items.size()), source, ctx.theme.fonts);
            auto &item = items[static_cast<std::size_t>(draft.selected)];
            Ui::SettingRow(Text(ctx, "settings.collision.name"), nullptr, ctx.theme.fonts, [&] {
                if (Ui::InputTextControl("##name", item.name, 128, ctx.theme.fonts))
                    CollisionEdited(draft);
            });
            Ui::SettingRow(Text(ctx, "settings.collision.description"), nullptr, ctx.theme.fonts, [&] {
                if (Ui::InputTextControl("##description", item.description, 1024, ctx.theme.fonts))
                    CollisionEdited(draft);
            });
            // Typed identity presentation uses a bounded stack buffer, never ToString's per-frame allocation.
            std::array<char, 37> identity{};
            static constexpr char digits[] = "0123456789abcdef";
            std::size_t offset{};
            for (std::size_t byte = 0; byte < item.id.Bytes().size(); ++byte) {
                if (byte == 4 || byte == 6 || byte == 8 || byte == 10)
                    identity[offset++] = '-';
                identity[offset++] = digits[item.id.Bytes()[byte] >> 4];
                identity[offset++] = digits[item.id.Bytes()[byte] & 15];
            }
            ImGui::TextUnformatted(identity.data());
        }

        void DrawLayerFlags(SettingsCollisionDraft &draft, const EditorGuiContext &ctx) {
            const auto &items = draft.input->layers;
            if (draft.selected < 0 || static_cast<std::size_t>(draft.selected) >= items.size())
                return;
            const auto id = items[static_cast<std::size_t>(draft.selected)].id;
            auto found = std::ranges::find(draft.input->schema.layers, id, &Physics::CollisionLayerDefinition::id);
            if (found == draft.input->schema.layers.end())
                return;
            if (Ui::CheckboxControl(Text(ctx, "settings.collision.static"), &found->admitsStatic, ctx.theme.fonts))
                CollisionEdited(draft);
            if (Ui::CheckboxControl(Text(ctx, "settings.collision.kinematic"), &found->admitsKinematic, ctx.theme.fonts))
                CollisionEdited(draft);
            if (Ui::CheckboxControl(Text(ctx, "settings.collision.dynamic"), &found->admitsDynamic, ctx.theme.fonts))
                CollisionEdited(draft);
            if (Ui::CheckboxControl(Text(ctx, "settings.collision.overlap"), &found->admitsOverlap, ctx.theme.fonts))
                CollisionEdited(draft);
        }

        void DrawCreation(SettingsCollisionDraft &draft, CollisionProjectSettingsSession &owner, const EditorGuiContext &ctx) {
            Ui::SettingRow(Text(ctx, "settings.collision.new_name"), nullptr, ctx.theme.fonts, [&] {
                (void)Ui::InputTextControl("##new-name", draft.newName, 128, ctx.theme.fonts);
            });
            if (static_cast<CollisionItemKind>(draft.kind) == CollisionItemKind::Profile && !draft.input->layers.empty()) {
                const auto &layers = draft.input->layers;
                draft.initialLayer = std::clamp(draft.initialLayer, 0, static_cast<int>(layers.size()) - 1);
                const Ui::ComboItemSource source{.label = [&layers](int index) {
                    return layers[static_cast<std::size_t>(index)].name.c_str();
                }};
                Ui::SettingRow(Text(ctx, "settings.collision.layer"), nullptr, ctx.theme.fonts, [&] {
                    (void)Ui::ComboControl("##new-profile-layer", &draft.initialLayer, static_cast<int>(layers.size()), source,
                                           ctx.theme.fonts);
                });
            }
            const std::array<const char *, 3> responses{Text(ctx, "settings.collision.ignore"), Text(ctx, "settings.collision.overlap"),
                                                        Text(ctx, "settings.collision.block")};
            Ui::SettingRow(Text(ctx, "settings.collision.initial_response"), nullptr, ctx.theme.fonts, [&] {
                (void)Ui::ComboControl("##initial-response", &draft.initialResponse, responses.data(), 3, ctx.theme.fonts);
            });
            if (Ui::Button({.label = Text(ctx, "settings.collision.create"), .enabled = !draft.newName.empty()})) {
                const auto created = CreateCollisionItem(draft, owner);
                if (created.HasError())
                    draft.error = created.ErrorValue();
            }
            if (Ui::Button({.label = Text(ctx, "settings.collision.remove_candidate"), .variant = Ui::ButtonVariant::Secondary})) {
                const auto removed = RemoveCollisionItem(draft);
                if (removed.HasError())
                    draft.error = removed.ErrorValue();
                draft.selected = 0;
            }
        }
    }  // namespace

    void DrawCollisionDefinitions(SettingsCollisionDraft &draft, CollisionProjectSettingsSession &owner, const EditorGuiContext &ctx) {
        const std::array<const char *, 3> kinds{Text(ctx, "settings.collision.layers"), Text(ctx, "settings.collision.profiles"),
                                                Text(ctx, "settings.collision.channels")};
        if (Ui::ComboControl("##definition-kind", &draft.kind, kinds.data(), 3, ctx.theme.fonts))
            draft.selected = 0;
        switch (static_cast<CollisionItemKind>(draft.kind)) {
            case CollisionItemKind::Layer:
                DrawMetadata(draft, draft.input->layers, ctx);
                DrawLayerFlags(draft, ctx);
                break;
            case CollisionItemKind::Profile:
                DrawMetadata(draft, draft.input->profiles, ctx);
                break;
            case CollisionItemKind::Channel:
                DrawMetadata(draft, draft.input->channels, ctx);
                break;
        }
        DrawCreation(draft, owner, ctx);
    }
}  // namespace Horo::Editor::SettingsModalInternal
