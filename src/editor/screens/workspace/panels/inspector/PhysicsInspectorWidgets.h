#pragma once

/** @file PhysicsInspectorWidgets.h
 * @brief Private shared-control adapters for bounded typed Physics drafts.
 */
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/inspector/PhysicsInspectorSession.h"

namespace Horo::Editor::PhysicsInspectorWidgets {
    /** @brief Resolve only localized presentation text. */
    const char *Label(const EditorGuiContext &context, const char *key);
    /** @brief Edit a finite typed scalar; semantic validation remains with the owning Physics schema. */
    bool Scalar(const EditorGuiContext &context, const char *key, float &value);
    /** @brief Edit exact Cartesian coordinates through the shared axis control. */
    bool Vector(const EditorGuiContext &context, const char *key, Math::Vec3 &value);
    /** @brief Edit a normalized quaternion explicitly without lossy Euler conversion. */
    bool Pose(const EditorGuiContext &context, Runtime::AuthoredPhysicsPose &value);
    /** @brief Edit an authored toggle through the shared checkbox control. */
    bool Toggle(const EditorGuiContext &context, const char *key, bool &value);
    /** @brief Edit canonical decimal text without double conversion or unchecked integer casts. */
    bool Integer(const EditorGuiContext &context, const char *key, PhysicsInspectorText &text, std::uint64_t &value);
    /** @brief Edit a canonical project asset identity; parse only on text changes. */
    bool Asset(const EditorGuiContext &context, const char *key, PhysicsInspectorText &text, Assets::AssetId &value);
    /** @brief Edit a canonical project collision-profile identity without lookup/default substitution. */
    bool Profile(const EditorGuiContext &context, PhysicsInspectorText &text, Physics::CollisionProfileId &value);
    /** @brief Draw one bounded shared action with localized copy. */
    bool Action(const EditorGuiContext &context, const char *key, bool enabled = true);
    /** @brief Show an exact stable identity without a heap allocation or floating-point conversion. */
    void Stable(const EditorGuiContext &context, const char *key, std::uint64_t value);
    /** @brief Draw all authored body fields without creating a native body. */
    bool Body(const EditorGuiContext &context, Runtime::RigidBodyComponent &body);
    /** @brief Draw exact collider source, binding, local pose, scale and sensor properties. */
    bool Collider(const EditorGuiContext &context, PhysicsInspectorColliderDraft &draft);
    /** @brief Draw the fixed/distance authored constraint profile with exact typed endpoints. */
    bool Constraint(const EditorGuiContext &context, PhysicsInspectorConstraintDraft &draft);
}  // namespace Horo::Editor::PhysicsInspectorWidgets
