#pragma once

/** @file PhysicsInspector.h
 * @brief Detached typed Physics edits prepared for the canonical Scene transaction owner.
 */
#include "Horo/Editor/SceneDocumentTransaction.h"

namespace Horo::Editor {
    /** @brief Authored Physics values only; no solver, project lookup or document authority. */
    struct PhysicsInspectorValues final {
        std::optional<Runtime::RigidBodyComponent> body;
        std::vector<Runtime::ColliderComponent> colliders;
        std::vector<Runtime::PhysicsConstraintComponent> constraints;
    };

    /** @brief Prepare one complete Physics edit without publishing document or history state.
     * @details Retains every non-Physics field and exact object ownership. Existing component identities/schema
     * remain stable, changed payloads advance their generation without wrapping, and new components start at one.
     * The complete authored body/collider/constraint graph is validated before returning a replacement. Project
     * profile/material identities remain explicit authored references; this operation does not resolve assets or
     * promise native support. The caller submits the result through SceneDocumentTransactions::Execute with the
     * same snapshot session/revision/state and separately proves current selection and edit permission.
     * @param snapshot Exact bounded owner snapshot, captured outside the draw loop.
     * @param object Hand-authored object selected by the host.
     * @param values Complete desired Physics values with unchanged baseline generations for existing identities.
     * @param limits Finite preparation and snapshot ceilings.
     * @return Detached replacement or original typed validation/capacity/stale rejection; publishes nothing.
     */
    [[nodiscard]] Result<SceneObjectReplacement> PreparePhysicsInspectorEdit(const SceneTransactionSnapshot &snapshot,
                                                                             SceneSource::SceneObjectId object,
                                                                             const PhysicsInspectorValues &values,
                                                                             SceneTransactionLimits limits = {});
}  // namespace Horo::Editor
