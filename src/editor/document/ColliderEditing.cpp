#include "Horo/Editor/PhysicsInspector.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "editor/document/SceneDocument.h"
#include "editor/document/SceneDocumentInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>

namespace Horo::Editor {
    namespace {
        /** @brief Retain the canonical transaction rejection vocabulary. */
        Error Rejected(const SceneTransactionFailure failure) {
            return MakeError(SceneTransactionError(failure));
        }

        /** @brief Resolve the explicit body hierarchy without recursion or runtime/native state. */
        Result<Math::Mat4> BodyMatrix(const std::span<const SceneSource::SceneObjectSnapshot> objects,
                                      const Runtime::PhysicsBodyReference body) {
            Math::Mat4 matrix = Math::Mat4::Identity();
            std::optional<SceneSource::SceneObjectId> next{SceneSource::SceneObjectId{body.object.value}};
            for (std::size_t depth = 0; next && depth <= objects.size(); ++depth) {
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

        /** @brief Reject malformed detached geometry before projection or inverse-ray arithmetic. */
        bool ValidShape(const Runtime::PhysicsAnalyticCollider &shape) {
            if (const auto *box = std::get_if<Runtime::PhysicsBoxCollider>(&shape))
                return Math::IsFinite(box->halfExtentsMeters) && box->halfExtentsMeters.x > 0 && box->halfExtentsMeters.y > 0 &&
                       box->halfExtentsMeters.z > 0;
            if (const auto *sphere = std::get_if<Runtime::PhysicsSphereCollider>(&shape))
                return std::isfinite(sphere->radiusMeters) && sphere->radiusMeters > 0;
            if (const auto *capsule = std::get_if<Runtime::PhysicsCapsuleCollider>(&shape))
                return std::isfinite(capsule->radiusMeters) && capsule->radiusMeters > 0 &&
                       std::isfinite(capsule->cylindricalHalfHeightMeters) && capsule->cylindricalHalfHeightMeters > 0;
            const auto &plane = std::get<Runtime::PhysicsStaticPlaneCollider>(shape);
            return Math::IsFinite(plane.normal) && Math::NearlyEqual(Math::Dot(plane.normal, plane.normal), 1.0F) &&
                   std::isfinite(plane.signedDistanceMeters);
        }

        /** @brief Compose the same local geometry transform for display and intersection. */
        Result<Math::Mat4> ShapeMatrix(const ColliderPresentation &collider) {
            if (collider.diagnostic)
                return Result<Math::Mat4>::Failure(*collider.diagnostic);
            const auto local = collider.localTransform.TryToMatrix();
            if (local.HasError())
                return Result<Math::Mat4>::Failure(local.ErrorValue());
            if (!ValidShape(collider.shape) || !Math::IsFinite(collider.bodyToWorld) || collider.localTransform.scale.x <= 0 ||
                collider.localTransform.scale.y <= 0 || collider.localTransform.scale.z <= 0)
                return Result<Math::Mat4>::Failure(Rejected(SceneTransactionFailure::Invalid));
            const bool uniform = Math::NearlyEqual(collider.localTransform.scale.x, collider.localTransform.scale.y) &&
                                 Math::NearlyEqual(collider.localTransform.scale.y, collider.localTransform.scale.z);
            if (!uniform && (std::holds_alternative<Runtime::PhysicsSphereCollider>(collider.shape) ||
                             std::holds_alternative<Runtime::PhysicsCapsuleCollider>(collider.shape)))
                return Result<Math::Mat4>::Failure(
                    MakeError(Physics::PhysicsErrors::OperationUnsupported, "Sphere and capsule guides require uniform authored scale."));
            return Result<Math::Mat4>::Success(Math::Multiply(collider.bodyToWorld, local.Value()));
        }

        /** @brief Append bounded local geometry after the shared world transform. */
        void Segment(ColliderWireframe &wire, const Math::Mat4 &matrix, const Math::Vec3 first, const Math::Vec3 second) {
            if (wire.count < wire.segments.size())
                wire.segments[wire.count++] = {Math::TransformAffinePoint(matrix, first), Math::TransformAffinePoint(matrix, second)};
        }

        /** @brief Generate one deterministic circle in a local coordinate plane. */
        void Circle(ColliderWireframe &wire, const Math::Mat4 &matrix, const Math::Vec3 center, const Math::Vec3 u, const Math::Vec3 v,
                    const float radius) {
            constexpr int Count = 32;
            for (int index = 0; index < Count; ++index) {
                const float first = static_cast<float>(index) * 2 * std::numbers::pi_v<float> / Count;
                const float second = static_cast<float>(index + 1) * 2 * std::numbers::pi_v<float> / Count;
                Segment(wire, matrix, center + (u * std::cos(first) + v * std::sin(first)) * radius,
                        center + (u * std::cos(second) + v * std::sin(second)) * radius);
            }
        }

        /** @brief Draw the cylindrical seams and exterior hemispheres of a canonical capsule. */
        void CapsuleWire(ColliderWireframe &wire, const Math::Mat4 &matrix, const Runtime::PhysicsCapsuleCollider &capsule) {
            const float height = capsule.cylindricalHalfHeightMeters;
            Circle(wire, matrix, {0, height, 0}, {1, 0, 0}, {0, 0, 1}, capsule.radiusMeters);
            Circle(wire, matrix, {0, -height, 0}, {1, 0, 0}, {0, 0, 1}, capsule.radiusMeters);
            for (const Math::Vec3 axis : {Math::Vec3{1, 0, 0}, Math::Vec3{0, 0, 1}}) {
                for (int index = 0; index < 32; ++index) {
                    // Split at the equators so no segment connects through the capsule interior.
                    const float first = static_cast<float>(index) * 2 * std::numbers::pi_v<float> / 32;
                    const float second = static_cast<float>(index + 1) * 2 * std::numbers::pi_v<float> / 32;
                    const float y = index < 16 ? height : -height;
                    Segment(wire, matrix,
                            axis * (std::cos(first) * capsule.radiusMeters) + Math::Vec3{0, std::sin(first) * capsule.radiusMeters + y, 0},
                            axis * (std::cos(second) * capsule.radiusMeters) +
                                Math::Vec3{0, std::sin(second) * capsule.radiusMeters + y, 0});
                }
                for (const float side : {-1.0F, 1.0F})
                    Segment(wire, matrix, axis * (side * capsule.radiusMeters) + Math::Vec3{0, -height, 0},
                            axis * (side * capsule.radiusMeters) + Math::Vec3{0, height, 0});
            }
        }

        /** @brief Add a ray parameter only inside its requested world-distance interval. */
        void Candidate(std::optional<double> &nearest, const double distance, const Math::Ray &ray) {
            if (std::isfinite(distance) && distance >= ray.minimumDistance && distance <= ray.maximumDistance &&
                (!nearest || distance < *nearest))
                nearest = distance;
        }

        /** @brief Keep quadratic discriminants accurate for small colliders at large admitted world offsets. */
        double DotDouble(const Math::Vec3 first, const Math::Vec3 second) {
            return static_cast<double>(first.x) * second.x + static_cast<double>(first.y) * second.y +
                   static_cast<double>(first.z) * second.z;
        }

        /** @brief Intersect a sphere/hemisphere using an unnormalized local direction, preserving world t. */
        void SphereHit(std::optional<double> &nearest, const Math::Vec3 origin, const Math::Vec3 direction, const float radius,
                       const float centerY, const int hemisphere, const Math::Ray &ray) {
            const Math::Vec3 offset = origin - Math::Vec3{0, centerY, 0};
            const double a = DotDouble(direction, direction);
            const double b = DotDouble(offset, direction);
            const double c = DotDouble(offset, offset) - static_cast<double>(radius) * radius;
            const double discriminant = b * b - a * c;
            if (a <= 0 || discriminant < 0)
                return;
            for (const double distance : {(-b - std::sqrt(discriminant)) / a, (-b + std::sqrt(discriminant)) / a}) {
                const double y = origin.y + direction.y * distance - centerY;
                if (hemisphere == 0 || (hemisphere > 0 && y >= 0) || (hemisphere < 0 && y <= 0))
                    Candidate(nearest, distance, ray);
            }
        }

        /** @brief Intersect the finite cylinder and exterior hemispheres of a canonical +Y capsule. */
        std::optional<double> CapsuleHit(const Math::Vec3 origin, const Math::Vec3 direction,
                                         const Runtime::PhysicsCapsuleCollider &capsule, const Math::Ray &ray) {
            std::optional<double> nearest;
            const double a = static_cast<double>(direction.x) * direction.x + static_cast<double>(direction.z) * direction.z;
            const double b = static_cast<double>(origin.x) * direction.x + static_cast<double>(origin.z) * direction.z;
            const double c = static_cast<double>(origin.x) * origin.x + static_cast<double>(origin.z) * origin.z -
                             static_cast<double>(capsule.radiusMeters) * capsule.radiusMeters;
            const double discriminant = b * b - a * c;
            if (a > 0 && discriminant >= 0) {
                for (const double distance : {(-b - std::sqrt(discriminant)) / a, (-b + std::sqrt(discriminant)) / a}) {
                    if (const double y = origin.y + direction.y * distance; std::abs(y) <= capsule.cylindricalHalfHeightMeters)
                        Candidate(nearest, distance, ray);
                }
            }
            SphereHit(nearest, origin, direction, capsule.radiusMeters, capsule.cylindricalHalfHeightMeters, 1, ray);
            SphereHit(nearest, origin, direction, capsule.radiusMeters, -capsule.cylindricalHalfHeightMeters, -1, ray);
            return nearest;
        }

        /** @brief Use slab boundaries rather than a conservative world AABB for oriented boxes. */
        std::optional<double> BoxHit(const Math::Vec3 origin, const Math::Vec3 direction, const Math::Vec3 extent, const Math::Ray &ray) {
            double enter = -std::numeric_limits<double>::infinity();
            double leave = std::numeric_limits<double>::infinity();
            const std::array origins{origin.x, origin.y, origin.z};
            const std::array directions{direction.x, direction.y, direction.z};
            const std::array extents{extent.x, extent.y, extent.z};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (directions[axis] == 0) {
                    if (std::abs(origins[axis]) > extents[axis])
                        return std::nullopt;
                    continue;
                }
                const double first = (-static_cast<double>(extents[axis]) - origins[axis]) / directions[axis];
                const double second = (static_cast<double>(extents[axis]) - origins[axis]) / directions[axis];
                enter = std::max(enter, std::min(first, second));
                leave = std::min(leave, std::max(first, second));
                if (enter > leave)
                    return std::nullopt;
            }
            std::optional<double> nearest;
            Candidate(nearest, enter, ray);
            Candidate(nearest, leave, ray);
            return nearest;
        }

        /** @brief Intersect supported local geometry without normalizing the inverse-transformed ray direction. */
        std::optional<double> Hit(const ColliderPresentation &collider, const Math::Ray &ray) {
            const auto matrix = ShapeMatrix(collider);
            if (matrix.HasError())
                return std::nullopt;
            const auto inverse = Math::TryInverseAffine(matrix.Value());
            if (inverse.HasError())
                return std::nullopt;
            const Math::Vec3 origin = Math::TransformAffinePoint(inverse.Value(), ray.origin);
            const Math::Vec3 direction = Math::TransformDirection(inverse.Value(), ray.direction);
            if (const auto *box = std::get_if<Runtime::PhysicsBoxCollider>(&collider.shape))
                return BoxHit(origin, direction, box->halfExtentsMeters, ray);
            std::optional<double> nearest;
            if (const auto *sphere = std::get_if<Runtime::PhysicsSphereCollider>(&collider.shape)) {
                SphereHit(nearest, origin, direction, sphere->radiusMeters, 0, 0, ray);
            } else if (const auto *capsule = std::get_if<Runtime::PhysicsCapsuleCollider>(&collider.shape)) {
                return CapsuleHit(origin, direction, *capsule, ray);
            } else {
                const auto &plane = std::get<Runtime::PhysicsStaticPlaneCollider>(collider.shape);
                if (const double denominator = DotDouble(plane.normal, direction); denominator != 0)
                    Candidate(nearest, (plane.signedDistanceMeters - DotDouble(plane.normal, origin)) / denominator, ray);
            }
            return nearest;
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
            const Math::Vec3 axis{rotation.Value().x, rotation.Value().y, rotation.Value().z};
            if (Math::LengthSquared(axis) > 1.0e-12F) {
                const float angle = 2 * std::atan2(Math::Length(axis), rotation.Value().w);
                const auto snapped = Math::Quaternion::TryFromAxisAngle(axis, quantize(angle, Math::DegreesToRadians(15)));
                if (snapped.HasError())
                    return Result<Math::Transform>::Failure(snapped.ErrorValue());
                draft.rotation = snapped.Value() * primary.localTransform.rotation;
            }
            const auto scale = [&](const float value, const float original) {
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
            const auto object = std::ranges::find(snapshot.objects, edit.target.object, &SceneSource::SceneObjectSnapshot::id);
            const auto state = ResolveSceneObjectEditorState(snapshot.objects, edit.target.object);
            if (object == snapshot.objects.end() || object->generated || !state || state->effectivelyLocked)
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Unauthorized));
            const auto child = std::ranges::find(object->components.colliders, edit.target.component, &Runtime::ColliderComponent::id);
            if (child == object->components.colliders.end() || child->generation != edit.generation)
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Stale));
            if (!std::holds_alternative<Runtime::PhysicsAnalyticCollider>(child->source))
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::OperationUnsupported,
                                                       "Cooked collider source editing requires its owning shape document."));
            const auto bodyState = ResolveSceneObjectEditorState(snapshot.objects, {child->body.object.value});
            if (!bodyState || bodyState->effectivelyLocked)
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Unauthorized));
            if (const auto world = BodyMatrix(snapshot.objects, child->body); world.HasError())
                return Result<void>::Failure(world.ErrorValue());
            if (edit.transform.TryToMatrix().HasError())
                return Result<void>::Failure(Rejected(SceneTransactionFailure::Invalid));
            return Result<void>::Success();
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
                    value.editable = value.editable && !value.diagnostic;
                    result.push_back(std::move(value));
                }
            }
            return Return::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Return::Failure(Rejected(SceneTransactionFailure::Capacity));
        }
    }

    /** @copydoc BuildColliderWireframe */
    Result<ColliderWireframe> BuildColliderWireframe(const ColliderPresentation &collider) {
        using Return = Result<ColliderWireframe>;
        const auto matrix = ShapeMatrix(collider);
        if (matrix.HasError())
            return Return::Failure(matrix.ErrorValue());
        ColliderWireframe wire;
        if (const auto *box = std::get_if<Runtime::PhysicsBoxCollider>(&collider.shape)) {
            std::array<Math::Vec3, 8> points{};
            for (std::size_t index = 0; index < points.size(); ++index)
                points[index] = {box->halfExtentsMeters.x * ((index & 1U) ? 1 : -1), box->halfExtentsMeters.y * ((index & 2U) ? 1 : -1),
                                 box->halfExtentsMeters.z * ((index & 4U) ? 1 : -1)};
            for (std::size_t index = 0; index < points.size(); ++index)
                for (const std::size_t bit : {1U, 2U, 4U})
                    if (!(index & bit))
                        Segment(wire, matrix.Value(), points[index], points[index | bit]);
        } else if (const auto *sphere = std::get_if<Runtime::PhysicsSphereCollider>(&collider.shape)) {
            Circle(wire, matrix.Value(), {}, {1, 0, 0}, {0, 1, 0}, sphere->radiusMeters);
            Circle(wire, matrix.Value(), {}, {1, 0, 0}, {0, 0, 1}, sphere->radiusMeters);
            Circle(wire, matrix.Value(), {}, {0, 1, 0}, {0, 0, 1}, sphere->radiusMeters);
        } else if (const auto *capsule = std::get_if<Runtime::PhysicsCapsuleCollider>(&collider.shape)) {
            CapsuleWire(wire, matrix.Value(), *capsule);
        } else {
            const auto &plane = std::get<Runtime::PhysicsStaticPlaneCollider>(collider.shape);
            const auto tangent =
                Math::TryNormalize(Math::Cross(plane.normal, std::abs(plane.normal.y) < 0.9F ? Math::Vec3{0, 1, 0} : Math::Vec3{1, 0, 0}));
            if (tangent.HasError())
                return Return::Failure(tangent.ErrorValue());
            const Math::Vec3 u = tangent.Value(), v = Math::Cross(plane.normal, u), center = plane.normal * plane.signedDistanceMeters;
            for (int index = -5; index <= 5; ++index) {
                Segment(wire, matrix.Value(), center + u * static_cast<float>(index) - v * 5,
                        center + u * static_cast<float>(index) + v * 5);
                Segment(wire, matrix.Value(), center + v * static_cast<float>(index) - u * 5,
                        center + v * static_cast<float>(index) + u * 5);
            }
        }
        for (std::size_t index = 0; index < wire.count; ++index)
            if (!Math::IsFinite(wire.segments[index].first) || !Math::IsFinite(wire.segments[index].second))
                return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
        return Return::Success(wire);
    }

    /** @copydoc PickPhysicsCollider */
    Result<std::optional<ColliderEditTarget>> PickPhysicsCollider(const std::span<const ColliderPresentation> colliders,
                                                                  const Math::Ray &ray) {
        using Return = Result<std::optional<ColliderEditTarget>>;
        if (Math::TryMakeRay(ray.origin, ray.direction, ray.minimumDistance, ray.maximumDistance).HasError() ||
            !Math::NearlyEqual(Math::Dot(ray.direction, ray.direction), 1.0F))
            return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
        std::optional<ColliderEditTarget> target;
        std::optional<double> nearest;
        for (const auto &collider : colliders) {
            if (!collider.visible || collider.diagnostic)
                continue;
            if (!collider.target.object.IsValid() || !collider.target.component.IsValid() || collider.generation == 0)
                return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
            const auto distance = Hit(collider, ray);
            if (distance && (!nearest || *distance < *nearest || (*distance == *nearest && collider.target < *target))) {
                nearest = distance;
                target = collider.target;
            }
        }
        return Return::Success(target);
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
