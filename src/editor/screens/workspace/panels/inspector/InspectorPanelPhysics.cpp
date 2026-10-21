#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"
#include "editor/screens/workspace/panels/inspector/PhysicsInspectorWidgets.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

namespace Horo::Editor {
    namespace {
        using namespace PhysicsInspectorWidgets;

        /** @brief Draw stable contributor data without allocating label strings or using display names as control IDs. */
        void DrawColliderChild(const ColliderPresentation &child, const EditorWorkspaceViewModel &view,
                               EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
            const auto owner = std::ranges::find(view.objects, child.target.object, &SceneObject::id);
            const std::string_view name = owner == view.objects.end() ? std::string_view{"?"} : std::string_view{owner->name};
            std::array<char, MaximumSceneObjectNameBytes + 32> label{};
            const auto text = std::format_to_n(label.data(), label.size() - 1, "{} / {}", name, child.target.component.value);
            label[std::min(label.size() - 1, static_cast<std::size_t>(text.size))] = '\0';
            Ui::FieldLabel(label.data(), context.theme.fonts);
            if (child.diagnostic)
                Ui::Hint(Label(context, "workspace.physics.gizmo_unsupported"), context.theme.fonts);
            std::array<char, 48> identity{};
            static_cast<void>(
                std::format_to_n(identity.data(), identity.size() - 1, "{}:{}", child.target.object.value, child.target.component.value));
            ImGui::PushID(identity.data());
            if (Action(context, "workspace.physics.edit_child", child.editable && child.visible)) {
                command.command = EditorWorkspaceViewCommand::SelectPhysicsColliders;
                auto selection =
                    ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper ? view.selectedColliders : std::vector<ColliderEditTarget>{};
                if (const auto selected = std::ranges::find(selection, child.target); selected == selection.end())
                    selection.push_back(child.target);
                else
                    selection.erase(selected);
                command.colliderPayload =
                    ColliderViewportRequest{view.physicsContext, view.colliderSelectionRevision, std::move(selection), {}};
            }
            ImGui::PopID();
        }

        /** @brief Build a compound selection only after its action has been activated. */
        void SelectCompoundChildren(const Runtime::PhysicsBodyReference body, const EditorWorkspaceViewModel &view,
                                    EditorWorkspaceViewCommandData &command, const std::size_t editableCount) {
            std::vector<ColliderEditTarget> children;
            children.reserve(editableCount);
            for (const auto &child : view.viewportColliders)
                if (child.body == body && child.editable && child.visible)
                    children.push_back(child.target);
            command.command = EditorWorkspaceViewCommand::SelectPhysicsColliders;
            command.colliderPayload = ColliderViewportRequest{view.physicsContext, view.colliderSelectionRevision, std::move(children), {}};
        }

        /** @brief Select an explicit body's real contributors; allocate a selection batch only for an actual action. */
        void DrawColliderHierarchy(const SceneObject &object, const EditorWorkspaceViewModel &view, EditorWorkspaceViewCommandData &command,
                                   const EditorGuiContext &context) {
            if (!object.components.rigidBody)
                return;
            const Runtime::PhysicsBodyReference body{{object.id.value}, object.components.rigidBody->body};
            Ui::Card card({.id = "physics_compound_children"});
            card.DrawTitleBar(
                {.id = "title", .title = Label(context, "workspace.physics.compound_children"), .fonts = context.theme.fonts});
            if (card.BeginBody()) {
                std::size_t editableCount = 0;
                for (const auto &child : view.viewportColliders) {
                    if (child.body != body)
                        continue;
                    DrawColliderChild(child, view, command, context);
                    if (child.editable && child.visible)
                        ++editableCount;
                }
                if (Action(context, "workspace.physics.edit_compound", editableCount != 0))
                    SelectCompoundChildren(body, view, command, editableCount);
            }
            card.Finish();
        }

