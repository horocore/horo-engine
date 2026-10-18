#include "editor/screens/workspace/panels/inspector/PhysicsInspectorWidgets.h"

namespace Horo::Editor::PhysicsInspectorWidgets {
    namespace {
        /** @brief Draw a body-local endpoint with explicit stable target IDs and frame. */
        bool Endpoint(const EditorGuiContext &context, Runtime::PhysicsConstraintBodyEndpoint &endpoint, PhysicsInspectorText &object,
                      PhysicsInspectorText &slot) {
            bool changed = Integer(context, "workspace.physics.body_object", object, endpoint.body.object.value);
            changed = Integer(context, "workspace.physics.body_slot", slot, endpoint.body.body.value) || changed;
            return Pose(context, endpoint.localFrame) || changed;
        }

        /** @brief Stage the second endpoint explicitly; switching to a body requires supplying its identity. */
        bool Second(const EditorGuiContext &context, PhysicsInspectorConstraintDraft &draft) {
            const std::array entries{Label(context, "workspace.physics.world"), Label(context, "workspace.physics.body")};
            int mode = std::holds_alternative<Runtime::PhysicsConstraintBodyEndpoint>(draft.value.second) ? 1 : 0;
            bool changed = Ui::DrawComboPropRow(Label(context, "workspace.physics.second_endpoint"), "second_kind", mode, entries,
                                                context.theme.fonts);
            if (changed) {
                if (mode == 1) {
                    draft.value.second = Runtime::PhysicsConstraintBodyEndpoint{};
                    draft.secondObject = PhysicsInspectorSession::Decimal(0);
                    draft.secondBody = PhysicsInspectorSession::Decimal(0);
                } else {
                    draft.value.second = Runtime::PhysicsConstraintWorldEndpoint{};
                }
            }
            if (auto *body = std::get_if<Runtime::PhysicsConstraintBodyEndpoint>(&draft.value.second))
                return Endpoint(context, *body, draft.secondObject, draft.secondBody) || changed;
            return Pose(context, std::get<Runtime::PhysicsConstraintWorldEndpoint>(draft.value.second).frame) || changed;
        }

        /** @brief Expose only the canonical serializable fixed/distance profile. */
        bool Parameters(const EditorGuiContext &context, Runtime::PhysicsConstraintComponent &value) {
            const std::array entries{Label(context, "workspace.physics.fixed"), Label(context, "workspace.physics.distance")};
            int kind = std::holds_alternative<Runtime::PhysicsDistanceConstraint>(value.parameters) ? 1 : 0;
            bool changed = Ui::DrawComboPropRow(Label(context, "workspace.physics.constraint_kind"), "constraint_kind", kind, entries,
                                                context.theme.fonts);
            if (changed) {
                if (kind == 1)
                    value.parameters = Runtime::PhysicsDistanceConstraint{};
                else
                    value.parameters = Runtime::PhysicsFixedConstraint{};
            }
            if (auto *distance = std::get_if<Runtime::PhysicsDistanceConstraint>(&value.parameters)) {
                changed = Scalar(context, "workspace.physics.minimum_distance", distance->minimumMeters) || changed;
                changed = Scalar(context, "workspace.physics.maximum_distance", distance->maximumMeters) || changed;
            }
            return changed;
        }
    }  // namespace

    /** @copydoc Constraint */
    bool Constraint(const EditorGuiContext &context, PhysicsInspectorConstraintDraft &draft) {
        Stable(context, "workspace.physics.component_id", draft.value.id.value);
        Stable(context, "workspace.physics.constraint_slot", draft.value.constraint.value);
        Stable(context, "workspace.physics.generation", draft.value.generation);
        Stable(context, "workspace.physics.schema", draft.value.schemaVersion);
        bool changed = Toggle(context, "workspace.physics.enabled", draft.value.enabled);
        changed = Parameters(context, draft.value) || changed;
        Ui::FieldLabel(Label(context, "workspace.physics.first_endpoint"), context.theme.fonts);
        ImGui::PushID("first");
        changed = Endpoint(context, draft.value.first, draft.firstObject, draft.firstBody) || changed;
        ImGui::PopID();
        Ui::FieldLabel(Label(context, "workspace.physics.second_endpoint"), context.theme.fonts);
        ImGui::PushID("second");
        changed = Second(context, draft) || changed;
        ImGui::PopID();
        Ui::Hint(Label(context, "workspace.physics.constraint_unsupported"), context.theme.fonts);
        return changed;
    }
}  // namespace Horo::Editor::PhysicsInspectorWidgets
