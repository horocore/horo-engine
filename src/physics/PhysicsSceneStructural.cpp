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
                const auto found = std::ranges::find(created, *entity.parent, &Runtime::RuntimeEntityView::entity);
                if (found != created.end()) {
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
                auto decoded = Assets::DecodeCookedArtifact({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
                if (decoded.HasError())
                    return Result<void>::Failure(decoded.ErrorValue());
                if (decoded.Value().id != asset || decoded.Value().type != resource.metadata.expectedType ||
                    decoded.Value().payload.empty())
                    return Result<void>::Failure(MakeError(code, "Structural resource does not match its admitted identity/type."));
                return Result<void>::Success();
            }
            const auto resolved = active.FindAsset(asset);
            if (!resolved || !resolved->type || resolved->bytes.empty())
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
            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> PrepareOwned(
                const Runtime::RuntimeSceneView active, const std::span<const Runtime::RuntimeEntityView> created,
                const std::span<const Runtime::EntityRef> destroyed) {
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
                const auto retiring = [&](const Runtime::EntityRef entity) {
                    return std::ranges::find(destroyed, entity) != destroyed.end();
                };
                std::vector<BodyHandle> retiredBodies;
                std::vector<ShapeHandle> retiredShapes;
                std::vector<ConstraintHandle> retiredConstraints;
                for (const auto &binding : candidate->bindings)
                    if (retiring(binding.entity))
                        retiredBodies.push_back(binding.handle);
                for (const auto &binding : candidate->shapes)
                    if (retiring(binding.entity) || std::ranges::find(retiredBodies, binding.body) != retiredBodies.end())
                        retiredShapes.push_back(binding.handle);
                for (const auto &binding : candidate->constraints)
                    if (retiring(binding.entity))
                        retiredConstraints.push_back(binding.handle);
                std::erase_if(candidate->bindings, [&](const auto &binding) {
                    return retiring(binding.entity);
                });
                std::erase_if(candidate->shapes, [&](const auto &binding) {
                    return retiring(binding.entity) || std::ranges::find(retiredBodies, binding.body) != retiredBodies.end();
                });
                std::erase_if(candidate->authoredBodies, [&](const auto &binding) {
                    return std::ranges::find(retiredBodies, binding.handle) != retiredBodies.end();
                });
                std::erase_if(candidate->authoredShapes, [&](const auto &binding) {
                    return std::ranges::find(retiredShapes, binding.handle) != retiredShapes.end();
                });
                for (const auto &entity : created)
                    if (!entity.components)
                        return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>("Missing structural component projection.");

                std::vector<Runtime::EntityRef> owners;
                std::vector<PhysicsSceneGroupBody> bodies;
                std::vector<PhysicsSceneGroupShape> shapes;

                struct ColliderBindingInput {
                    Runtime::EntityRef entity;
                    Runtime::PhysicsColliderSlotId slot;
                    std::size_t shape{};
                    std::size_t body{};
                };

                std::vector<ColliderBindingInput> colliderBindings;
                for (const auto &entity : created) {
                    if (!entity.components || !entity.components->rigidBody || !entity.components->rigidBody->enabled)
                        continue;
                    const auto &component = *entity.components->rigidBody;
                    auto transform = WorldTransform(entity, active, created);
                    if (transform.HasError())
                        return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(transform.ErrorValue());
                    auto pose = ToBodyPhysicsPose(transform.Value());
                    if (pose.HasError())
                        return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(pose.ErrorValue());
                    PhysicsSceneGroupBody body;
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
                    std::vector<PhysicsSceneGroupShape::Child> children;
                    for (const auto &contributor : created) {
                        for (std::size_t index = 0; index < contributor.components->colliders.size(); ++index) {
                            const auto &collider = contributor.components->colliders[index];
                            if (!collider.enabled)
                                continue;
                            auto target =
                                ResolveTarget(contributor, Runtime::GroupPhysicsReferenceKind::ColliderBody, index, collider.body, active);
                            if (target.HasError())
                                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(target.ErrorValue());
                            if (target.Value() != entity.entity || collider.body.body != component.body)
                                continue;
                            const auto *analytic = std::get_if<Runtime::PhysicsAnalyticCollider>(&collider.source);
                            for (const auto &material : collider.materials)
                                if (const auto valid =
                                        ValidateAsset(contributor, active, material.material, PhysicsErrors::MaterialDescriptorInvalid);
                                    valid.HasError())
                                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(valid.ErrorValue());
                            if (!analytic) {
                                const auto asset = std::get<Runtime::PhysicsShapeAssetReference>(collider.source).asset;
                                if (const auto valid = ValidateAsset(contributor, active, asset, PhysicsErrors::ShapeArtifactInvalid);
                                    valid.HasError())
                                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(valid.ErrorValue());
                                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                                    MakeError(PhysicsErrors::OperationUnsupported,
                                              "The exact cooked shape has no qualified CanonicalV1 realization path; runtime cooking and "
                                              "fallback are forbidden."));
                            }
                            auto resolved =
                                ResolvePhysicsPrimitiveShape({.geometry = AnalyticGeometry(*analytic),
                                                              .localPose = {collider.localPose.translation, collider.localPose.rotation},
                                                              .scale = {collider.scale}});
                            if (resolved.HasError())
                                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(resolved.ErrorValue());
                            if (!children.empty() && body.descriptor.sensor != collider.sensor)
                                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                                    MakeError(PhysicsErrors::OperationUnsupported));
                            body.descriptor.sensor = collider.sensor;
                            colliderBindings.push_back({contributor.entity, collider.collider, shapes.size(), bodies.size()});
                            children.push_back({static_cast<std::uint32_t>(shapes.size()), resolved.Value().localPose});
                            shapes.push_back({resolved.Value().geometry});
                        }
                    }
                    if (children.empty())
                        return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>(
                            "An enabled group body has no collider contributors.");
                    // A compound also preserves the local pose of a single collider.
                    body.shape = static_cast<std::uint32_t>(shapes.size());
                    shapes.push_back({std::move(children)});
                    bodies.push_back(std::move(body));
                    owners.push_back(entity.entity);
                }
                const bool hasConstraints = std::ranges::any_of(created, [](const auto &entity) {
                    return std::ranges::any_of(entity.components->physicsConstraints, [](const auto &constraint) {
                        return constraint.enabled;
                    });
                });
                if (bodies.empty() && !hasConstraints && retiredBodies.empty() && retiredShapes.empty() && retiredConstraints.empty())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Success(std::move(candidate));
                auto prepared = candidate->state->world->PrepareSceneGroup(shapes, bodies);
                if (prepared.HasError())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(prepared.ErrorValue());
                candidate->native = std::move(prepared).Value();
                if (const auto retirement = candidate->native->PrepareRetirement(retiredBodies, retiredShapes, retiredConstraints);
                    retirement.HasError())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(retirement.ErrorValue());
                std::erase_if(candidate->constraints, [&](const auto &binding) {
                    const auto handles = candidate->native->RetiredConstraints();
                    return std::ranges::find(handles, binding.handle) != handles.end();
                });
                std::erase_if(candidate->authoredConstraints, [&](const auto &binding) {
                    const auto handles = candidate->native->RetiredConstraints();
                    return std::ranges::find(handles, binding.handle) != handles.end();
                });
                for (const auto &binding : colliderBindings)
                    candidate->shapes.push_back({binding.entity, binding.slot, candidate->native->Shapes()[binding.shape],
                                                 candidate->native->Handles()[binding.body]});
                for (std::size_t index = 0; index < owners.size(); ++index) {
                    const auto entity = std::ranges::find(created, owners[index], &Runtime::RuntimeEntityView::entity);
                    candidate->bindings.push_back(
                        {owners[index], entity->components->rigidBody->body, candidate->native->Handles()[index]});
                }
                std::vector<PhysicsConstraintDescriptor> constraints;
                std::vector<std::pair<Runtime::EntityRef, Runtime::PhysicsConstraintSlotId>> constraintBindings;
                const auto resolve = [&](const Runtime::EntityRef entity,
                                         const Runtime::PhysicsBodySlotId slot) -> std::optional<BodyHandle> {
                    for (const auto &binding : candidate->bindings)
                        if (binding.entity == entity && binding.slot == slot)
                            return binding.handle;
                    return std::nullopt;
                };
                for (const auto &entity : created) {
                    for (std::size_t index = 0; index < entity.components->physicsConstraints.size(); ++index) {
                        const auto &component = entity.components->physicsConstraints[index];
                        if (!component.enabled)
                            continue;
                        auto first =
                            ResolveTarget(entity, Runtime::GroupPhysicsReferenceKind::ConstraintFirst, index, component.first.body, active);
                        if (first.HasError())
                            return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(first.ErrorValue());
                        const auto firstHandle = resolve(first.Value(), component.first.body.body);
                        if (!firstHandle)
                            return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>(
                                "A group constraint has no exact first body binding.");
                        PhysicsConstraintDescriptor constraint;
                        constraint.first = {*firstHandle, {component.first.localFrame.translation, component.first.localFrame.rotation}};
                        if (const auto *second = std::get_if<Runtime::PhysicsConstraintBodyEndpoint>(&component.second)) {
                            auto target =
                                ResolveTarget(entity, Runtime::GroupPhysicsReferenceKind::ConstraintSecond, index, second->body, active);
                            if (target.HasError())
                                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(target.ErrorValue());
                            const auto secondHandle = resolve(target.Value(), second->body.body);
                            if (!secondHandle)
                                return Failure<std::unique_ptr<Runtime::SceneStructuralCandidate>>(
                                    "A group constraint has no exact second body binding.");
                            constraint.second =
                                PhysicsBodyAnchor{*secondHandle, {second->localFrame.translation, second->localFrame.rotation}};
                        } else {
                            const auto &frame = std::get<Runtime::PhysicsConstraintWorldEndpoint>(component.second).frame;
                            constraint.second = PhysicsWorldAnchor{{frame.translation, frame.rotation}};
                        }
                        constraint.parameters = std::visit([]<typename T>(const T &parameters) -> decltype(constraint.parameters) {
                            if constexpr (std::is_same_v<T, Runtime::PhysicsFixedConstraint>)
                                return PhysicsFixedConstraint{};
                            else
                                return PhysicsDistanceConstraint{parameters.minimumMeters, parameters.maximumMeters};
                        }, component.parameters);
                        constraints.push_back(constraint);
                        constraintBindings.emplace_back(entity.entity, component.constraint);
                    }
                }
                if (const auto admitted = candidate->native->PrepareConstraints(constraints); admitted.HasError())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(admitted.ErrorValue());
                for (std::size_t index = 0; index < constraintBindings.size(); ++index)
                    candidate->constraints.push_back(
                        {constraintBindings[index].first, constraintBindings[index].second, candidate->native->Constraints()[index]});
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
