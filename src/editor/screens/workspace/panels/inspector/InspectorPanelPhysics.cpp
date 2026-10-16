#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"
#include "editor/screens/workspace/panels/inspector/PhysicsInspectorWidgets.h"

namespace Horo::Editor {
    namespace {
        using namespace PhysicsInspectorWidgets;

        /** @brief Draw a staged body's portable fields; removal remains part of the same uncommitted gesture. */
        void DrawBody(PhysicsInspectorSession &session, const EditorGuiContext &context) {
            if (!session.body)
                return;
            Ui::Card card({.id = "physics_body"});
            card.DrawTitleBar({.id = "title", .title = Label(context, "workspace.physics.body"), .fonts = context.theme.fonts});
            if (card.BeginBody()) {
                session.dirty = Body(context, *session.body) || session.dirty;
                if (Action(context, "workspace.physics.remove_body")) {
                    session.body.reset();
                    session.dirty = true;
                }
            }
            card.Finish();
        }

        /** @brief Draw each collider in bounded authored order using stable index-local control scopes. */
        void DrawColliders(PhysicsInspectorSession &session, const EditorGuiContext &context) {
            for (std::size_t index = 0; index < session.colliders.size(); ++index) {
                ImGui::PushID(static_cast<int>(index));
                Ui::Card card({.id = "physics_collider"});
                card.DrawTitleBar({.id = "title", .title = Label(context, "workspace.physics.collider"), .fonts = context.theme.fonts});
                bool remove = false;
                if (card.BeginBody()) {
                    session.dirty = Collider(context, session.colliders[index]) || session.dirty;
                    remove = Action(context, "workspace.physics.remove_collider");
                }
                card.Finish();
                ImGui::PopID();
                if (remove) {
                    session.colliders.erase(session.colliders.begin() + static_cast<std::ptrdiff_t>(index));
                    session.dirty = true;
                    break;
                }
            }
        }

        /** @brief Keep each constraint's two frames and parameters together in one shared card. */
        void DrawConstraints(PhysicsInspectorSession &session, const EditorGuiContext &context) {
            for (std::size_t index = 0; index < session.constraints.size(); ++index) {
                ImGui::PushID(static_cast<int>(index));
                Ui::Card card({.id = "physics_constraint"});
                card.DrawTitleBar({.id = "title", .title = Label(context, "workspace.physics.constraint"), .fonts = context.theme.fonts});
                bool remove = false;
                if (card.BeginBody()) {
                    session.dirty = Constraint(context, session.constraints[index]) || session.dirty;
                    remove = Action(context, "workspace.physics.remove_constraint");
                }
                card.Finish();
                ImGui::PopID();
                if (remove) {
                    session.constraints.erase(session.constraints.begin() + static_cast<std::ptrdiff_t>(index));
                    session.dirty = true;
                    break;
                }
            }
        }

        /** @brief Stage additions without publishing incomplete body/collider or unresolved asset bindings. */
        void DrawAdditions(PhysicsInspectorSession &session, const EditorGuiContext &context) {
            if (!session.body && Action(context, "workspace.physics.add_body"))
                session.AddBody();
            if (Action(context, "workspace.physics.add_collider", session.colliders.size() < Runtime::MaximumPhysicsCollidersPerBody))
                session.AddCollider();
            if (Action(context, "workspace.physics.add_constraint",
                       session.constraints.size() < Runtime::MaximumPhysicsConstraintsPerObject))
                session.AddConstraint();
        }

        /** @brief Cancellation wins over Apply in the same frame; requests remain detached until host admission. */
        void DrawActions(PhysicsInspectorSession &session, bool &inputError, const SceneObjectId object,
                         EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
            const bool apply =
                Action(context, "workspace.physics.apply", session.dirty && command.command == EditorWorkspaceViewCommand::None);
            const bool cancel =
                Action(context, "workspace.physics.cancel", session.dirty) ||
                (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false));
            if (cancel) {
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
        if (item("workspace.physics.add_body", "###inspector_physics_body", !m_physicsSession.body))
            m_physicsSession.AddBody();
        if (item("workspace.physics.add_collider", "###inspector_physics_collider",
                 m_physicsSession.colliders.size() < Runtime::MaximumPhysicsCollidersPerBody))
            m_physicsSession.AddCollider();
        if (item("workspace.physics.add_constraint", "###inspector_physics_constraint",
                 m_physicsSession.constraints.size() < Runtime::MaximumPhysicsConstraintsPerObject))
            m_physicsSession.AddConstraint();
    }

    /** @copydoc InspectorPanel::DrawPhysicsInspector */
    void InspectorPanel::DrawPhysicsInspector(const SceneObject &object, const EditorWorkspaceViewModel &viewModel,
                                              EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        using namespace PhysicsInspectorWidgets;
        if (m_physicsSession.Begin(object, viewModel.physicsContext, viewModel.physicsSelectionRevision))
            m_physicsInputError = false;
        // Absent components use the normal add-component route. A dirty empty draft still needs Apply/Cancel for removal.
        if (!m_physicsSession.body && m_physicsSession.colliders.empty() && m_physicsSession.constraints.empty() && !m_physicsSession.dirty)
            return;
        Ui::FieldLabel(Label(context, "workspace.physics.title"), context.theme.fonts);
        Ui::Hint(Label(context, "workspace.physics.authoring_only"), context.theme.fonts);
        Stable(context, "workspace.physics.object_id", object.id.value);
        const bool canEdit = viewModel.physicsContext.canEdit && !object.effectivelyLocked;
        if (!canEdit)
            Ui::Hint(Label(context, "workspace.physics.unavailable"), context.theme.fonts);
        ImGui::BeginDisabled(!canEdit);
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
