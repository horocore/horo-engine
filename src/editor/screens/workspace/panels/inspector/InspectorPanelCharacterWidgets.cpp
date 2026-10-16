#include "editor/screens/workspace/panels/inspector/InspectorPanelInternal.h"

#include <array>
#include <utility>

namespace Horo::Editor {
    using namespace InspectorPanelDetail;

    namespace {
        /** @brief Accumulates semantic edit events while every shared control is still drawn. */
        void Merge(Ui::PropertyEditResult &target, const Ui::PropertyEditResult value) noexcept {
            target.changed |= value.changed;
            target.committed |= value.committed;
        }

        /** @brief Uses the existing shared localized boolean row without a raw ImGui checkbox. */
        [[nodiscard]] bool Boolean(const char *key, bool &value, const EditorGuiContext &context) {
            const std::array entries{context.localization.Get("editor", "workspace.value.off").c_str(),
                                     context.localization.Get("editor", "workspace.value.on").c_str()};
            int selected = value ? 1 : 0;
            const bool changed =
                Ui::DrawComboPropRow(context.localization.Get("editor", key).c_str(), key, selected, entries, context.theme.fonts);
            if (changed)
                value = selected != 0;
            return changed;
        }

        /** @brief Keeps world-basis vectors typed outside the shared three-axis presentation control. */
        [[nodiscard]] Ui::PropertyEditResult Vector(const char *key, Math::Vec3 &value, const EditorGuiContext &context) {
            std::array fields{value.x, value.y, value.z};
            const auto edit = Ui::DrawFloat3PropRow(context.localization.Get("editor", key).c_str(), key, fields, context.theme.fonts);
            if (edit.changed)
                value = {fields[0], fields[1], fields[2]};
            return edit;
        }

        /** @brief Draws capsule dimensions in canonical meters, independent of visual hierarchy scale. */
        [[nodiscard]] Ui::PropertyEditResult Capsule(Runtime::PhysicsCapsuleCollider &shape, bool crouched,
                                                     const EditorGuiContext &context) {
            const char *radius = crouched ? "workspace.inspector.character.crouched_radius" : "workspace.inspector.character.radius";
            const char *height =
                crouched ? "workspace.inspector.character.crouched_half_height" : "workspace.inspector.character.half_height";
            Ui::PropertyEditResult result;
            Merge(result, Ui::DrawFloatPropRow(context.localization.Get("editor", radius).c_str(), radius, shape.radiusMeters,
                                               context.theme.fonts));
            Merge(result, Ui::DrawFloatPropRow(context.localization.Get("editor", height).c_str(), height,
                                               shape.cylindricalHalfHeightMeters, context.theme.fonts));
            return result;
        }

        /** @brief Draws finite geometry/basis fields and an explicit optional crouched capsule. */
        [[nodiscard]] Ui::PropertyEditResult Geometry(Runtime::CharacterSceneGeometry &geometry, const EditorGuiContext &context) {
            auto result = Capsule(geometry.capsule, false, context);
            Merge(result, Vector("workspace.inspector.character.up", geometry.up, context));
            Merge(result, Vector("workspace.inspector.character.gravity", geometry.gravity, context));
            bool crouched = geometry.crouchedCapsule.has_value();
            if (Boolean("workspace.inspector.character.crouch", crouched, context)) {
                if (crouched)
                    geometry.crouchedCapsule =
                        Runtime::PhysicsCapsuleCollider{geometry.capsule.radiusMeters, geometry.capsule.cylindricalHalfHeightMeters * 0.5F};
                else
                    geometry.crouchedCapsule.reset();
                result.changed = result.committed = true;
            }
            if (geometry.crouchedCapsule)
                Merge(result, Capsule(*geometry.crouchedCapsule, true, context));
            return result;
        }

        /** @brief Draws all movement scalars through shared property controls. */
        [[nodiscard]] Ui::PropertyEditResult Movement(Runtime::CharacterSceneMovement &movement, const EditorGuiContext &context) {
            const std::array fields{std::pair{"workspace.inspector.character.skin", &movement.skinWidthMeters},
                                    std::pair{"workspace.inspector.character.minimum_move", &movement.minimumMoveDistanceMeters},
                                    std::pair{"workspace.inspector.character.step", &movement.maximumStepHeightMeters},
                                    std::pair{"workspace.inspector.character.slope", &movement.maximumSlopeDegrees},
                                    std::pair{"workspace.inspector.character.jump", &movement.jumpSpeedMetersPerSecond}};
            Ui::PropertyEditResult result;
            for (const auto &[key, value] : fields)
                Merge(result, Ui::DrawFloatPropRow(context.localization.Get("editor", key).c_str(), key, *value, context.theme.fonts));
            return result;
        }