        /** @brief Draw a staged body's portable fields; removal remains part of the same uncommitted gesture. */
        void DrawBody(PhysicsInspectorSession &session, const EditorGuiContext &context) {
            if (!session.Draft().body)
                return;
            Ui::Card card({.id = "physics_body"});
            card.DrawTitleBar({.id = "title", .title = Label(context, "workspace.physics.body"), .fonts = context.theme.fonts});
            if (card.BeginBody()) {
                session.Draft().dirty = Body(context, *session.Draft().body) || session.Draft().dirty;
                if (Action(context, "workspace.physics.remove_body")) {
                    session.Draft().body.reset();
                    session.Draft().dirty = true;
                }
            }
            card.Finish();
        }

        /** @brief Draw each collider in bounded authored order using stable index-local control scopes. */
        void DrawColliders(PhysicsInspectorSession &session, const EditorGuiContext &context) {
            for (std::size_t index = 0; index < session.Draft().colliders.size(); ++index) {
                ImGui::PushID(static_cast<int>(index));
                Ui::Card card({.id = "physics_collider"});
                card.DrawTitleBar({.id = "title", .title = Label(context, "workspace.physics.collider"), .fonts = context.theme.fonts});
                bool remove = false;
                if (card.BeginBody()) {
                    session.Draft().dirty = Collider(context, session.Draft().colliders[index]) || session.Draft().dirty;
                    remove = Action(context, "workspace.physics.remove_collider");
                }
                card.Finish();
                ImGui::PopID();
                if (remove) {
                    session.Draft().colliders.erase(session.Draft().colliders.begin() + static_cast<std::ptrdiff_t>(index));
                    session.Draft().dirty = true;
                    break;
                }
            }
        }

        /** @brief Keep each constraint's two frames and parameters together in one shared card. */
        void DrawConstraints(PhysicsInspectorSession &session, const EditorGuiContext &context) {
            for (std::size_t index = 0; index < session.Draft().constraints.size(); ++index) {
                ImGui::PushID(static_cast<int>(index));
                Ui::Card card({.id = "physics_constraint"});
                card.DrawTitleBar({.id = "title", .title = Label(context, "workspace.physics.constraint"), .fonts = context.theme.fonts});
                bool remove = false;
                if (card.BeginBody()) {
                    session.Draft().dirty = Constraint(context, session.Draft().constraints[index]) || session.Draft().dirty;
                    remove = Action(context, "workspace.physics.remove_constraint");
                }
                card.Finish();
                ImGui::PopID();
                if (remove) {
                    session.Draft().constraints.erase(session.Draft().constraints.begin() + static_cast<std::ptrdiff_t>(index));
                    session.Draft().dirty = true;
                    break;
                }
            }
        }

        /** @brief Stage additions without publishing incomplete body/collider or unresolved asset bindings. */
        void DrawAdditions(PhysicsInspectorSession &session, const EditorGuiContext &context) {
            if (!session.Draft().body && Action(context, "workspace.physics.add_body"))
                session.AddBody();
            if (Action(context, "workspace.physics.add_collider",
                       session.Draft().colliders.size() < Runtime::MaximumPhysicsCollidersPerBody))
                session.AddCollider();
            if (Action(context, "workspace.physics.add_constraint",
                       session.Draft().constraints.size() < Runtime::MaximumPhysicsConstraintsPerObject))
                session.AddConstraint();
        }

