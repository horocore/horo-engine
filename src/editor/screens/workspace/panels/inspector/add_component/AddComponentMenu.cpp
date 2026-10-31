#include "AddComponentMenu.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/Paths.h"

#include <type_traits>

namespace Horo::Editor {
    /** @copydoc DrawAddComponentMenu */
    std::optional<AddComponentAction> DrawAddComponentMenu(const AddComponentRegistry &registry, const AddComponentContext &state,
                                                           const EditorGuiContext &context) {
        std::optional<AddComponentAction> selected;
        for (const auto &category : AddComponentRegistry::Categories()) {
            const auto entries = registry.Entries(category.id);
            if (entries.empty())
                continue;
            const auto &label = context.localization.Get("editor", category.labelKey);
            std::string categoryLabel = label + "###";
            categoryLabel.append(category.labelKey);
            if (!Ui::BeginContextSubmenu(categoryLabel.c_str(), context.theme.fonts))
                continue;
            bool drawn = false;
            for (const auto &entry : entries) {
                if (drawn && entry.separatorBefore)
                    Ui::ContextMenuSeparator();
                const auto &text = entry.localized ? context.localization.Get("editor", entry.label) : entry.label;
                const std::string itemLabel = text + "###" + entry.id;
                if (Ui::ContextMenuItem(itemLabel.c_str(), nullptr, context.theme.fonts, Ui::ContextMenuItemTone::Normal, {},
                                        registry.IsAvailable(entry, state)))
                    selected = entry.action;
                drawn = true;
            }
            Ui::EndContextSubmenu();
        }
        return selected;
    }

    /** @copydoc ApplyAddComponentAction */
    void ApplyAddComponentAction(const AddComponentAction &action, const SceneObject &object, const std::string_view projectRoot,
                                 PhysicsInspectorSession &physics, EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::None)
            return;
        std::visit([&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ComponentType>) {
                command.command = EditorWorkspaceViewCommand::AddComponentToObject;
                command.objectPayload = object.id;
                command.componentTypePayload = value;
            } else if constexpr (std::is_same_v<T, AddPhysicsComponent>) {
                switch (value) {
                    case AddPhysicsComponent::Body:
                        physics.AddBody();
                        break;
                    case AddPhysicsComponent::BoxCollider:
                        physics.AddCollider();
                        break;
                    case AddPhysicsComponent::TriggerCollider:
                        physics.AddCollider(true);
                        break;
                    case AddPhysicsComponent::Joint:
                        physics.AddConstraint();
                        break;
                }
            } else if constexpr (std::is_same_v<T, AddComponentWorkflow>) {
                command.command = EditorWorkspaceViewCommand::CreateLuaBehavior;
                command.stringPayload = ProjectLayout::ScriptsRoot(std::filesystem::path{projectRoot}).string();
            } else {
                command.command = EditorWorkspaceViewCommand::AttachBehaviorToObject;
                command.objectPayload = object.id;
                command.behaviorTypePayload = value;
            }
        }, action);
    }
}  // namespace Horo::Editor
