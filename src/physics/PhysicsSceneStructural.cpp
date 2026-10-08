#include "Horo/Assets/AssetCook.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsSceneActivationInternal.h"

#include <algorithm>
#include <limits>
#include <new>
#include <ranges>
#include <type_traits>

namespace Horo::Physics::Detail {
    namespace {
        template <typename T> Result<T> Failure(const char *message) {
            return Result<T>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, message));
        }

        /** @brief Resolves only Scene-issued runtime fixups, or genuine authored references in the active Scene. */
        Result<Runtime::EntityRef> ResolveTarget(const Runtime::RuntimeEntityView &entity, const Runtime::GroupPhysicsReferenceKind kind,
                                                 const std::size_t occurrence, const Runtime::PhysicsBodyReference authored,
                                                 const Runtime::RuntimeSceneView active) {
            for (const auto &reference : entity.physicsReferences)
                if (reference.kind == kind && reference.component == occurrence && reference.body == authored.body)
                    return Result<Runtime::EntityRef>::Success(reference.target);
            if (const auto target = active.Find(authored.object))
                return Result<Runtime::EntityRef>::Success(*target);
            return Failure<Runtime::EntityRef>("A structural Physics reference has no exact Scene-issued runtime binding.");
        }

        /** @brief Composes candidate and resident parents without consulting authored IDs or creating a temporary Scene. */
        Result<Math::Transform> WorldTransform(Runtime::RuntimeEntityView entity, const Runtime::RuntimeSceneView active,
                                               const std::span<const Runtime::RuntimeEntityView> created) {
            if (!entity.localTransform)
                return Failure<Math::Transform>("A structural body has no local transform.");
            auto matrix = entity.localTransform->TryToMatrix();
            if (matrix.HasError())
                return Result<Math::Transform>::Failure(matrix.ErrorValue());
            std::size_t remaining = active.SlotCount() + created.size();
            while (entity.parent) {
                if (remaining-- == 0)
                    return Failure<Math::Transform>("A structural Physics hierarchy contains a cycle.");
                if (const auto found = std::ranges::find(created, *entity.parent, &Runtime::RuntimeEntityView::entity);
                    found != created.end()) {
                    entity = *found;
                } else {
                    auto parent = active.Get(*entity.parent);
                    if (parent.HasError())
                        return Result<Math::Transform>::Failure(parent.ErrorValue());
                    entity = parent.Value();
                }
                if (!entity.localTransform)
                    return Failure<Math::Transform>("A structural body parent has no transform.");
                auto parentMatrix = entity.localTransform->TryToMatrix();
                if (parentMatrix.HasError())
                    return Result<Math::Transform>::Failure(parentMatrix.ErrorValue());
                matrix = Result<Math::Mat4>::Success(Math::Multiply(parentMatrix.Value(), matrix.Value()));
            }
            return Math::TryDecomposeAffineTRS(matrix.Value());
        }

        /** @brief Preserves the closed analytic geometry vocabulary; native types never enter Scene projections. */
        PhysicsShapeDescriptor AnalyticGeometry(const Runtime::PhysicsAnalyticCollider &geometry) {
            return std::visit([]<typename T>(const T &shape) -> PhysicsShapeDescriptor {
                if constexpr (std::is_same_v<T, Runtime::PhysicsBoxCollider>)
                    return PhysicsBoxShape{shape.halfExtentsMeters};
                else if constexpr (std::is_same_v<T, Runtime::PhysicsSphereCollider>)
                    return PhysicsSphereShape{shape.radiusMeters};
                else if constexpr (std::is_same_v<T, Runtime::PhysicsCapsuleCollider>)
                    return PhysicsCapsuleShape{shape.radiusMeters, shape.cylindricalHalfHeightMeters};
                else
                    return PhysicsStaticPlaneShape{shape.normal, shape.signedDistanceMeters};
            }, geometry);
        }

        /** @brief Uses only the admitted named closure, preserving activation's exact dependency policy. */
        Result<void> ValidateAsset(const Runtime::RuntimeEntityView &entity, const Runtime::RuntimeSceneView active,
                                   const Assets::AssetId asset, const ErrorCodeDescriptor &code) {
            for (const auto &resource : entity.groupAssets) {
                if (resource.metadata.id != asset)
                    continue;
                const auto bytes = resource.artifact.Bytes();
                auto decoded = Assets::DecodeCookedArtifactBytes(bytes);
                if (decoded.HasError())
                    return Result<void>::Failure(decoded.ErrorValue());
                if (decoded.Value().id != asset || decoded.Value().type != resource.metadata.expectedType ||
                    decoded.Value().payload.empty())
                    return Result<void>::Failure(MakeError(code, "Structural resource does not match its admitted identity/type."));
                return Result<void>::Success();
            }
            if (const auto resolved = active.FindAsset(asset); !resolved || !resolved->type || resolved->bytes.empty())
                return Result<void>::Failure(MakeError(code, "Required structural resource is absent from the admitted closure."));
            return Result<void>::Success();
        }

        /** @brief Complete native additions and generation-qualified tables, published together without allocation. */
        class StructuralCandidate final : public Runtime::SceneStructuralCandidate {
        public:
            std::shared_ptr<PhysicsStructuralState> state;
            Runtime::RuntimeSceneView scene;
            std::uint64_t revision{};
            std::unique_ptr<PhysicsSceneBodyPreparation> native;
            std::vector<StructuralBodyBinding> bindings;
            std::vector<StructuralShapeBinding> shapes;
            std::vector<StructuralConstraintBinding> constraints;
            std::vector<PhysicsSceneBodyBinding> authoredBodies;
            std::vector<PhysicsSceneShapeBinding> authoredShapes;
            std::vector<PhysicsSceneConstraintBinding> authoredConstraints;

            Result<void> ValidatePublication() const override {
                if (!state || state->closed || state->revision != revision || !scene.IsCurrent() || state->scene != scene.RuntimeId() ||
                    !state->authority->IsCurrent(state->evidence))
                    return Result<void>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
                return native ? native->ValidatePublication() : Result<void>::Success();
            }

            void Publish() noexcept override {
                if (native)
                    native->Publish();
                state->bodies.swap(bindings);
                state->shapes.swap(shapes);
                state->constraints.swap(constraints);
                state->authoredBodies.swap(authoredBodies);
                state->authoredShapes.swap(authoredShapes);
                state->authoredConstraints.swap(authoredConstraints);
                ++state->revision;
            }

            Result<void> AfterPublication() override {
                return Result<void>::Success();
            }
        };

        /** @brief Application-composed selected world; this adapter never selects or reloads a backend. */
        class StructuralParticipant final : public Runtime::SceneStructuralParticipant {
        public:
            explicit StructuralParticipant(std::shared_ptr<PhysicsStructuralRegistration> registration)
                : registration_(std::move(registration)) {}

            Runtime::SceneStructuralOwner Owner() const noexcept override {
                return Runtime::SceneStructuralOwner::Physics;
            }

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(
                const Runtime::RuntimeSceneView active, const std::span<const Runtime::RuntimeEntityView> created,
                const std::span<const Runtime::EntityRef> destroyed) override {
                if (!registration_->active || registration_->active->closed || !active.IsCurrent() ||
                    registration_->active->scene != active.RuntimeId())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(MakeError(PhysicsErrors::InvalidState));
                try {
                    return PrepareOwned(active, created, destroyed);
                } catch (const std::bad_alloc &) {
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
                }
            }

        private:
            /** @brief Detached descriptor and binding projections shared by the preparation phases. */
            struct GroupPlan {
                struct ColliderBindingInput {
                    Runtime::EntityRef entity;
                    Runtime::PhysicsColliderSlotId slot;
                    std::size_t shape{};
                    std::size_t body{};
                };

                std::vector<BodyHandle> retiredBodies;
                std::vector<ShapeHandle> retiredShapes;
                std::vector<ConstraintHandle> retiredConstraints;
                std::vector<Runtime::EntityRef> owners;
                std::vector<PhysicsSceneGroupBody> bodies;
                std::vector<PhysicsSceneGroupShape> shapes;
                std::vector<ColliderBindingInput> colliderBindings;
                std::vector<PhysicsConstraintDescriptor> constraints;
                std::vector<std::pair<Runtime::EntityRef, Runtime::PhysicsConstraintSlotId>> constraintBindings;
            };

            /** @brief Removes retired bindings from detached copies only; native retirement is prepared later. */
            static void ProjectRetirement(StructuralCandidate &candidate, GroupPlan &plan,
                                          const std::span<const Runtime::EntityRef> destroyed) {
                const auto retiring = [destroyed](const Runtime::EntityRef entity) {
                    return std::ranges::find(destroyed, entity) != destroyed.end();
                };
                for (const auto &binding : candidate.bindings)
                    if (retiring(binding.entity))
                        plan.retiredBodies.push_back(binding.handle);
                for (const auto &binding : candidate.shapes)
                    if (retiring(binding.entity) || std::ranges::find(plan.retiredBodies, binding.body) != plan.retiredBodies.end())
                        plan.retiredShapes.push_back(binding.handle);
                for (const auto &binding : candidate.constraints)
                    if (retiring(binding.entity))
                        plan.retiredConstraints.push_back(binding.handle);
                std::erase_if(candidate.bindings, [&retiring](const auto &binding) {
                    return retiring(binding.entity);
                });
                std::erase_if(candidate.shapes, [&retiring, &plan](const auto &binding) {
                    return retiring(binding.entity) || std::ranges::find(plan.retiredBodies, binding.body) != plan.retiredBodies.end();
                });
                std::erase_if(candidate.authoredBodies, [&plan](const auto &binding) {
                    return std::ranges::find(plan.retiredBodies, binding.handle) != plan.retiredBodies.end();
                });
                std::erase_if(candidate.authoredShapes, [&plan](const auto &binding) {
                    return std::ranges::find(plan.retiredShapes, binding.handle) != plan.retiredShapes.end();
                });
            }

            /** @brief Converts the typed authored body policy and candidate hierarchy into a native-neutral descriptor. */
            static Result<void> PrepareBodyPolicy(PhysicsSceneGroupBody &body, const Runtime::RuntimeEntityView &entity,
                                                  const Runtime::RuntimeSceneView active,
                                                  const std::span<const Runtime::RuntimeEntityView> created) {
                const auto &component = *entity.components->rigidBody;
                auto transform = WorldTransform(entity, active, created);
                if (transform.HasError())
                    return Result<void>::Failure(transform.ErrorValue());
                auto pose = ToBodyPhysicsPose(transform.Value());
                if (pose.HasError())
                    return Result<void>::Failure(pose.ErrorValue());
                body.descriptor.body.pose = pose.Value();
                body.descriptor.body.motion = static_cast<PhysicsMotionType>(component.motion);
                body.descriptor.body.mass = std::visit([]<typename T>(const T &mass) -> PhysicsMassPolicy {
                    if constexpr (std::is_same_v<T, Runtime::AuthoredPhysicsNoMass>)
                        return PhysicsNoMass{};
                    else if constexpr (std::is_same_v<T, Runtime::AuthoredPhysicsMass>)
                        return PhysicsMass{mass.kilograms};
                    else
                        return PhysicsDensity{mass.kilogramsPerCubicMeter};
                }, component.mass);
                body.descriptor.body.linearVelocity = component.initialLinearVelocity;
                body.descriptor.body.angularVelocity = component.initialAngularVelocity;
                body.descriptor.body.motionSafety.linearDampingPerSecond = component.linearDampingPerSecond;
                body.descriptor.body.motionSafety.angularDampingPerSecond = component.angularDampingPerSecond;
                body.descriptor.body.motionSafety.maximumLinearSpeed = component.maximumLinearSpeed;
                body.descriptor.body.motionSafety.maximumAngularSpeed = component.maximumAngularSpeed;
                return Result<void>::Success();
            }

            /** @brief Validates one collider's immutable resource closure before admitting its shape. */
            static Result<void> PrepareCollider(GroupPlan &plan, PhysicsSceneGroupBody &body,
                                                std::vector<PhysicsSceneGroupShape::Child> &children,
                                                const Runtime::RuntimeEntityView &contributor, const Runtime::ColliderComponent &collider,
                                                const Runtime::RuntimeSceneView active) {
                const bool hasChildren = !children.empty();
                const auto *analytic = std::get_if<Runtime::PhysicsAnalyticCollider>(&collider.source);
                for (const auto &material : collider.materials)
                    if (const auto valid = ValidateAsset(contributor, active, material.material, PhysicsErrors::MaterialDescriptorInvalid);
                        valid.HasError())
                        return Result<void>::Failure(valid.ErrorValue());
                if (!analytic) {
                    const auto asset = std::get<Runtime::PhysicsShapeAssetReference>(collider.source).asset;
                    if (const auto valid = ValidateAsset(contributor, active, asset, PhysicsErrors::ShapeArtifactInvalid); valid.HasError())
                        return Result<void>::Failure(valid.ErrorValue());
                    return Result<void>::Failure(
                        MakeError(PhysicsErrors::OperationUnsupported,
                                  "The exact cooked shape has no qualified CanonicalV1 realization path; runtime cooking and "
                                  "fallback are forbidden."));
                }
                auto resolved = ResolvePhysicsPrimitiveShape({.geometry = AnalyticGeometry(*analytic),
                                                              .localPose = {collider.localPose.translation, collider.localPose.rotation},
                                                              .scale = {collider.scale}});
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                if (hasChildren && body.descriptor.sensor != collider.sensor)
                    return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
                if (body.descriptor.collision && body.descriptor.collision->profile != collider.collisionProfile)
                    return Result<void>::Failure(
                        MakeError(PhysicsErrors::OperationUnsupported,
                                  "The current native group body realization requires one exact profile for all contributors."));
                if (!body.descriptor.collision)
                    body.descriptor.collision = PhysicsSceneCollisionBinding{.profile = collider.collisionProfile};
                body.descriptor.collision->colliders.push_back({.localPose = resolved.Value().localPose});
                body.descriptor.sensor = collider.sensor;
                plan.colliderBindings.emplace_back(contributor.entity, collider.collider, plan.shapes.size(), plan.bodies.size());
                children.emplace_back(static_cast<std::uint32_t>(plan.shapes.size()), resolved.Value().localPose);
                plan.shapes.emplace_back(resolved.Value().geometry);
                return Result<void>::Success();
            }

            /** @brief Collects only exact body-slot contributors and preserves their authored local poses. */
            static Result<void> PrepareBodyColliders(GroupPlan &plan, PhysicsSceneGroupBody &body, const Runtime::RuntimeEntityView &entity,
                                                     const Runtime::RuntimeSceneView active,
                                                     const std::span<const Runtime::RuntimeEntityView> created) {
                std::vector<PhysicsSceneGroupShape::Child> children;
                for (const auto &contributor : created) {
                    for (std::size_t index = 0; index < contributor.components->colliders.size(); ++index) {
                        const auto &collider = contributor.components->colliders[index];
                        if (!collider.enabled)
                            continue;
                        auto target =
                            ResolveTarget(contributor, Runtime::GroupPhysicsReferenceKind::ColliderBody, index, collider.body, active);
                        if (target.HasError())
                            return Result<void>::Failure(target.ErrorValue());
                        if (target.Value() != entity.entity || collider.body.body != entity.components->rigidBody->body)
                            continue;
                        if (const auto ready = PrepareCollider(plan, body, children, contributor, collider, active); ready.HasError())
                            return ready;
                    }
                }
                if (children.empty())
                    return Failure<void>("An enabled group body has no collider contributors.");
                // A compound also preserves the local pose of a single collider.
                body.shape = static_cast<std::uint32_t>(plan.shapes.size());
                plan.shapes.emplace_back(std::move(children));
                return Result<void>::Success();
            }

            /** @brief Projects enabled bodies before any detached native allocation. */
            static Result<void> PrepareBodies(GroupPlan &plan, const Runtime::RuntimeSceneView active,
                                              const std::span<const Runtime::RuntimeEntityView> created) {
                for (const auto &entity : created) {
                    if (!entity.components)
                        return Failure<void>("Missing structural component projection.");
                }
                for (const auto &entity : created) {
                    if (!entity.components->rigidBody || !entity.components->rigidBody->enabled)
                        continue;
                    PhysicsSceneGroupBody body;
                    if (const auto ready = PrepareBodyPolicy(body, entity, active, created); ready.HasError())
                        return ready;
                    if (const auto ready = PrepareBodyColliders(plan, body, entity, active, created); ready.HasError())
                        return ready;
                    plan.bodies.push_back(std::move(body));
                    plan.owners.push_back(entity.entity);
                }
                return Result<void>::Success();
            }

            /** @brief Resolves the exact runtime entity/body slot from candidate bindings. */
            static std::optional<BodyHandle> ResolveBody(const StructuralCandidate &candidate, const Runtime::EntityRef entity,
                                                         const Runtime::PhysicsBodySlotId slot) {
                for (const auto &binding : candidate.bindings)
                    if (binding.entity == entity && binding.slot == slot)
                        return binding.handle;
                return std::nullopt;
            }

            /** @brief Resolves a constraint's second endpoint without synthesizing a resident body. */
            static Result<void> ResolveSecondEndpoint(PhysicsConstraintDescriptor &constraint, const Runtime::RuntimeEntityView &entity,
                                                      const Runtime::PhysicsConstraintComponent &component, const std::size_t index,
                                                      const Runtime::RuntimeSceneView active, const StructuralCandidate &candidate) {
                if (const auto *second = std::get_if<Runtime::PhysicsConstraintBodyEndpoint>(&component.second)) {
                    auto target = ResolveTarget(entity, Runtime::GroupPhysicsReferenceKind::ConstraintSecond, index, second->body, active);
                    if (target.HasError())
                        return Result<void>::Failure(target.ErrorValue());
                    const auto secondHandle = ResolveBody(candidate, target.Value(), second->body.body);
                    if (!secondHandle)
                        return Failure<void>("A group constraint has no exact second body binding.");
                    constraint.second = PhysicsBodyAnchor{*secondHandle, {second->localFrame.translation, second->localFrame.rotation}};
                } else {
                    const auto &frame = std::get<Runtime::PhysicsConstraintWorldEndpoint>(component.second).frame;
                    constraint.second = PhysicsWorldAnchor{{frame.translation, frame.rotation}};
                }
                return Result<void>::Success();
            }

            /** @brief Projects one enabled typed constraint against the prepared body bindings. */
            static Result<PhysicsConstraintDescriptor> PrepareConstraint(const Runtime::RuntimeEntityView &entity, const std::size_t index,
                                                                         const Runtime::RuntimeSceneView active,
                                                                         const StructuralCandidate &candidate) {
                const auto &component = entity.components->physicsConstraints[index];
                auto first =
                    ResolveTarget(entity, Runtime::GroupPhysicsReferenceKind::ConstraintFirst, index, component.first.body, active);
                if (first.HasError())
                    return Result<PhysicsConstraintDescriptor>::Failure(first.ErrorValue());
                const auto firstHandle = ResolveBody(candidate, first.Value(), component.first.body.body);
                if (!firstHandle)
                    return Failure<PhysicsConstraintDescriptor>("A group constraint has no exact first body binding.");
                PhysicsConstraintDescriptor constraint;
                constraint.first = {*firstHandle, {component.first.localFrame.translation, component.first.localFrame.rotation}};
                if (const auto ready = ResolveSecondEndpoint(constraint, entity, component, index, active, candidate); ready.HasError())
                    return Result<PhysicsConstraintDescriptor>::Failure(ready.ErrorValue());
                constraint.parameters = std::visit([]<typename T>(const T &parameters) -> decltype(constraint.parameters) {
                    if constexpr (std::is_same_v<T, Runtime::PhysicsFixedConstraint>)
                        return PhysicsFixedConstraint{};
                    else
                        return PhysicsDistanceConstraint{parameters.minimumMeters, parameters.maximumMeters};
                }, component.parameters);
                return Result<PhysicsConstraintDescriptor>::Success(std::move(constraint));
            }

            /** @brief Stages constraint descriptors and their publication bindings in occurrence order. */
            static Result<void> PrepareConstraints(StructuralCandidate &candidate, GroupPlan &plan, const Runtime::RuntimeSceneView active,
                                                   const std::span<const Runtime::RuntimeEntityView> created) {
                for (const auto &entity : created) {
                    for (std::size_t index = 0; index < entity.components->physicsConstraints.size(); ++index) {
                        const auto &component = entity.components->physicsConstraints[index];
                        if (!component.enabled)
                            continue;
                        auto descriptor = PrepareConstraint(entity, index, active, candidate);
                        if (descriptor.HasError())
                            return Result<void>::Failure(descriptor.ErrorValue());
                        plan.constraints.push_back(std::move(descriptor).Value());
                        plan.constraintBindings.emplace_back(entity.entity, component.constraint);
                    }
                }
                if (const auto admitted = candidate.native->PrepareConstraints(plan.constraints); admitted.HasError())
                    return admitted;
                for (std::size_t index = 0; index < plan.constraintBindings.size(); ++index)
                    candidate.constraints.emplace_back(plan.constraintBindings[index].first, plan.constraintBindings[index].second,
                                                       candidate.native->Constraints()[index]);
                return Result<void>::Success();
            }

            /** @brief Incorporates native retirement closure and additions in detached binding tables. */
            static void ProjectNativeBindings(StructuralCandidate &candidate, const GroupPlan &plan,
                                              const std::span<const Runtime::RuntimeEntityView> created) {
                std::erase_if(candidate.constraints, [&candidate](const auto &binding) {
                    const auto handles = candidate.native->RetiredConstraints();
                    return std::ranges::find(handles, binding.handle) != handles.end();
                });
                std::erase_if(candidate.authoredConstraints, [&candidate](const auto &binding) {
                    const auto handles = candidate.native->RetiredConstraints();
                    return std::ranges::find(handles, binding.handle) != handles.end();
                });
                for (const auto &binding : plan.colliderBindings)
                    candidate.shapes.emplace_back(binding.entity, binding.slot, candidate.native->Shapes()[binding.shape],
                                                  candidate.native->Handles()[binding.body]);
                for (std::size_t index = 0; index < plan.owners.size(); ++index) {
                    const auto entity = std::ranges::find(created, plan.owners[index], &Runtime::RuntimeEntityView::entity);
                    candidate.bindings.emplace_back(plan.owners[index], entity->components->rigidBody->body,
                                                    candidate.native->Handles()[index]);
                }
            }

            /** @brief Prepares the complete detached owner candidate; publication remains a separate no-fail phase. */
            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> PrepareOwned(
                const Runtime::RuntimeSceneView active, const std::span<const Runtime::RuntimeEntityView> created,
                const std::span<const Runtime::EntityRef> destroyed) const {
                auto candidate = std::make_unique<StructuralCandidate>();
                candidate->state = registration_->active;
                candidate->scene = active;
                candidate->revision = candidate->state->revision;
                candidate->bindings = candidate->state->bodies;
                candidate->shapes = candidate->state->shapes;
                candidate->constraints = candidate->state->constraints;
                candidate->authoredBodies = candidate->state->authoredBodies;
                candidate->authoredShapes = candidate->state->authoredShapes;
                candidate->authoredConstraints = candidate->state->authoredConstraints;
                if (candidate->revision == std::numeric_limits<std::uint64_t>::max())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                        MakeError(PhysicsErrors::GenerationExhausted));
                GroupPlan plan;
                ProjectRetirement(*candidate, plan, destroyed);
                if (const auto ready = PrepareBodies(plan, active, created); ready.HasError())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(ready.ErrorValue());
                if (const bool hasConstraints = std::ranges::any_of(created,
                                                                    [](const auto &entity) {
                    return std::ranges::any_of(entity.components->physicsConstraints, [](const auto &constraint) {
                        return constraint.enabled;
                    });
                });
                    plan.bodies.empty() && !hasConstraints && plan.retiredBodies.empty() && plan.retiredShapes.empty() &&
                    plan.retiredConstraints.empty())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Success(std::move(candidate));
                auto prepared = candidate->state->world->PrepareSceneGroup(plan.shapes, plan.bodies);
                if (prepared.HasError())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(prepared.ErrorValue());
                candidate->native = std::move(prepared).Value();
                if (const auto ready =
                        candidate->native->PrepareRetirement(plan.retiredBodies, plan.retiredShapes, plan.retiredConstraints);
                    ready.HasError())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(ready.ErrorValue());
                ProjectNativeBindings(*candidate, plan, created);
                if (const auto ready = PrepareConstraints(*candidate, plan, active, created); ready.HasError())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(ready.ErrorValue());
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Success(std::move(candidate));
            }

            std::shared_ptr<PhysicsStructuralRegistration> registration_;
        };
    }  // namespace

    /** @copydoc MakePhysicsStructuralParticipant */
    std::unique_ptr<Runtime::SceneStructuralParticipant> MakePhysicsStructuralParticipant(
        std::shared_ptr<PhysicsStructuralRegistration> registration) {
        return std::make_unique<StructuralParticipant>(std::move(registration));
    }
}  // namespace Horo::Physics::Detail
