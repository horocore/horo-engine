#include "editor/screens/workspace/panels/inspector/PhysicsInspectorWidgets.h"

namespace Horo::Editor::PhysicsInspectorWidgets {
    namespace {
        /** @brief Stage an explicit source-kind change; asset references remain unresolved until authored. */
        bool SourceKind(const EditorGuiContext &context, PhysicsInspectorColliderDraft &draft) {
            const std::array entries{Label(context, "workspace.physics.box"), Label(context, "workspace.physics.sphere"),
                                     Label(context, "workspace.physics.capsule"), Label(context, "workspace.physics.plane"),
                                     Label(context, "workspace.physics.shape_asset")};
            const auto *analytic = std::get_if<Runtime::PhysicsAnalyticCollider>(&draft.value.source);
            int kind = analytic ? static_cast<int>(analytic->index()) : 4;
            if (!Ui::DrawComboPropRow(Label(context, "workspace.physics.shape"), "shape", kind, entries, context.theme.fonts))
                return false;
            switch (kind) {
                case 0:
                    draft.value.source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsBoxCollider{}};
                    break;
                case 1:
                    draft.value.source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{}};
                    break;
                case 2:
                    draft.value.source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsCapsuleCollider{}};
                    break;
                case 3:
                    draft.value.source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsStaticPlaneCollider{}};
                    break;
                default:
                    draft.value.source = Runtime::PhysicsShapeAssetReference{};
                    draft.shapeAsset = PhysicsInspectorSession::Identity("");
                    draft.shapePart = PhysicsInspectorSession::Decimal(0);
                    break;
            }
            return true;
        }

        /** @brief Draw the exact fields of the selected analytic shape, preserving unsupported inputs for correction. */
        bool Analytic(const EditorGuiContext &context, Runtime::PhysicsAnalyticCollider &shape) {
            if (auto *box = std::get_if<Runtime::PhysicsBoxCollider>(&shape))
                return Vector(context, "workspace.physics.half_extents", box->halfExtentsMeters);
            if (auto *sphere = std::get_if<Runtime::PhysicsSphereCollider>(&shape))
                return Scalar(context, "workspace.physics.radius", sphere->radiusMeters);
            if (auto *capsule = std::get_if<Runtime::PhysicsCapsuleCollider>(&shape)) {
                const bool radius = Scalar(context, "workspace.physics.radius", capsule->radiusMeters);
                return Scalar(context, "workspace.physics.cylinder_half_height", capsule->cylindricalHalfHeightMeters) || radius;
            }
            auto &plane = std::get<Runtime::PhysicsStaticPlaneCollider>(shape);
            const bool normal = Vector(context, "workspace.physics.plane_normal", plane.normal);
            return Scalar(context, "workspace.physics.plane_distance", plane.signedDistanceMeters) || normal;
        }

        /** @brief Draw the complete slot mapping; no material lookup or implicit default is performed. */
        bool Materials(const EditorGuiContext &context, PhysicsInspectorColliderDraft &draft) {
            bool changed = false;
            for (std::size_t index = 0; index < draft.materials.size(); ++index) {
                ImGui::PushID(static_cast<int>(index));
                auto &material = draft.materials[index];
                auto slot = material.value.slot.Value();
                if (Integer(context, "workspace.physics.material_slot", material.slot, slot)) {
                    material.value.slot = Physics::PhysicsMaterialSlotId::FromValue(slot);
                    changed = true;
                }
                changed = Asset(context, "workspace.physics.material_asset", material.asset, material.value.material) || changed;
                const bool remove = Action(context, "workspace.physics.remove_material");
                ImGui::PopID();
                if (remove) {
                    draft.materials.erase(draft.materials.begin() + static_cast<std::ptrdiff_t>(index));
                    return true;
                }
            }
            if (Action(context, "workspace.physics.add_material",
                       draft.materials.size() < Runtime::MaximumPhysicsColliderMaterialBindings)) {
                draft.materials.push_back({{}, PhysicsInspectorSession::Decimal(0), PhysicsInspectorSession::Identity("")});
                return true;
            }
            return changed;
        }

        /** @brief Keep source-specific asset fields separate from the shape selector and material map. */
        bool Source(const EditorGuiContext &context, PhysicsInspectorColliderDraft &draft) {
            bool changed = SourceKind(context, draft);
            if (auto *analytic = std::get_if<Runtime::PhysicsAnalyticCollider>(&draft.value.source))
                return Analytic(context, *analytic) || changed;
            auto &asset = std::get<Runtime::PhysicsShapeAssetReference>(draft.value.source);
            changed = Asset(context, "workspace.physics.shape_asset", draft.shapeAsset, asset.asset) || changed;
            return Integer(context, "workspace.physics.shape_part", draft.shapePart, asset.subresource.value) || changed;
        }
    }  // namespace

    /** @copydoc Collider */
    bool Collider(const EditorGuiContext &context, PhysicsInspectorColliderDraft &draft) {
        auto &value = draft.value;
        Stable(context, "workspace.physics.component_id", value.id.value);
        Stable(context, "workspace.physics.collider_slot", value.collider.value);
        Stable(context, "workspace.physics.generation", value.generation);
        Stable(context, "workspace.physics.schema", value.schemaVersion);
        bool changed = Toggle(context, "workspace.physics.enabled", value.enabled);
        changed = Toggle(context, "workspace.physics.sensor", value.sensor) || changed;
        changed = Integer(context, "workspace.physics.body_object", draft.bodyObject, value.body.object.value) || changed;
        changed = Integer(context, "workspace.physics.body_slot", draft.bodySlot, value.body.body.value) || changed;
        changed = Source(context, draft) || changed;
        changed = Pose(context, value.localPose) || changed;
        changed = Vector(context, "workspace.physics.scale", value.scale) || changed;
        changed = Profile(context, draft.profile, value.collisionProfile) || changed;
        return Materials(context, draft) || changed;
    }
}  // namespace Horo::Editor::PhysicsInspectorWidgets
