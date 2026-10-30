#include "component_fields/definitions/BoxColliderFields.h"
#include "component_fields/definitions/CapsuleColliderFields.h"
#include "component_fields/definitions/PlaneColliderFields.h"
#include "component_fields/definitions/SphereColliderFields.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/inspector/PhysicsInspectorWidgets.h"

#include <algorithm>
#include <limits>

namespace Horo::Editor::PhysicsInspectorWidgets {
    namespace {
        /** @brief Selects only named, owner-published references while retaining unresolved typed values. */
        template <typename Choice, typename Id>
        bool Reference(const EditorGuiContext &context, const char *key, PhysicsInspectorText &text, Id &value,
                       const std::vector<Choice> &choices, const char *emptyKey) {
            Field(context, key);
            int selected = 0;
            for (std::size_t index = 0; index < choices.size(); ++index)
                if (choices[index].id == value)
                    selected = static_cast<int>(index) + 1;
            const Ui::ComboItemSource source{.label = [&](const int index) {
                if (index != 0)
                    return choices[static_cast<std::size_t>(index - 1)].name.c_str();
                return Label(context, value.IsValid() ? "workspace.physics.missing_reference" : "workspace.physics.choose_reference");
            }};
            ImGui::PushID(key);
            ImGui::PushItemWidth(-1);
            ImGui::BeginDisabled(choices.empty());
            const bool changed = Ui::ComboControl("##reference", &selected, static_cast<int>(choices.size()) + 1, source,
                                                  context.theme.fonts, {.error = !text.valid});
            ImGui::EndDisabled();
            ImGui::PopItemWidth();
            ImGui::PopID();
            if (choices.empty())
                Ui::Hint(Label(context, emptyKey), context.theme.fonts);
            if (!changed || selected == 0)
                return false;
            value = choices[static_cast<std::size_t>(selected - 1)].id;
            text = PhysicsInspectorSession::Identity(value.ToString());
            return true;
        }

        /** @brief Stage an explicit source-kind change; asset references remain unresolved until authored. */
        bool SourceKind(const EditorGuiContext &context, PhysicsInspectorColliderDraft &draft) {
            const std::array entries{Label(context, "workspace.physics.box"), Label(context, "workspace.physics.sphere"),
                                     Label(context, "workspace.physics.capsule"), Label(context, "workspace.physics.plane"),
                                     Label(context, "workspace.physics.shape_asset")};
            const auto *analytic = std::get_if<Runtime::PhysicsAnalyticCollider>(&draft.value.source);
            int kind = analytic ? static_cast<int>(analytic->index()) : 4;
            if (!Choice(context, "workspace.physics.shape", "shape", kind, entries))
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
            return std::visit([&context](auto &value) {
                return InspectorFields::Draw(value, context, InspectorFields::Layout::Stacked).changed;
            }, shape);
        }

        /** @brief Draw the complete slot mapping; no material lookup or implicit default is performed. */
        bool Materials(const EditorGuiContext &context, PhysicsInspectorColliderDraft &draft, const EditorWorkspaceViewModel &view) {
            bool changed = false;
            const bool assetShape = std::holds_alternative<Runtime::PhysicsShapeAssetReference>(draft.value.source);
            for (std::size_t index = 0; index < draft.materials.size(); ++index) {
                ImGui::PushID(static_cast<int>(index));
                auto &material = draft.materials[index];
                if (assetShape) {
                    auto slot = material.value.slot.Value();
                    if (Integer(context, "workspace.physics.material_slot", material.slot, slot)) {
                        material.value.slot = Physics::PhysicsMaterialSlotId::FromValue(slot);
                        changed = true;
                    }
                }
                changed = Reference(context, "workspace.physics.material_asset", material.asset, material.value.material,
                                    view.physicsMaterials, "workspace.physics.no_materials") ||
                          changed;
                const bool remove = draft.materials.size() > 1 && Action(context, "workspace.physics.remove_material");
                ImGui::PopID();
                if (remove) {
                    draft.materials.erase(draft.materials.begin() + static_cast<std::ptrdiff_t>(index));
                    return true;
                }
            }
            std::uint64_t nextSlot = 0;
            for (const auto &material : draft.materials)
                nextSlot = std::max(nextSlot, material.value.slot.Value());
            if (assetShape && Action(context, "workspace.physics.add_material",
                                     draft.materials.size() < Runtime::MaximumPhysicsColliderMaterialBindings &&
                                         nextSlot != std::numeric_limits<std::uint64_t>::max())) {
                draft.materials.push_back({{Physics::PhysicsMaterialSlotId::FromValue(nextSlot + 1), {}},
                                           PhysicsInspectorSession::Decimal(nextSlot + 1),
                                           PhysicsInspectorSession::Identity("")});
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
    bool Collider(const EditorGuiContext &context, PhysicsInspectorColliderDraft &draft, const EditorWorkspaceViewModel &view) {
        auto &value = draft.value;
        bool changed = Toggle(context, "workspace.physics.enabled", value.enabled);
        changed = Toggle(context, "workspace.physics.sensor", value.sensor) || changed;
        changed = Source(context, draft) || changed;
        changed = Pose(context, value.localPose) || changed;
        changed = Vector(context, "workspace.physics.scale", value.scale) || changed;
        changed = Reference(context, "workspace.physics.profile", draft.profile, value.collisionProfile, view.physicsProfiles,
                            "workspace.physics.no_profiles") ||
                  changed;
        changed = Materials(context, draft, view) || changed;
        Details(context, value.id.value);
        return changed;
    }
}  // namespace Horo::Editor::PhysicsInspectorWidgets