        /** @brief Cancellation wins over Apply in the same frame; requests remain detached until host admission. */
        void DrawActions(PhysicsInspectorSession &session, bool &inputError, const SceneObjectId object,
                         EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
            const bool apply =
                Action(context, "workspace.physics.apply", session.Draft().dirty && command.command == EditorWorkspaceViewCommand::None);
            if (const bool cancel =
                    Action(context, "workspace.physics.cancel", session.Draft().dirty) ||
                    (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false));
                cancel) {
                session.Cancel();
                inputError = false;
                return;
            }
            if (!apply)
                return;
            auto request = session.Request();
            inputError = request.HasError();
            if (request.HasValue()) {
                command.command = EditorWorkspaceViewCommand::UpdatePhysicsComponents;
                command.objectPayload = object;
                command.physicsPayload.emplace(std::move(request).Value());
            }
        }
    }  // namespace

    /** @copydoc InspectorPanel::DrawAddPhysicsComponents */
    void InspectorPanel::DrawAddPhysicsComponents(const SceneObject &object, const EditorWorkspaceViewModel &viewModel,
                                                  const EditorGuiContext &context) {
        using namespace PhysicsInspectorWidgets;
        const bool canEdit = viewModel.physicsContext.canEdit && !object.effectivelyLocked;
        const auto item = [&](const char *key, const char *id, bool available) {
            const auto label = std::string{Label(context, key)} + id;
            return Ui::ContextMenuItem(label.c_str(), nullptr, context.theme.fonts, Ui::ContextMenuItemTone::Normal, {},
                                       canEdit && available);
        };
        Ui::ContextMenuSeparator();
        if (!canEdit)
            Ui::Hint(Label(context, "workspace.physics.unavailable"), context.theme.fonts);
        if (item("workspace.physics.add_body", "###inspector_physics_body", !m_physicsSession.Draft().body))
            m_physicsSession.AddBody();
        if (item("workspace.physics.add_collider", "###inspector_physics_collider",
                 m_physicsSession.Draft().colliders.size() < Runtime::MaximumPhysicsCollidersPerBody))
            m_physicsSession.AddCollider();
        if (item("workspace.physics.add_constraint", "###inspector_physics_constraint",
                 m_physicsSession.Draft().constraints.size() < Runtime::MaximumPhysicsConstraintsPerObject))
            m_physicsSession.AddConstraint();
    }

    /** @copydoc InspectorPanel::DrawPhysicsInspector */
    void InspectorPanel::DrawPhysicsInspector(const SceneObject &object, const EditorWorkspaceViewModel &viewModel,
                                              EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        using namespace PhysicsInspectorWidgets;
        if (m_physicsSession.Begin(object, viewModel.physicsContext, viewModel.physicsSelectionRevision))
            m_physicsInputError = false;
        // Absent components use the normal add-component route. A dirty empty draft still needs Apply/Cancel for removal.
        if (!m_physicsSession.Draft().body && m_physicsSession.Draft().colliders.empty() && m_physicsSession.Draft().constraints.empty() &&
            !m_physicsSession.Draft().dirty)
            return;
        Ui::FieldLabel(Label(context, "workspace.physics.title"), context.theme.fonts);
        Ui::Hint(Label(context, "workspace.physics.authoring_only"), context.theme.fonts);
        Stable(context, "workspace.physics.object_id", object.id.value);
        const bool canEdit = viewModel.physicsContext.canEdit && !object.effectivelyLocked;
        if (!canEdit)
            Ui::Hint(Label(context, "workspace.physics.unavailable"), context.theme.fonts);
        ImGui::BeginDisabled(!canEdit);
        DrawColliderHierarchy(object, viewModel, command, context);
        DrawBody(m_physicsSession, context);
        DrawColliders(m_physicsSession, context);
        DrawConstraints(m_physicsSession, context);
        DrawAdditions(m_physicsSession, context);
        DrawActions(m_physicsSession, m_physicsInputError, object.id, command, context);
        ImGui::EndDisabled();
        if (m_physicsInputError)
            Ui::ErrorText(Label(context, "workspace.physics.invalid_identity"), context.theme.fonts);
        if (!viewModel.physicsEditError.empty()) {
            Ui::ErrorText(Label(context, "workspace.physics.rejected"), context.theme.fonts);
            Ui::Hint(viewModel.physicsEditError.c_str(), context.theme.fonts);
        }
    }
}  // namespace Horo::Editor
