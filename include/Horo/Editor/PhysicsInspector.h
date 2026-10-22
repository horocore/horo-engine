#pragma once

/** @file PhysicsInspector.h
 * @brief Detached typed Physics edits prepared for the canonical Scene transaction owner.
 */
#include "Horo/Editor/SceneDocumentTransaction.h"

#include <array>

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

    /** @brief Persistent collider contributor identity; array order never identifies a compound child. */
    struct ColliderEditTarget final {
        SceneSource::SceneObjectId object;
        Runtime::PhysicsComponentId component;
        [[nodiscard]] constexpr auto operator<=>(const ColliderEditTarget &) const noexcept = default;
    };

    /** @brief Detached authored child projected in its explicit body's space, with no runtime handles. */
    struct ColliderPresentation final {
        ColliderEditTarget target;
        Runtime::PhysicsBodyReference body;
        std::uint64_t generation{};
        Runtime::PhysicsAnalyticCollider shape;
        Math::Transform localTransform;
        Math::Mat4 bodyToWorld{Math::Mat4::Identity()};
        bool editable{};
        bool visible{};
        std::optional<Error> diagnostic; /**< Unsupported asset sources and invalid body transforms remain explicit. */
    };

    /** @brief One generation-fenced absolute pose/scale replacement; shape dimensions remain canonical authored values. */
    struct ColliderTransformEdit final {
        ColliderEditTarget target;
        std::uint64_t generation{};
        Math::Transform transform;
    };

    /** @brief Fixed-storage world-space wireframe segment. */
    struct ColliderWireSegment final {
        Math::Vec3 first;
        Math::Vec3 second;
    };

    /** @brief Bounded overlay geometry; no per-frame allocation or native renderer dependency. */
    struct ColliderWireframe final {
        std::array<ColliderWireSegment, 160> segments{};
        std::size_t count{};
    };

    /** @brief Project canonical analytic contributors and explicit compound body relationships.
     * @param objects Exact committed authored root. @param limits Bounded snapshot ceilings.
     * @return Owned presentation or capacity/identity error; cooked sources retain an unsupported diagnostic.
     */
    [[nodiscard]] Result<std::vector<ColliderPresentation>> ProjectPhysicsColliders(
        std::span<const SceneSource::SceneObjectSnapshot> objects, SceneTransactionLimits limits = {});
    /** @brief Generate finite box/sphere/capsule/plane guides from the same geometry used by picking.
     * @param collider Current detached projection or transient preview. @return Fixed world segments or its diagnostic.
     */
    [[nodiscard]] Result<ColliderWireframe> BuildColliderWireframe(const ColliderPresentation &collider);
    /** @brief Pick exact analytic surfaces, preserving world ray distance under local affine inversion.
     * @param colliders Current committed/preview projections. @param ray Normalized finite world ray segment.
     * @return Nearest visible supported child identity, stable on ties, or invalid-ray failure.
     */
    [[nodiscard]] Result<std::optional<ColliderEditTarget>> PickPhysicsCollider(std::span<const ColliderPresentation> colliders,
                                                                                const Math::Ray &ray);
    /** @brief Evaluate a shared world-space group delta around the primary child's pivot.
     * @param selection Primary-first committed child projections retained for the gesture.
     * @param primaryDraft New primary body-local pose/scale. @param snap Enable 0.1 m, 15 degree and 10 percent increments.
     * @param snapAxesWorld World orientation of the active snapping axes; identity selects the world grid.
     * @return Absolute generation-fenced edits, or unsupported shear/nonuniform sphere/capsule and invalid input errors.
     * @details Snap is relative to the captured primary. No owner, history or source is mutated.
     */
    [[nodiscard]] Result<std::vector<ColliderTransformEdit>> EvaluateColliderGroupTransform(std::span<const ColliderPresentation> selection,
                                                                                            Math::Transform primaryDraft, bool snap,
                                                                                            Math::Quaternion snapAxesWorld = {});
    /** @brief Prepare one atomic multi-child edit for the existing Scene/history owner.
     * @param snapshot Exact owner snapshot. @param edits Distinct generation-fenced child edits.
     * @param limits Finite transaction ceilings. @return Complete replacements or zero-mutation rejection.
     * @details Preserves contributor identity, body relationship, shape source, materials and unrelated fields.
     * No source-asset interior is guessed or edited. Execute still owns thread, lock, session, cancel and reload admission.
     */
    [[nodiscard]] Result<std::vector<SceneObjectReplacement>> PrepareColliderTransformEdits(const SceneTransactionSnapshot &snapshot,
                                                                                            std::span<const ColliderTransformEdit> edits,
                                                                                            SceneTransactionLimits limits = {});
}  // namespace Horo::Editor
