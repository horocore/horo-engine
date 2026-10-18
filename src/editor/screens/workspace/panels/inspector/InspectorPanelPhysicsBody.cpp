#include "editor/screens/workspace/panels/inspector/PhysicsInspectorWidgets.h"

namespace Horo::Editor::PhysicsInspectorWidgets {
    namespace {
        /** @brief Motion changes select the corresponding explicit authored mass representation. */
        bool Motion(const EditorGuiContext &context, Runtime::RigidBodyComponent &body) {
            const std::array entries{Label(context, "workspace.physics.static"), Label(context, "workspace.physics.kinematic"),
                                     Label(context, "workspace.physics.dynamic")};
            auto motion = static_cast<int>(body.motion);
            if (!Ui::DrawComboPropRow(Label(context, "workspace.physics.motion"), "motion", motion, entries, context.theme.fonts))
                return false;
            body.motion = static_cast<Runtime::AuthoredPhysicsMotionType>(motion);
            if (body.motion == Runtime::AuthoredPhysicsMotionType::Dynamic)
                body.mass = Runtime::AuthoredPhysicsMass{};
            else
                body.mass = Runtime::AuthoredPhysicsNoMass{};
            return true;
        }

        /** @brief Keep mass and density distinct authoring choices, never inferred from a render mesh. */
        bool Mass(const EditorGuiContext &context, Runtime::RigidBodyComponent &body) {
            if (body.motion != Runtime::AuthoredPhysicsMotionType::Dynamic)
                return false;
            const std::array entries{Label(context, "workspace.physics.mass"), Label(context, "workspace.physics.density")};
            int mode = std::holds_alternative<Runtime::AuthoredPhysicsDensity>(body.mass) ? 1 : 0;
            bool changed =
                Ui::DrawComboPropRow(Label(context, "workspace.physics.mass_mode"), "mass_mode", mode, entries, context.theme.fonts);
            if (changed)
                body.mass = mode == 1 ? Runtime::AuthoredPhysicsMassPolicy{Runtime::AuthoredPhysicsDensity{}}
                                      : Runtime::AuthoredPhysicsMassPolicy{Runtime::AuthoredPhysicsMass{}};
            if (auto *mass = std::get_if<Runtime::AuthoredPhysicsMass>(&body.mass))
                changed = Scalar(context, "workspace.physics.mass", mass->kilograms) || changed;
            if (auto *density = std::get_if<Runtime::AuthoredPhysicsDensity>(&body.mass))
                changed = Scalar(context, "workspace.physics.density", density->kilogramsPerCubicMeter) || changed;
            return changed;
        }
    }  // namespace

    /** @copydoc Body */
    bool Body(const EditorGuiContext &context, Runtime::RigidBodyComponent &body) {
        Stable(context, "workspace.physics.component_id", body.id.value);
        Stable(context, "workspace.physics.body_slot", body.body.value);
        Stable(context, "workspace.physics.generation", body.generation);
        Stable(context, "workspace.physics.schema", body.schemaVersion);
        bool changed = Toggle(context, "workspace.physics.enabled", body.enabled);
        changed = Motion(context, body) || changed;
        changed = Mass(context, body) || changed;
        changed = Vector(context, "workspace.physics.linear_velocity", body.initialLinearVelocity) || changed;
        changed = Vector(context, "workspace.physics.angular_velocity", body.initialAngularVelocity) || changed;
        changed = Scalar(context, "workspace.physics.linear_damping", body.linearDampingPerSecond) || changed;
        changed = Scalar(context, "workspace.physics.angular_damping", body.angularDampingPerSecond) || changed;
        changed = Scalar(context, "workspace.physics.linear_limit", body.maximumLinearSpeed) || changed;
        changed = Scalar(context, "workspace.physics.angular_limit", body.maximumAngularSpeed) || changed;
        return changed;
    }
}  // namespace Horo::Editor::PhysicsInspectorWidgets