        /** @brief Draws the closed policy vocabulary using localized entries in exact typed enum order. */
        [[nodiscard]] bool Policies(Runtime::CharacterSceneMovement &movement, const EditorGuiContext &context) {
            bool changed = Boolean("workspace.inspector.character.preserve_speed", movement.preserveHorizontalSpeed, context);
            changed |= Boolean("workspace.inspector.character.dynamic_platform", movement.dynamicPlatformSupport, context);
            changed |= Boolean("workspace.inspector.character.platform_heading", movement.inheritPlatformHeading, context);
            const std::array slopes{context.localization.Get("editor", "workspace.inspector.character.stop").c_str(),
                                    context.localization.Get("editor", "workspace.inspector.character.slide").c_str()};
            int slope = static_cast<int>(movement.steepSlopePolicy);
            if (Ui::DrawComboPropRow(context.localization.Get("editor", "workspace.inspector.character.steep_policy").c_str(),
                                     "steep_policy", slope, slopes, context.theme.fonts)) {
                movement.steepSlopePolicy = static_cast<Character::CharacterSteepSlopePolicy>(slope);
                changed = true;
            }
            const std::array leave{context.localization.Get("editor", "workspace.inspector.character.none").c_str(),
                                   context.localization.Get("editor", "workspace.inspector.character.linear").c_str(),
                                   context.localization.Get("editor", "workspace.inspector.character.point").c_str()};
            int velocity = static_cast<int>(movement.platformLeaveVelocity);
            if (Ui::DrawComboPropRow(context.localization.Get("editor", "workspace.inspector.character.leave_velocity").c_str(),
                                     "leave_velocity", velocity, leave, context.theme.fonts)) {
                movement.platformLeaveVelocity = static_cast<Character::CharacterPlatformLeaveVelocityPolicy>(velocity);
                changed = true;
            }
            return changed;
        }

        /** @brief Keeps bounded invalid UUID/decimal text visible while only the reducer admits complete typed bindings. */
        [[nodiscard]] Ui::PropertyEditResult Bindings(InspectorCharacterBindings &bindings, const EditorGuiContext &context) {
            constexpr std::array keys{"workspace.inspector.character.profile",         "workspace.inspector.character.query",
                                      "workspace.inspector.character.material",        "workspace.inspector.character.slot",
                                      "workspace.inspector.character.required_layer",  "workspace.inspector.character.required_profile",
                                      "workspace.inspector.character.excluded_object", "workspace.inspector.character.excluded_body",
                                      "workspace.inspector.character.contacts"};
            Ui::PropertyEditResult result;
            for (std::size_t index = 0; index < keys.size(); ++index) {
                Ui::DrawPropRow(context.localization.Get("editor", keys[index]).c_str(), "", context.theme.fonts);
                result.changed |=
                    Ui::InputTextControl(keys[index], bindings.text[index], 65, context.theme.fonts, {.error = !bindings.valid});
                result.committed |= ImGui::IsItemDeactivatedAfterEdit();
            }
            return result;
        }
    }  // namespace

    /** @copydoc InspectorPanel::DrawCharacterWidgets */
    InspectorCharacterEdit InspectorPanel::DrawCharacterWidgets(const EditorGuiContext &context) {
        auto &draft = m_editSession.Draft();
        if (!draft.characterController)
            return {};
        Ui::Card card(Ui::CardProps{.id = "##CharacterControllerCard"});
        const auto header = DrawComponentTitleBar(card, context.localization.Get("editor", "workspace.inspector.character.title").c_str(),
                                                  {.enabled = draft.characterController->enabled}, context);
        if (header.resetRequested) {
            draft.characterController = Runtime::CharacterControllerComponent{.id = draft.characterController->id,
                                                                              .enabled = draft.characterController->enabled,
                                                                              .collision = draft.characterController->collision};
            draft.characterBindings.text[8] = "16";
        }
        if (header.toggleEnabledRequested)
            draft.characterController->enabled = !draft.characterController->enabled;
        Ui::PropertyEditResult edit{header.resetRequested || header.toggleEnabledRequested,
                                    header.resetRequested || header.toggleEnabledRequested};
        const bool cancelled = IsInspectorCardEscapeRequested();
        if (card.BeginBody()) {
            Merge(edit, Geometry(draft.characterController->geometry, context));
            Merge(edit, Movement(draft.characterController->movement, context));
            if (Policies(draft.characterController->movement, context))
                edit.changed = edit.committed = true;
            Merge(edit, Bindings(draft.characterBindings, context));
            card.Finish();
        }
        return {.changed = edit.changed,
                .committed = edit.committed,
                .cancelRequested = cancelled,
                .removeRequested = header.removeRequested};
    }
}  // namespace Horo::Editor
