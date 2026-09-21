#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsSceneActivationInternal.h"

#include <algorithm>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Physics::Detail {
    namespace {
        struct ColliderReference final {
            Runtime::SceneObjectId object;
            const Runtime::ColliderComponent *component{};
        };

        struct ConstraintReference final {
            Runtime::SceneObjectId object;
            const Runtime::PhysicsConstraintComponent *component{};
        };

        [[nodiscard]] AuthoredBodyKey Key(const Runtime::PhysicsBodyReference reference) noexcept {
            return {reference.object.value, reference.body.value};
        }

        [[nodiscard]] AuthoredBodyKey Key(const Runtime::SceneObjectId object, const Runtime::PhysicsBodySlotId body) noexcept {
            return {object.value, body.value};
        }

        template <typename T>
        [[nodiscard]] Result<T> ContextFailure(const ErrorCodeDescriptor &code, const std::string_view stage,
                                               const Runtime::SceneObjectId object, const std::uint64_t component,
                                               const std::optional<Assets::AssetId> asset, std::string message) {
            return Result<T>::Failure(AddActivationContext(MakeError(code, std::move(message)), stage, object, component, asset));
        }

        [[nodiscard]] const Runtime::SceneAssetDependency *FindDependency(const Runtime::RuntimeSceneDefinition &definition,
                                                                          const Assets::AssetId asset) noexcept {
            const auto found = std::ranges::find(definition.AssetDependencies(), asset, [](const auto &dependency) {
                return dependency.id;
            });
            return found == definition.AssetDependencies().end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] Result<Runtime::RuntimeSceneAssetView> ResolveAsset(const Runtime::RuntimeSceneDefinition &definition,
                                                                          const Runtime::RuntimeSceneView scene,
                                                                          const Assets::AssetId asset, const ErrorCodeDescriptor &errorCode,
                                                                          const std::string_view stage, const Runtime::SceneObjectId object,
                                                                          const std::uint64_t component) {
            const Runtime::SceneAssetDependency *dependency = FindDependency(definition, asset);
            if (dependency == nullptr)
                return ContextFailure<Runtime::RuntimeSceneAssetView>(errorCode, stage, object, component, asset,
                                                                      "The authored asset is not declared as a scene dependency.");
            const std::optional<Runtime::RuntimeSceneAssetView> resolved = scene.FindAsset(asset);
            if (!resolved.has_value() || resolved->bytes.empty())
                return ContextFailure<Runtime::RuntimeSceneAssetView>(errorCode, stage, object, component, asset,
                                                                      "The exact scene asset payload is missing or empty.");
            if (resolved->type == nullptr || *resolved->type != dependency->expectedType)
                return ContextFailure<
                    Runtime::RuntimeSceneAssetView>(errorCode, stage, object, component, asset,
                                                    "The resolved scene asset type does not match the authored dependency.");
            return Result<Runtime::RuntimeSceneAssetView>::Success(*resolved);
        }

        [[nodiscard]] Result<Math::Transform> ResolveWorldTransform(const Runtime::RuntimeSceneView scene, std::size_t slot,
                                                                    std::vector<std::uint8_t> &states,
                                                                    std::vector<std::optional<Math::Transform>> &cache);

        [[nodiscard]] Result<Math::Mat4> ComposeWorldMatrix(const Runtime::RuntimeSceneView scene, const Runtime::RuntimeEntityView &entity,
                                                            const Math::Mat4 &localMatrix, std::vector<std::uint8_t> &states,
                                                            std::vector<std::optional<Math::Transform>> &cache) {
            if (!entity.parent.has_value())
                return Result<Math::Mat4>::Success(localMatrix);
            const Result<Math::Transform> parent = ResolveWorldTransform(scene, entity.parent->entity.index, states, cache);
            if (parent.HasError())
                return Result<Math::Mat4>::Failure(parent.ErrorValue());
            const Result<Math::Mat4> parentMatrix = parent.Value().TryToMatrix();
            if (parentMatrix.HasError())
                return Result<Math::Mat4>::Failure(parentMatrix.ErrorValue());
            return Result<Math::Mat4>::Success(Math::Multiply(parentMatrix.Value(), localMatrix));
        }

        [[nodiscard]] Result<Math::Transform> ResolveWorldTransform(const Runtime::RuntimeSceneView scene, const std::size_t slot,
                                                                    std::vector<std::uint8_t> &states,
                                                                    std::vector<std::optional<Math::Transform>> &cache) {
            if (slot >= states.size())
                return Result<Math::Transform>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Physics body entity slot is outside the scene view."));
            if (states[slot] == 2)
                return Result<Math::Transform>::Success(*cache[slot]);
            if (states[slot] == 1)
                return Result<Math::Transform>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Physics scene hierarchy contains a parent cycle."));
            const std::optional<Runtime::RuntimeEntityView> entity = scene.EntityAt(slot);
            if (!entity.has_value() || entity->localTransform == nullptr)
                return Result<Math::Transform>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Physics body entity is inactive or has no local transform."));

            states[slot] = 1;
            const Result<Math::Mat4> local = entity->localTransform->TryToMatrix();
            if (local.HasError()) {
                states[slot] = 0;
                return Result<Math::Transform>::Failure(local.ErrorValue());
            }
            const Result<Math::Mat4> worldMatrix = ComposeWorldMatrix(scene, *entity, local.Value(), states, cache);
            if (worldMatrix.HasError()) {
                states[slot] = 0;
                return Result<Math::Transform>::Failure(worldMatrix.ErrorValue());
            }
            const Result<Math::Transform> decomposed = Math::TryDecomposeAffineTRS(worldMatrix.Value());
            if (decomposed.HasError()) {
                states[slot] = 0;
                return decomposed;
            }
            cache[slot] = decomposed.Value();
            states[slot] = 2;
            return decomposed;
        }

        [[nodiscard]] Result<PhysicsPose> ToPhysicsPose(const Runtime::AuthoredPhysicsPose &pose) {
            PhysicsPose result{pose.translation, pose.rotation};
            if (const Result<void> valid = ValidatePhysicsPose(result); valid.HasError())
                return Result<PhysicsPose>::Failure(valid.ErrorValue());
            return Result<PhysicsPose>::Success(result);
        }

        [[nodiscard]] Result<PhysicsPose> ToBodyPhysicsPose(const Math::Transform &transform) {
            if (!Math::NearlyEqual(transform.scale, Math::Vec3{1.0F, 1.0F, 1.0F}))
                return Result<PhysicsPose>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Physics body transforms require unit world scale."));
            PhysicsPose result{transform.translation, transform.rotation};
            if (const Result<void> valid = ValidatePhysicsPose(result); valid.HasError())
                return Result<PhysicsPose>::Failure(valid.ErrorValue());
            return Result<PhysicsPose>::Success(result);
        }

        [[nodiscard]] PhysicsMassPolicy ToPhysicsMassPolicy(const Runtime::AuthoredPhysicsMassPolicy &mass) {
            return std::visit([]<typename Mass>(const Mass &value) -> PhysicsMassPolicy {
                using MassType = std::decay_t<Mass>;
                if constexpr (std::is_same_v<MassType, Runtime::AuthoredPhysicsNoMass>)
                    return PhysicsNoMass{};
                else if constexpr (std::is_same_v<MassType, Runtime::AuthoredPhysicsMass>)
                    return PhysicsMass{value.kilograms};
                else
                    return PhysicsDensity{value.kilogramsPerCubicMeter};
            }, mass);
        }

        [[nodiscard]] PhysicsShapeDescriptor ToPhysicsShape(const Runtime::PhysicsAnalyticCollider &source) {
            return std::visit([]<typename Shape>(const Shape &shape) -> PhysicsShapeDescriptor {
                using ShapeType = std::decay_t<Shape>;
                if constexpr (std::is_same_v<ShapeType, Runtime::PhysicsBoxCollider>)
                    return PhysicsBoxShape{shape.halfExtentsMeters};
                else if constexpr (std::is_same_v<ShapeType, Runtime::PhysicsSphereCollider>)
                    return PhysicsSphereShape{shape.radiusMeters};
                else if constexpr (std::is_same_v<ShapeType, Runtime::PhysicsCapsuleCollider>)
                    return PhysicsCapsuleShape{shape.radiusMeters, shape.cylindricalHalfHeightMeters};
                else
                    return PhysicsStaticPlaneShape{shape.normal, shape.signedDistanceMeters};
            }, source);
        }

        [[nodiscard]] const ErrorCodeDescriptor &PlanErrorCode(const Error &error) noexcept {
            return error.code.Value() == PhysicsErrors::OperationUnsupported.code.Value() ? PhysicsErrors::OperationUnsupported
                                                                                          : PhysicsErrors::DescriptorInvalid;
        }

        template <typename T>
        [[nodiscard]] Result<T> ContextFailureFromPlanError(const Error &error, const std::string_view stage,
                                                            const Runtime::SceneObjectId object, const std::uint64_t component,
                                                            const std::optional<Assets::AssetId> asset) {
            return ContextFailure<T>(PlanErrorCode(error), stage, object, component, asset, error.message);
        }

        [[nodiscard]] Result<std::vector<Runtime::PhysicsSceneComponentView>> BuildPhysicsComponentViews(
            const Runtime::RuntimeSceneDefinition &definition) {
            std::vector<Runtime::PhysicsSceneComponentView> views;
            views.reserve(definition.Entities().size());
            for (const Runtime::RuntimeEntityDefinition &entity : definition.Entities())
                views.emplace_back(
                    Runtime::PhysicsSceneComponentView{.owner = entity.object,
                                                       .rigidBody = entity.components.rigidBody ? &*entity.components.rigidBody : nullptr,
                                                       .colliders = entity.components.colliders,
                                                       .constraints = entity.components.physicsConstraints});
            if (const Result<void> valid = Runtime::ValidatePhysicsSceneComponentViews(views); valid.HasError())
                return Result<std::vector<Runtime::PhysicsSceneComponentView>>::Failure(valid.ErrorValue());
            return Result<std::vector<Runtime::PhysicsSceneComponentView>>::Success(std::move(views));
        }

        [[nodiscard]] Result<PlannedBody> BuildPlannedBody(const Runtime::PhysicsSceneComponentView &view,
                                                           const Runtime::RuntimeSceneView scene,
                                                           std::vector<std::uint8_t> &transformStates,
                                                           std::vector<std::optional<Math::Transform>> &transformCache) {
            const Runtime::RigidBodyComponent &component = *view.rigidBody;
            const std::optional<Runtime::EntityRef> entity = scene.Find(view.owner);
            if (!entity.has_value())
                return ContextFailure<PlannedBody>(PhysicsErrors::DescriptorInvalid, "body", view.owner, component.id.value, std::nullopt,
                                                   "The authored body object is not present in the runtime scene.");
            const Result<Math::Transform> worldTransform =
                ResolveWorldTransform(scene, entity->entity.index, transformStates, transformCache);
            if (worldTransform.HasError())
                return ContextFailureFromPlanError<PlannedBody>(worldTransform.ErrorValue(), "body", view.owner, component.id.value,
                                                                std::nullopt);
            const Result<PhysicsPose> pose = ToBodyPhysicsPose(worldTransform.Value());
            if (pose.HasError())
                return ContextFailureFromPlanError<PlannedBody>(pose.ErrorValue(), "body", view.owner, component.id.value, std::nullopt);
            const PhysicsAuthoredBodyDescriptor authored{.motion = static_cast<PhysicsMotionType>(component.motion),
                                                         .mass = ToPhysicsMassPolicy(component.mass),
                                                         .initialLinearVelocity = component.initialLinearVelocity,
                                                         .initialAngularVelocity = component.initialAngularVelocity,
                                                         .motionSafety = {.linearDampingPerSecond = component.linearDampingPerSecond,
                                                                          .angularDampingPerSecond = component.angularDampingPerSecond,
                                                                          .lockedAxes = PhysicsAxisLock::None,
                                                                          .maximumLinearSpeed = component.maximumLinearSpeed,
                                                                          .maximumAngularSpeed = component.maximumAngularSpeed,
                                                                          .maximumDepenetrationSpeed = MaximumPhysicsDepenetrationSpeed}};
            if (const Result<void> valid = ValidatePhysicsAuthoredBodyDescriptor(authored); valid.HasError())
                return ContextFailureFromPlanError<PlannedBody>(valid.ErrorValue(), "body", view.owner, component.id.value, std::nullopt);
            return Result<PlannedBody>::Success(PlannedBody{.object = view.owner,
                                                            .component = component.id,
                                                            .slot = component.body,
                                                            .pose = pose.Value(),
                                                            .authored = authored});
        }

        [[nodiscard]] Result<std::vector<PlannedBody>> BuildPlannedBodies(const std::vector<Runtime::PhysicsSceneComponentView> &views,
                                                                          const Runtime::RuntimeSceneView scene) {
            std::vector<PlannedBody> bodies;
            std::vector<std::uint8_t> transformStates(scene.SlotCount());
            std::vector<std::optional<Math::Transform>> transformCache(scene.SlotCount());
            for (const Runtime::PhysicsSceneComponentView &view : views) {
                if (view.rigidBody == nullptr || !view.rigidBody->enabled)
                    continue;
                Result<PlannedBody> body = BuildPlannedBody(view, scene, transformStates, transformCache);
                if (body.HasError())
                    return Result<std::vector<PlannedBody>>::Failure(body.ErrorValue());
                bodies.emplace_back(std::move(body).Value());
            }
            std::ranges::sort(bodies, [](const PlannedBody &left, const PlannedBody &right) {
                return std::tie(left.object.value, left.slot.value) < std::tie(right.object.value, right.slot.value);
            });
            return Result<std::vector<PlannedBody>>::Success(std::move(bodies));
        }

        [[nodiscard]] PlannedBody *FindPlannedBody(std::vector<PlannedBody> &bodies, const Runtime::PhysicsBodyReference reference) {
            const auto found = std::ranges::find_if(bodies, [reference](const PlannedBody &body) {
                return Key(body.object, body.slot) == Key(reference);
            });
            return found == bodies.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] std::optional<std::size_t> FindPlannedBodyIndex(const std::vector<PlannedBody> &bodies,
                                                                      const Runtime::PhysicsBodyReference reference) {
            const auto found = std::ranges::find_if(bodies, [reference](const PlannedBody &body) {
                return Key(body.object, body.slot) == Key(reference);
            });
            if (found == bodies.end())
                return std::nullopt;
            return static_cast<std::size_t>(std::distance(bodies.begin(), found));
        }

        [[nodiscard]] Result<PlannedCollider> BuildPlannedCollider(const Runtime::RuntimeSceneDefinition &definition,
                                                                   const Runtime::RuntimeSceneView scene,
                                                                   const ColliderReference &reference) {
            const Runtime::ColliderComponent &collider = *reference.component;
            for (const Runtime::PhysicsColliderMaterialBinding &material : collider.materials) {
                if (const Result<Runtime::RuntimeSceneAssetView> resolved =
                        ResolveAsset(definition, scene, material.material, PhysicsErrors::MaterialDescriptorInvalid, "material",
                                     reference.object, collider.id.value);
                    resolved.HasError())
                    return Result<PlannedCollider>::Failure(resolved.ErrorValue());
            }

            PhysicsShapeDescriptor geometry;
            if (const auto *analytic = std::get_if<Runtime::PhysicsAnalyticCollider>(&collider.source)) {
                geometry = ToPhysicsShape(*analytic);
            } else {
                const Runtime::PhysicsShapeAssetReference &asset = std::get<Runtime::PhysicsShapeAssetReference>(collider.source);
                const Result<Runtime::RuntimeSceneAssetView> resolved =
                    ResolveAsset(definition, scene, asset.asset, PhysicsErrors::ShapeArtifactInvalid, "shape", reference.object,
                                 collider.id.value);
                if (resolved.HasError())
                    return Result<PlannedCollider>::Failure(resolved.ErrorValue());
                return ContextFailure<PlannedCollider>(PhysicsErrors::OperationUnsupported, "shape", reference.object, collider.id.value,
                                                       asset.asset,
                                                       "The exact cooked shape payload has no qualified CanonicalV1 realization path; "
                                                       "runtime cooking and fallback are forbidden.");
            }
            const Result<PhysicsPose> localPose = ToPhysicsPose(collider.localPose);
            if (localPose.HasError())
                return ContextFailure<PlannedCollider>(PhysicsErrors::DescriptorInvalid, "collider", reference.object, collider.id.value,
                                                       std::nullopt, localPose.ErrorValue().message);
            Result<ResolvedPhysicsPrimitiveShape> resolved =
                ResolvePhysicsPrimitiveShape({.geometry = std::move(geometry), .localPose = localPose.Value(), .scale = {collider.scale}});
            if (resolved.HasError())
                return ContextFailureFromPlanError<PlannedCollider>(resolved.ErrorValue(), "collider", reference.object, collider.id.value,
                                                                    std::nullopt);
            ResolvedPhysicsPrimitiveShape resolvedShape = std::move(resolved).Value();
            return Result<PlannedCollider>::Success(PlannedCollider{.object = reference.object,
                                                                    .component = collider.id,
                                                                    .collider = collider.collider,
                                                                    .geometry = std::move(resolvedShape.geometry),
                                                                    .localPose = resolvedShape.localPose,
                                                                    .sensor = collider.sensor});
        }

        [[nodiscard]] std::vector<ColliderReference> CollectEnabledColliders(const std::vector<Runtime::PhysicsSceneComponentView> &views) {
            std::vector<ColliderReference> colliders;
            for (const Runtime::PhysicsSceneComponentView &view : views)
                for (const Runtime::ColliderComponent &collider : view.colliders)
                    if (collider.enabled)
                        colliders.emplace_back(ColliderReference{view.owner, &collider});
            std::ranges::sort(colliders, [](const ColliderReference &left, const ColliderReference &right) {
                return std::tie(left.object.value, left.component->collider.value) <
                       std::tie(right.object.value, right.component->collider.value);
            });
            return colliders;
        }

        [[nodiscard]] Result<void> AppendPlannedCollider(const Runtime::RuntimeSceneDefinition &definition,
                                                         const Runtime::RuntimeSceneView scene, const ColliderReference &reference,
                                                         std::vector<PlannedBody> &bodies) {
            const Runtime::ColliderComponent &collider = *reference.component;
            PlannedBody *body = FindPlannedBody(bodies, collider.body);
            if (body == nullptr)
                return ContextFailure<void>(PhysicsErrors::DescriptorInvalid, "collider", reference.object, collider.id.value, std::nullopt,
                                            "The enabled collider references a disabled or absent authored body.");
            Result<PlannedCollider> planned = BuildPlannedCollider(definition, scene, reference);
            if (planned.HasError())
                return Result<void>::Failure(planned.ErrorValue());
            if (!body->colliders.empty() && body->sensor != planned.Value().sensor)
                return ContextFailure<void>(PhysicsErrors::OperationUnsupported, "collider", reference.object, collider.id.value,
                                            std::nullopt, "A native scene body cannot mix sensor and solid collider contributors.");
            body->sensor = planned.Value().sensor;
            body->colliders.emplace_back(std::move(planned).Value());
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendPlannedColliders(const Runtime::RuntimeSceneDefinition &definition,
                                                          const Runtime::RuntimeSceneView scene,
                                                          const std::vector<Runtime::PhysicsSceneComponentView> &views,
                                                          std::vector<PlannedBody> &bodies) {
            const std::vector<ColliderReference> colliders = CollectEnabledColliders(views);
            for (const ColliderReference &reference : colliders) {
                if (const Result<void> appended = AppendPlannedCollider(definition, scene, reference, bodies); appended.HasError())
                    return appended;
            }
            for (const PlannedBody &body : bodies)
                if (body.colliders.empty())
                    return ContextFailure<void>(PhysicsErrors::DescriptorInvalid, "body", body.object, body.component.value, std::nullopt,
                                                "The enabled authored body has no enabled collider contributor.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendPlannedConstraints(const std::vector<Runtime::PhysicsSceneComponentView> &views,
                                                            const std::vector<PlannedBody> &bodies,
                                                            std::vector<PlannedConstraint> &constraints) {
            std::vector<ConstraintReference> references;
            for (const Runtime::PhysicsSceneComponentView &view : views)
                for (const Runtime::PhysicsConstraintComponent &constraint : view.constraints)
                    if (constraint.enabled)
                        references.emplace_back(ConstraintReference{view.owner, &constraint});
            std::ranges::sort(references, [](const ConstraintReference &left, const ConstraintReference &right) {
                return std::tie(left.object.value, left.component->constraint.value) <
                       std::tie(right.object.value, right.component->constraint.value);
            });
            for (const ConstraintReference &reference : references) {
                const Runtime::PhysicsConstraintComponent &constraint = *reference.component;
                const std::optional<std::size_t> first = FindPlannedBodyIndex(bodies, constraint.first.body);
                if (!first.has_value())
                    return ContextFailure<void>(PhysicsErrors::DescriptorInvalid, "constraint", reference.object, constraint.id.value,
                                                std::nullopt, "The enabled constraint first endpoint has no staged authored body.");
                std::optional<std::size_t> second;
                if (const auto *body = std::get_if<Runtime::PhysicsConstraintBodyEndpoint>(&constraint.second)) {
                    second = FindPlannedBodyIndex(bodies, body->body);
                    if (!second.has_value())
                        return ContextFailure<void>(PhysicsErrors::DescriptorInvalid, "constraint", reference.object, constraint.id.value,
                                                    std::nullopt, "The enabled constraint second endpoint has no staged authored body.");
                }
                constraints.emplace_back(PlannedConstraint{.object = reference.object,
                                                           .component = constraint.id,
                                                           .slot = constraint.constraint,
                                                           .authored = constraint,
                                                           .firstBody = *first,
                                                           .secondBody = second});
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc BuildPhysicsScenePlan */
    Result<PhysicsScenePlan> BuildPhysicsScenePlan(const Runtime::RuntimeSceneDefinition &definition,
                                                   const Runtime::RuntimeSceneView scene) {
        const Result<std::vector<Runtime::PhysicsSceneComponentView>> views = BuildPhysicsComponentViews(definition);
        if (views.HasError())
            return Result<PhysicsScenePlan>::Failure(views.ErrorValue());
        Result<std::vector<PlannedBody>> bodies = BuildPlannedBodies(views.Value(), scene);
        if (bodies.HasError())
            return Result<PhysicsScenePlan>::Failure(bodies.ErrorValue());
        PhysicsScenePlan plan;
        plan.bodies = std::move(bodies).Value();
        if (const Result<void> colliders = AppendPlannedColliders(definition, scene, views.Value(), plan.bodies); colliders.HasError())
            return Result<PhysicsScenePlan>::Failure(colliders.ErrorValue());
        if (const Result<void> constraints = AppendPlannedConstraints(views.Value(), plan.bodies, plan.constraints); constraints.HasError())
            return Result<PhysicsScenePlan>::Failure(constraints.ErrorValue());
        return Result<PhysicsScenePlan>::Success(std::move(plan));
    }
}  // namespace Horo::Physics::Detail
