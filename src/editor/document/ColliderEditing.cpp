#include "ColliderGeometryInternal.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "editor/document/SceneDocument.h"
#include "editor/document/SceneDocumentInternal.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace Horo::Editor {
    namespace {
        using ColliderEditingDetail::ShapeMatrix;

        /** @brief Retain the canonical transaction rejection vocabulary. */
        Error Rejected(const SceneTransactionFailure failure) {
            return MakeError(SceneTransactionError(failure));
        }

        /** @brief Resolve the explicit body hierarchy without recursion or runtime/native state. */
        Result<Math::Mat4> BodyMatrix(const std::span<const SceneSource::SceneObjectSnapshot> objects,
                                      const Runtime::PhysicsBodyReference body) {
            Math::Mat4 matrix = Math::Mat4::Identity();
            std::optional<SceneSource::SceneObjectId> next{SceneSource::SceneObjectId{body.object.value}};
            for (std::size_t depth = 0; depth <= objects.size(); ++depth) {
                if (!next)
                    break;
                const auto object = std::ranges::find(objects, *next, &SceneSource::SceneObjectSnapshot::id);
                if (object == objects.end() ||
                    (depth == 0 && (!object->components.rigidBody || object->components.rigidBody->body != body.body)))
                    return Result<Math::Mat4>::Failure(Rejected(SceneTransactionFailure::Invalid));
                const auto local = object->localTransform.TryToMatrix();
                if (local.HasError())
                    return Result<Math::Mat4>::Failure(local.ErrorValue());
                matrix = Math::Multiply(local.Value(), matrix);
                next = object->parent;
            }
            if (next)
                return Result<Math::Mat4>::Failure(Rejected(SceneTransactionFailure::Invalid));
            const auto transform = Math::TryDecomposeAffineTRS(matrix);
            if (transform.HasError())
                return Result<Math::Mat4>::Failure(transform.ErrorValue());
            if (!Math::NearlyEqual(transform.Value().scale, Math::Vec3{1, 1, 1}))
                return Result<Math::Mat4>::Failure(MakeError(Physics::PhysicsErrors::OperationUnsupported,
                                                             "Collider editing requires the explicit body's unit world scale."));
            return Result<Math::Mat4>::Success(matrix);
        }

        /** @brief Quantize translation in the active gizmo axes rather than unrelated contributor/body axes. */
        Result<Math::Vec3> SnapTranslation(const ColliderPresentation &primary, const Math::Vec3 draft, const Math::Quaternion axesWorld) {
            const auto inverseAxes = axesWorld.TryInverse();
            const auto inverseBody = Math::TryInverseAffine(primary.bodyToWorld);
            if (inverseAxes.HasError())
                return Result<Math::Vec3>::Failure(inverseAxes.ErrorValue());
            if (inverseBody.HasError())
                return Result<Math::Vec3>::Failure(inverseBody.ErrorValue());
            const auto worldDelta = Math::TransformDirection(primary.bodyToWorld, draft - primary.localTransform.translation);
            const auto projected = inverseAxes.Value().TryRotate(worldDelta);
            if (projected.HasError())
                return Result<Math::Vec3>::Failure(projected.ErrorValue());
            const auto quantize = [](const float value) {
                return std::round(value / 0.1F) * 0.1F;
            };
            const auto snapped =
                axesWorld.TryRotate({quantize(projected.Value().x), quantize(projected.Value().y), quantize(projected.Value().z)});
            if (snapped.HasError())
                return Result<Math::Vec3>::Failure(snapped.ErrorValue());
            return Result<Math::Vec3>::Success(primary.localTransform.translation +
                                               Math::TransformDirection(inverseBody.Value(), snapped.Value()));
        }

        /** @brief Snap one validated primary draft without accumulating quantization across preview frames. */
        Result<Math::Transform> SnapPrimary(const ColliderPresentation &primary, Math::Transform draft, const Math::Quaternion axesWorld) {
            const auto quantize = [](const float value, const float step) {
                return std::round(value / step) * step;
            };
            const auto translation = SnapTranslation(primary, draft.translation, axesWorld);
            if (translation.HasError())
                return Result<Math::Transform>::Failure(translation.ErrorValue());
            draft.translation = translation.Value();
            const auto rotation = (draft.rotation * primary.localTransform.rotation.Inverse()).TryNormalized();
            if (rotation.HasError())
                return Result<Math::Transform>::Failure(rotation.ErrorValue());
            if (const Math::Vec3 axis{rotation.Value().x, rotation.Value().y, rotation.Value().z}; Math::LengthSquared(axis) > 1.0e-12F) {
                const float angle = 2 * std::atan2(Math::Length(axis), rotation.Value().w);
                const auto snapped = Math::Quaternion::TryFromAxisAngle(axis, quantize(angle, Math::DegreesToRadians(15)));
                if (snapped.HasError())
                    return Result<Math::Transform>::Failure(snapped.ErrorValue());
                draft.rotation = snapped.Value() * primary.localTransform.rotation;
            }
            const auto scale = [&quantize](const float value, const float original) {
                return original * std::max(0.1F, quantize(value / original, 0.1F));
            };
            draft.scale = {scale(draft.scale.x, primary.localTransform.scale.x), scale(draft.scale.y, primary.localTransform.scale.y),
                           scale(draft.scale.z, primary.localTransform.scale.z)};
            return Result<Math::Transform>::Success(draft);
        }

        /** @brief Convert a common world delta into one contributor's explicitly referenced body space. */
        Result<ColliderTransformEdit> TransformChild(const ColliderPresentation &child, const Math::Mat4 &delta, const bool unchanged) {
            if (!child.editable || child.diagnostic)
                return Result<ColliderTransformEdit>::Failure(child.diagnostic.value_or(Rejected(SceneTransactionFailure::Unauthorized)));
            const auto world = ShapeMatrix(child);
            const auto bodyInverse = Math::TryInverseAffine(child.bodyToWorld);
            if (world.HasError())
                return Result<ColliderTransformEdit>::Failure(world.ErrorValue());
            if (bodyInverse.HasError())
                return Result<ColliderTransformEdit>::Failure(bodyInverse.ErrorValue());
            auto local = Math::TryDecomposeAffineTRS(Math::Multiply(bodyInverse.Value(), Math::Multiply(delta, world.Value())));
            if (local.HasError())
                return Result<ColliderTransformEdit>::Failure(local.ErrorValue());
            if (unchanged)
                local = Result<Math::Transform>::Success(child.localTransform);
            const auto &scale = local.Value().scale;
            if (scale.x <= 0 || scale.y <= 0 || scale.z <= 0)
                return Result<ColliderTransformEdit>::Failure(Rejected(SceneTransactionFailure::Invalid));
            if ((std::holds_alternative<Runtime::PhysicsSphereCollider>(child.shape) ||
                 std::holds_alternative<Runtime::PhysicsCapsuleCollider>(child.shape)) &&
                (!Math::NearlyEqual(scale.x, scale.y) || !Math::NearlyEqual(scale.y, scale.z)))
                return Result<ColliderTransformEdit>::Failure(
                    MakeError(Physics::PhysicsErrors::OperationUnsupported,
                              "Sphere and capsule bounds require uniform scale; use the uniform handle."));
            return Result<ColliderTransformEdit>::Success({child.target, child.generation, local.Value()});
        }

        /** @brief Validate exact contributor generation, source and inherited body/owner edit permissions. */
        Result<void> ValidateEdit(const SceneTransactionSnapshot &snapshot, const ColliderTransformEdit &edit) {
            using enum SceneTransactionFailure;
            const auto object = std::ranges::find(snapshot.objects, edit.target.object, &SceneSource::SceneObjectSnapshot::id);
            if (const auto state = ResolveSceneObjectEditorState(snapshot.objects, edit.target.object);
                object == snapshot.objects.end() || object->generated || !state || state->effectivelyLocked)
                return Result<void>::Failure(Rejected(Unauthorized));
            const auto child = std::ranges::find(object->components.colliders, edit.target.component, &Runtime::ColliderComponent::id);
            if (child == object->components.colliders.end() || child->generation != edit.generation)
                return Result<void>::Failure(Rejected(Stale));
            if (!std::holds_alternative<Runtime::PhysicsAnalyticCollider>(child->source))
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::OperationUnsupported,
                                                       "Cooked collider source editing requires its owning shape document."));
            if (const auto bodyState = ResolveSceneObjectEditorState(snapshot.objects, {child->body.object.value});
                !bodyState || bodyState->effectivelyLocked)
                return Result<void>::Failure(Rejected(Unauthorized));
            if (const auto world = BodyMatrix(snapshot.objects, child->body); world.HasError())
                return Result<void>::Failure(world.ErrorValue());
            if (edit.transform.TryToMatrix().HasError())
                return Result<void>::Failure(Rejected(Invalid));
            return Result<void>::Success();
        }

        /** @brief Resolve portable geometry and its explicit body transform without changing authored data. */
        void ResolveColliderSource(ColliderPresentation &value, const Runtime::ColliderComponent &collider,
                                   const std::span<const SceneSource::SceneObjectSnapshot> objects) {
            if (const auto valid = Runtime::ValidateColliderComponent(collider); valid.HasError()) {
                value.diagnostic = valid.ErrorValue();
            } else if (const auto *analytic = std::get_if<Runtime::PhysicsAnalyticCollider>(&collider.source)) {
                value.shape = *analytic;
                const auto world = BodyMatrix(objects, collider.body);
                if (world.HasError())
                    value.diagnostic = world.ErrorValue();
                else
                    value.bodyToWorld = world.Value();
            } else {
                value.diagnostic =
                    MakeError(Physics::PhysicsErrors::OperationUnsupported,
                              "Cooked collider interiors are immutable. Open the owning shape source before editing its children.");
            }
        }

    }  // namespace

    /** @copydoc ProjectPhysicsColliders */
    Result<std::vector<ColliderPresentation>> ProjectPhysicsColliders(const std::span<const SceneSource::SceneObjectSnapshot> objects,
                                                                      const SceneTransactionLimits limits) {
        using Return = Result<std::vector<ColliderPresentation>>;
        if (objects.size() > limits.maximumSnapshotObjects)
            return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
        try {
            std::vector<ColliderPresentation> result;
            for (const auto &object : objects) {
                const auto state = ResolveSceneObjectEditorState(objects, object.id);
                if (!state)
                    return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
                for (const auto &collider : object.components.colliders) {
                    if (result.size() >= limits.maximumPreparedBytes / (sizeof(ColliderPresentation) + 512))
                        return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
                    ColliderPresentation value{.target = {object.id, collider.id},
                                               .body = collider.body,
                                               .generation = collider.generation,
                                               .localTransform = {collider.localPose.translation, collider.localPose.rotation,
                                                                  collider.scale},
                                               .editable = !state->effectivelyLocked && !object.generated,
                                               .visible = state->effectivelyVisible && collider.enabled};
                    const auto body = std::ranges::find(objects, SceneSource::SceneObjectId{collider.body.object.value},
                                                        &SceneSource::SceneObjectSnapshot::id);
                    const auto bodyState = ResolveSceneObjectEditorState(objects, {collider.body.object.value});
                    value.editable = value.editable && bodyState && !bodyState->effectivelyLocked;
                    value.visible = value.visible && bodyState && bodyState->effectivelyVisible && body != objects.end() &&
                                    body->components.rigidBody && body->components.rigidBody->enabled;
                    ResolveColliderSource(value, collider, objects);
                    value.editable = value.editable && !value.diagnostic;
                    result.push_back(std::move(value));
                }
            }
            return Return::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
        }
    }

    /** @copydoc EvaluateColliderGroupTransform */
    Result<std::vector<ColliderTransformEdit>> EvaluateColliderGroupTransform(const std::span<const ColliderPresentation> selection,
                                                                              Math::Transform primaryDraft, const bool snap,
                                                                              const Math::Quaternion snapAxesWorld) {
        using Return = Result<std::vector<ColliderTransformEdit>>;
        if (selection.empty() || selection.size() > SceneTransactionLimits{}.maximumChanges || primaryDraft.TryToMatrix().HasError())
            return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
        const auto &primary = selection.front();
        const auto initial = ShapeMatrix(primary);
        if (initial.HasError())
            return Return::Failure(initial.ErrorValue());
        if (primaryDraft.scale.x <= 0 || primaryDraft.scale.y <= 0 || primaryDraft.scale.z <= 0)
            return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
        if (snap) {
            const auto snapped = SnapPrimary(primary, primaryDraft, snapAxesWorld);
            if (snapped.HasError())
                return Return::Failure(snapped.ErrorValue());
            primaryDraft = snapped.Value();
        }
        const auto draft = primaryDraft.TryToMatrix();
        if (draft.HasError())
            return Return::Failure(draft.ErrorValue());
        const auto inverse = Math::TryInverseAffine(initial.Value());
        if (inverse.HasError())
            return Return::Failure(inverse.ErrorValue());
        const auto delta = Math::Multiply(Math::Multiply(primary.bodyToWorld, draft.Value()), inverse.Value());
        try {
            std::vector<ColliderTransformEdit> edits;
            edits.reserve(selection.size());
            for (const auto &child : selection) {
                const auto edited = TransformChild(child, delta, primaryDraft == primary.localTransform);
                if (edited.HasError())
                    return Return::Failure(edited.ErrorValue());
                edits.push_back(edited.Value());
            }
            return Return::Success(std::move(edits));
        } catch (const std::bad_alloc &) {
            return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
        }
    }

    /** @copydoc PrepareColliderTransformEdits */
    Result<std::vector<SceneObjectReplacement>> PrepareColliderTransformEdits(const SceneTransactionSnapshot &snapshot,
                                                                              const std::span<const ColliderTransformEdit> edits,
                                                                              const SceneTransactionLimits limits) {
        using Return = Result<std::vector<SceneObjectReplacement>>;
        if (edits.empty() || edits.size() > limits.maximumChanges || snapshot.objects.size() > limits.maximumSnapshotObjects ||
            edits.size() > limits.maximumPreparedBytes / sizeof(SceneObjectReplacement))
            return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
        try {
            std::vector<SceneObjectReplacement> changes;
            std::size_t remainingBytes = limits.maximumPreparedBytes;
            for (std::size_t index = 0; index < edits.size(); ++index) {
                const auto &edit = edits[index];
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (edits[prior].target == edit.target)
                        return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
                if (const auto valid = ValidateEdit(snapshot, edit); valid.HasError())
                    return Return::Failure(valid.ErrorValue());
                const auto object = std::ranges::find(snapshot.objects, edit.target.object, &SceneSource::SceneObjectSnapshot::id);
                // Prepare the entire object's children together so one object appears once in the atomic batch.
                if (std::ranges::any_of(changes, [&](const auto &change) {
                    return change.object == object->id;
                }))
                    continue;
                PhysicsInspectorValues values{object->components.rigidBody, object->components.colliders,
                                              object->components.physicsConstraints};
                for (auto &collider : values.colliders) {
                    const auto replacement = std::ranges::find_if(edits, [&](const auto &candidate) {
                        return candidate.target == ColliderEditTarget{object->id, collider.id};
                    });
                    if (replacement == edits.end())
                        continue;
                    collider.localPose = {replacement->transform.translation, replacement->transform.rotation};
                    collider.scale = replacement->transform.scale;
                }
                auto bounded = limits;
                bounded.maximumPreparedBytes = remainingBytes;
                auto prepared = PreparePhysicsInspectorEdit(snapshot, object->id, values, bounded);
                if (prepared.HasError())
                    return Return::Failure(prepared.ErrorValue());
                // Fingerprinting/owner preparation enforces nested storage ceilings again at Execute.
                const auto bytes =
                    sizeof(SceneObjectReplacement) + SceneDocumentDetail::EstimateSceneObjectOwnedMemoryBytes(*prepared.Value().desired);
                if (bytes > remainingBytes)
                    return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
                remainingBytes -= bytes;
                changes.push_back(std::move(prepared).Value());
            }
            return Return::Success(std::move(changes));
        } catch (const std::bad_alloc &) {
            return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
        }
    }
}  // namespace Horo::Editor
