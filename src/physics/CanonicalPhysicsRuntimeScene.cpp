#include "CanonicalPhysicsRuntimeInternal.h"

#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Stable pair key includes Jolt's native body reuse sequence. */
        [[nodiscard]] std::uint64_t CollisionPairKey(const JPH::BodyID first, const JPH::BodyID second) noexcept {
            const auto low = std::min(first.GetIndexAndSequenceNumber(), second.GetIndexAndSequenceNumber());
            const auto high = std::max(first.GetIndexAndSequenceNumber(), second.GetIndexAndSequenceNumber());
            return (static_cast<std::uint64_t>(low) << 32U) | high;
        }

        [[nodiscard]] const CanonicalSceneShapeRecord *FindSceneShape(const CanonicalWorld &world, const ShapeHandle handle) {
            const auto found = std::ranges::find_if(world.scene.shapes, [handle](const auto &shape) {
                return shape.handle == handle;
            });
            return found == world.scene.shapes.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] const CanonicalSceneBodyRecord *FindSceneBody(const CanonicalWorld &world, const BodyHandle handle) {
            const auto found = std::ranges::find_if(world.scene.bodies, [handle](const auto &body) {
                return body.handle == handle;
            });
            return found == world.scene.bodies.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] Result<void> AddCanonicalCompoundChildren(const CanonicalWorld &world, const PhysicsWorldId owner,
                                                                const std::span<const PhysicsSceneShapeInstance> instances,
                                                                JPH::StaticCompoundShapeSettings &settings) {
            for (const PhysicsSceneShapeInstance &instance : instances) {
                if (const Result<void> valid = ValidatePhysicsHandleOwner(instance.shape, owner); valid.HasError())
                    return Result<void>::Failure(valid.ErrorValue());
                if (const Result<void> pose = ValidatePhysicsPose(instance.localPose); pose.HasError())
                    return Result<void>::Failure(pose.ErrorValue());
                const auto *child = FindSceneShape(world, instance.shape);
                if (child == nullptr)
                    return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
                settings.AddShape(ToNative(instance.localPose.translation), ToNative(instance.localPose.rotation), child->shape.GetPtr());
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyCanonicalMassPolicy(JPH::BodyCreationSettings &settings, const PhysicsMassPolicy &mass) {
            if (const auto *explicitMass = std::get_if<PhysicsMass>(&mass); explicitMass != nullptr) {
                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass = explicitMass->kilograms;
                return Result<void>::Success();
            }
            const auto *density = std::get_if<PhysicsDensity>(&mass);
            if (density == nullptr)
                return Result<void>::Success();
            const float defaultMass = settings.GetMassProperties().mMass;
            if (!std::isfinite(defaultMass) || defaultMass <= 0.0F)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Canonical scene density could not derive a finite body mass."));
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = defaultMass * (density->kilogramsPerCubicMeter / 1'000.0F);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<CanonicalConstraintBodies> ResolveCanonicalConstraintBodies(const CanonicalWorld &world,
                                                                                         const PhysicsWorldId owner,
                                                                                         const PhysicsConstraintDescriptor &descriptor) {
            if (const Result<void> valid = ValidatePhysicsConstraintDescriptor(descriptor, owner); valid.HasError())
                return Result<CanonicalConstraintBodies>::Failure(valid.ErrorValue());
            const auto *first = FindSceneBody(world, descriptor.first.body);
            if (first == nullptr)
                return Result<CanonicalConstraintBodies>::Failure(MakeError(PhysicsErrors::HandleStale));
            const auto *secondBody = std::get_if<PhysicsBodyAnchor>(&descriptor.second);
            const auto *second = secondBody == nullptr ? nullptr : FindSceneBody(world, secondBody->body);
            if (secondBody != nullptr && second == nullptr)
                return Result<CanonicalConstraintBodies>::Failure(MakeError(PhysicsErrors::HandleStale));
            return Result<CanonicalConstraintBodies>::Success(CanonicalConstraintBodies{first, second});
        }

        [[nodiscard]] bool HasCanonicalConstraintCapacity(const CanonicalWorld &world) noexcept {
            return world.scene.nextConstraintSlot != std::numeric_limits<std::uint32_t>::max() &&
                   world.scene.constraints.size() < world.scene.maximumConstraints;
        }

        [[nodiscard]] Result<void> ValidateCanonicalConstraintLocks(const JPH::BodyLockWrite &firstLock,
                                                                    const JPH::BodyLockWrite &secondLock,
                                                                    const CanonicalSceneBodyRecord *second) {
            if (!firstLock.Succeeded() || (second != nullptr && !secondLock.Succeeded()))
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
            return Result<void>::Success();
        }

        [[nodiscard]] JPH::Ref<JPH::Constraint> CreateNativeFixedConstraint(JPH::Body &body1, JPH::Body &body2,
                                                                            const PhysicsPose &firstFrame, const PhysicsPose &secondFrame) {
            JPH::FixedConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mAutoDetectPoint = false;
            settings.mPoint1 = ToNativePoint(firstFrame.translation);
            settings.mAxisX1 = ToNative(firstFrame.rotation.Rotate({1.0F, 0.0F, 0.0F}));
            settings.mAxisY1 = ToNative(firstFrame.rotation.Rotate({0.0F, 1.0F, 0.0F}));
            settings.mPoint2 = ToNativePoint(secondFrame.translation);
            settings.mAxisX2 = ToNative(secondFrame.rotation.Rotate({1.0F, 0.0F, 0.0F}));
            settings.mAxisY2 = ToNative(secondFrame.rotation.Rotate({0.0F, 1.0F, 0.0F}));
            return settings.Create(body1, body2);
        }

        [[nodiscard]] JPH::Ref<JPH::Constraint> CreateNativeDistanceConstraint(JPH::Body &body1, JPH::Body &body2,
                                                                               const PhysicsPose &firstFrame,
                                                                               const PhysicsPose &secondFrame,
                                                                               const PhysicsDistanceConstraint &distance) {
            JPH::DistanceConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = ToNativePoint(firstFrame.translation);
            settings.mPoint2 = ToNativePoint(secondFrame.translation);
            settings.mMinDistance = distance.minimumMeters;
            settings.mMaxDistance = distance.maximumMeters;
            return settings.Create(body1, body2);
        }

        [[nodiscard]] Result<JPH::Ref<JPH::Constraint>> CreateNativeSceneConstraint(
            const JPH::BodyLockWrite &firstLock, const JPH::BodyLockWrite &secondLock, const CanonicalSceneBodyRecord *second,
            const PhysicsPose &firstFrame, const PhysicsPose &secondFrame,
            const std::variant<PhysicsFixedConstraint, PhysicsDistanceConstraint> &parameters) {
            JPH::Body &body1 = firstLock.GetBody();
            JPH::Body &body2 = second != nullptr ? secondLock.GetBody() : JPH::Body::sFixedToWorld;
            JPH::Ref<JPH::Constraint> nativeConstraint = std::visit([&]<typename Parameter>(const Parameter &value) {
                using ParameterType = std::decay_t<Parameter>;
                if constexpr (std::is_same_v<ParameterType, PhysicsFixedConstraint>)
                    return CreateNativeFixedConstraint(body1, body2, firstFrame, secondFrame);
                else
                    return CreateNativeDistanceConstraint(body1, body2, firstFrame, secondFrame, value);
            }, parameters);
            if (nativeConstraint == nullptr)
                return Result<JPH::Ref<JPH::Constraint>>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Canonical solver rejected the scene constraint admission."));
            return Result<JPH::Ref<JPH::Constraint>>::Success(std::move(nativeConstraint));
        }

        /** @brief Resolves a live unconstrained mutation target before deriving replacement policy. */
        [[nodiscard]] Result<const CanonicalSceneBodyRecord *> ValidateMutationTarget(const CanonicalWorld &canonical,
                                                                                      const PhysicsWorldId owner,
                                                                                      const PhysicsBodyMutation &mutation) {
            if (const auto handle = ValidatePhysicsHandleOwner(mutation.body, owner); handle.HasError())
                return Result<const CanonicalSceneBodyRecord *>::Failure(handle.ErrorValue());
            const auto *body = FindSceneBody(canonical, mutation.body);
            if (body == nullptr)
                return Result<const CanonicalSceneBodyRecord *>::Failure(MakeError(PhysicsErrors::HandleStale));
            if (JPH::BodyLockRead bodyLock(canonical.native.system->GetBodyLockInterfaceNoLock(), body->nativeBody); !bodyLock.Succeeded())
                return Result<const CanonicalSceneBodyRecord *>::Failure(MakeError(PhysicsErrors::HandleStale));
            if (mutation.wake > PhysicsBodyWakePolicy::Wake)
                return Result<const CanonicalSceneBodyRecord *>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Unknown body wake policy."));
            if (!mutation.shape && !mutation.motion && !mutation.mass && !mutation.motionSafety && !mutation.linearVelocity &&
                !mutation.angularVelocity && mutation.wake != PhysicsBodyWakePolicy::Wake)
                return Result<const CanonicalSceneBodyRecord *>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "A body mutation requires a policy field or explicit wake."));
            if (std::ranges::any_of(canonical.scene.constraints, [&mutation](const auto &constraint) {
                return constraint.first == mutation.body || constraint.second == mutation.body;
            }))
                return Result<const CanonicalSceneBodyRecord *>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Constrained body mutation requires constraint reconciliation."));
            return Result<const CanonicalSceneBodyRecord *>::Success(body);
        }

        /** @brief Combines retained body policy with the fields supplied by one deferred mutation. */
        [[nodiscard]] PhysicsBodyDescriptor MergeBodyMutation(const PhysicsBodyDescriptor &policy, const PhysicsBodyMutation &mutation) {
            PhysicsBodyDescriptor desired = policy;
            if (mutation.shape)
                desired.shape = *mutation.shape;
            if (mutation.motion) {
                desired.motion = *mutation.motion;
                if (desired.motion != PhysicsMotionType::Dynamic && !mutation.mass)
                    desired.mass = PhysicsNoMass{};
                if (desired.motion == PhysicsMotionType::Static) {
                    if (!mutation.linearVelocity)
                        desired.linearVelocity = {};
                    if (!mutation.angularVelocity)
                        desired.angularVelocity = {};
                }
            }
            if (mutation.mass)
                desired.mass = *mutation.mass;
            if (mutation.motionSafety)
                desired.motionSafety = *mutation.motionSafety;
            if (mutation.linearVelocity)
                desired.linearVelocity = *mutation.linearVelocity;
            if (mutation.angularVelocity)
                desired.angularVelocity = *mutation.angularVelocity;
            return desired;
        }

        /** @brief Calculates and validates native mass properties before any body setter runs. */
        [[nodiscard]] Result<JPH::MassProperties> PrepareMutationMass(const CanonicalSceneBodyRecord &body,
                                                                      const CanonicalSceneShapeRecord &shape,
                                                                      const PhysicsBodyDescriptor &desired) {
            if (desired.motion == PhysicsMotionType::Static)
                return Result<JPH::MassProperties>::Success(JPH::MassProperties{});
            JPH::BodyCreationSettings settings(shape.shape.GetPtr(), ToNativePoint(body.pose.translation), ToNative(body.pose.rotation),
                                               ToNativeMotion(desired.motion), JPH::ObjectLayer{0});
            if (const auto mass = ApplyCanonicalMassPolicy(settings, desired.mass); mass.HasError())
                return Result<JPH::MassProperties>::Failure(mass.ErrorValue());
            JPH::MassProperties prepared = settings.GetMassProperties();
            if (!std::isfinite(prepared.mMass) || prepared.mMass <= 0.0F)
                return Result<JPH::MassProperties>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "The replacement shape cannot produce finite body mass."));
            return Result<JPH::MassProperties>::Success(std::move(prepared));
        }

        /** @brief Checks replacement policy against resident shape and native motion capabilities. */
        [[nodiscard]] Result<void> ValidateMutationPolicy(const CanonicalWorld &canonical, const PhysicsWorldId owner,
                                                          const CanonicalSceneBodyRecord &body, const PhysicsBodyMutation &mutation,
                                                          const PhysicsBodyDescriptor &desired) {
            if (const auto valid = ValidatePhysicsBodyDescriptor(desired, owner); valid.HasError())
                return valid;
            if (desired.motion == PhysicsMotionType::Static && mutation.wake == PhysicsBodyWakePolicy::Wake && !mutation.shape &&
                !mutation.motion && !mutation.mass && !mutation.motionSafety && !mutation.linearVelocity && !mutation.angularVelocity)
                return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Static bodies cannot be woken."));
            const auto *shape = FindSceneShape(canonical, desired.shape);
            if (shape == nullptr)
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
            if (desired.motion != PhysicsMotionType::Static && shape->shape->MustBeStatic())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ShapeMotionUnsupported, "The requested shape requires a static body."));
            if (desired.motion != PhysicsMotionType::Static && !body.motionStorageReserved)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "This static body has no reserved moving-body storage."));
            if (desired.motionSafety.maximumDepenetrationSpeed != body.policy.motionSafety.maximumDepenetrationSpeed)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Per-body depenetration speed cannot be changed by this solver."));
            if (desired.motion != PhysicsMotionType::Static && desired.motionSafety.lockedAxes == PhysicsAxisLock::All)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "A moving body cannot lock all translation and rotation axes."));
            if (const auto mass = PrepareMutationMass(body, *shape, desired); mass.HasError())
                return Result<void>::Failure(mass.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Updates the resident motion properties while holding its native write lock. */
        [[nodiscard]] Result<void> ApplyMutationMotionProperties(CanonicalWorld &canonical, const CanonicalSceneBodyRecord &body,
                                                                 const PhysicsBodyDescriptor &desired,
                                                                 const JPH::MassProperties &preparedMass) {
            if (desired.motion == PhysicsMotionType::Static)
                return Result<void>::Success();
            // The owner thread retains this exact native body ID through the safe point.
            JPH::BodyLockWrite lock(canonical.native.system->GetBodyLockInterfaceNoLock(), body.nativeBody);
            if (!lock.Succeeded())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::SolverFatalCondition, "Resident body vanished during owner-thread mutation."));
            JPH::MotionProperties *properties = lock.GetBody().GetMotionProperties();
            properties->SetMassProperties(ToNativeAllowedDOFs(desired.motionSafety.lockedAxes), preparedMass);
            properties->SetLinearDamping(desired.motionSafety.linearDampingPerSecond);
            properties->SetAngularDamping(desired.motionSafety.angularDampingPerSecond);
            properties->SetMaxLinearVelocity(desired.motionSafety.maximumLinearSpeed);
            properties->SetMaxAngularVelocity(desired.motionSafety.maximumAngularSpeed);
            return Result<void>::Success();
        }

        /** @brief Preserves unspecified velocity components when applying a partial update. */
        void ApplyMutationVelocity(JPH::BodyInterface &interface, const JPH::BodyID body, const PhysicsBodyMutation &mutation,
                                   const PhysicsMotionType motion) {
            if (motion == PhysicsMotionType::Static || (!mutation.linearVelocity && !mutation.angularVelocity))
                return;
            JPH::Vec3 linear;
            JPH::Vec3 angular;
            interface.GetLinearAndAngularVelocity(body, linear, angular);
            interface.SetLinearAndAngularVelocity(body, mutation.linearVelocity ? ToNative(*mutation.linearVelocity) : linear,
                                                  mutation.angularVelocity ? ToNative(*mutation.angularVelocity) : angular);
        }

    }  // namespace

    /** @copydoc CreateCanonicalSceneShape */
    Result<ShapeHandle> CreateCanonicalSceneShape(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                  const PhysicsShapeDescriptor &descriptor) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (canonical.scene.nextShapeSlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.scene.shapes.size() >= canonical.scene.maximumShapes)
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (const Result<void> valid = ValidatePhysicsShapeDescriptor(descriptor); valid.HasError())
            return Result<ShapeHandle>::Failure(valid.ErrorValue());
        const Result<JPH::Ref<JPH::Shape>> nativeShape = CreateNativeShape(descriptor);
        if (nativeShape.HasError())
            return Result<ShapeHandle>::Failure(nativeShape.ErrorValue());

        const std::uint32_t slot = canonical.scene.nextShapeSlot++;
        const ShapeHandle identity{owner, {slot, 1}};
        canonical.scene.shapes.emplace_back(CanonicalSceneShapeRecord{.handle = identity, .shape = nativeShape.Value()});
        return Result<ShapeHandle>::Success(identity);
    }

    /** @copydoc CreateCanonicalSceneCompoundShape */
    Result<ShapeHandle> CreateCanonicalSceneCompoundShape(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                          const std::span<const PhysicsSceneShapeInstance> instances) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (instances.empty())
            return Result<ShapeHandle>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "A canonical scene compound requires at least one child shape."));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (canonical.scene.nextShapeSlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.scene.shapes.size() >= canonical.scene.maximumShapes)
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));

        JPH::StaticCompoundShapeSettings settings;
        if (const Result<void> children = AddCanonicalCompoundChildren(canonical, owner, instances, settings); children.HasError())
            return Result<ShapeHandle>::Failure(children.ErrorValue());
        const JPH::ShapeSettings::ShapeResult created = settings.Create();
        if (created.HasError())
            return Result<ShapeHandle>::Failure(
                MakeError(PhysicsErrors::ShapeArtifactInvalid, "Canonical solver rejected the scene compound shape."));

        const std::uint32_t slot = canonical.scene.nextShapeSlot++;
        const ShapeHandle identity{owner, {slot, 1}};
        canonical.scene.shapes.emplace_back(CanonicalSceneShapeRecord{.handle = identity, .shape = created.Get()});
        return Result<ShapeHandle>::Success(identity);
    }

    /** @copydoc CreateCanonicalSceneBody */
    Result<BodyHandle> CreateCanonicalSceneBody(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                const PhysicsSceneBodyDescriptor &descriptor) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (canonical.scene.nextBodySlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.scene.bodies.size() >= canonical.scene.maximumBodies)
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (const Result<void> valid = ValidatePhysicsBodyDescriptor(descriptor.body, owner); valid.HasError())
            return Result<BodyHandle>::Failure(valid.ErrorValue());
        const auto *shape = FindSceneShape(canonical, descriptor.body.shape);
        if (shape == nullptr)
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::HandleStale));

        JPH::BodyCreationSettings settings(shape->shape.GetPtr(), ToNativePoint(descriptor.body.pose.translation),
                                           ToNative(descriptor.body.pose.rotation), ToNativeMotion(descriptor.body.motion),
                                           JPH::ObjectLayer{0});
        settings.mLinearVelocity = ToNative(descriptor.body.linearVelocity);
        settings.mAngularVelocity = ToNative(descriptor.body.angularVelocity);
        settings.mLinearDamping = descriptor.body.motionSafety.linearDampingPerSecond;
        settings.mAngularDamping = descriptor.body.motionSafety.angularDampingPerSecond;
        settings.mMaxLinearVelocity = descriptor.body.motionSafety.maximumLinearSpeed;
        settings.mMaxAngularVelocity = descriptor.body.motionSafety.maximumAngularSpeed;
        settings.mAllowedDOFs = ToNativeAllowedDOFs(descriptor.body.motionSafety.lockedAxes);
        settings.mIsSensor = descriptor.sensor;
        // Reserve native motion storage for future static -> moving safe-point transitions.
        settings.mAllowDynamicOrKinematic = !shape->shape->MustBeStatic();

        if (const Result<void> mass = ApplyCanonicalMassPolicy(settings, descriptor.body.mass); mass.HasError())
            return Result<BodyHandle>::Failure(mass.ErrorValue());

        const JPH::BodyID nativeBody =
            canonical.native.system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (nativeBody.IsInvalid())
            return Result<BodyHandle>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Canonical solver rejected the scene body admission."));
        if (nativeBody.GetIndex() >= canonical.query.nativeFixtureIndices.size()) {
            canonical.native.system->GetBodyInterface().RemoveBody(nativeBody);
            canonical.native.system->GetBodyInterface().DestroyBody(nativeBody);
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }

        const std::uint32_t slot = canonical.scene.nextBodySlot++;
        const BodyHandle identity{owner, {slot, 1}};
        canonical.scene.bodies.emplace_back(
            CanonicalSceneBodyRecord{.handle = identity,
                                     .nativeBody = nativeBody,
                                     .pose = descriptor.body.pose,
                                     .policy = descriptor.body,
                                     .motionStorageReserved =
                                         descriptor.body.motion != PhysicsMotionType::Static || settings.mAllowDynamicOrKinematic});
        return Result<BodyHandle>::Success(identity);
    }

    /** @copydoc ResolveCanonicalBodyMutation */
    Result<PhysicsBodyDescriptor> ResolveCanonicalBodyMutation(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                               const PhysicsBodyMutation &mutation) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto target = ValidateMutationTarget(canonical, owner, mutation);
        if (target.HasError())
            return Result<PhysicsBodyDescriptor>::Failure(target.ErrorValue());
        PhysicsBodyDescriptor desired = MergeBodyMutation(target.Value()->policy, mutation);
        if (const auto valid = ValidateMutationPolicy(canonical, owner, *target.Value(), mutation, desired); valid.HasError())
            return Result<PhysicsBodyDescriptor>::Failure(valid.ErrorValue());
        return Result<PhysicsBodyDescriptor>::Success(std::move(desired));
    }

    /** @copydoc ApplyCanonicalBodyMutation */
    Result<void> ApplyCanonicalBodyMutation(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                            const PhysicsBodyMutation &mutation) {
        const auto resolved = ResolveCanonicalBodyMutation(world, owner, mutation);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        auto found = std::ranges::find_if(canonical.scene.bodies, [&mutation](const auto &body) {
            return body.handle == mutation.body;
        });
        const auto *shape = FindSceneShape(canonical, resolved.Value().shape);
        const PhysicsBodyDescriptor &desired = resolved.Value();
        auto &interface = canonical.native.system->GetBodyInterface();
        if (!mutation.shape && !mutation.motion && !mutation.mass && !mutation.motionSafety && !mutation.linearVelocity &&
            !mutation.angularVelocity && mutation.wake == PhysicsBodyWakePolicy::Wake) {
            interface.ActivateBody(found->nativeBody);
            return Result<void>::Success();
        }
        const auto preparedMass = PrepareMutationMass(*found, *shape, desired);
        if (preparedMass.HasError())
            return Result<void>::Failure(preparedMass.ErrorValue());
        const bool changedShape = found->policy.shape != desired.shape;
        const bool changedMotion = found->policy.motion != desired.motion;
        const bool rebuild = changedShape || changedMotion;
        const bool safetyWakes =
            mutation.motionSafety && (desired.motionSafety.lockedAxes != found->policy.motionSafety.lockedAxes ||
                                      desired.motionSafety.maximumLinearSpeed != found->policy.motionSafety.maximumLinearSpeed ||
                                      desired.motionSafety.maximumAngularSpeed != found->policy.motionSafety.maximumAngularSpeed);
        const bool wake = rebuild || mutation.mass || safetyWakes || mutation.linearVelocity || mutation.angularVelocity ||
                          mutation.wake == PhysicsBodyWakePolicy::Wake;
        const JPH::EActivation activation = wake ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;

        if (changedMotion && desired.motion == PhysicsMotionType::Static)
            interface.SetMotionType(found->nativeBody, JPH::EMotionType::Static, JPH::EActivation::DontActivate);
        if (changedShape)
            interface.SetShape(found->nativeBody, shape->shape.GetPtr(), false, activation);
        if (changedMotion && desired.motion != PhysicsMotionType::Static)
            interface.SetMotionType(found->nativeBody, ToNativeMotion(desired.motion), activation);

        if (const auto updated = ApplyMutationMotionProperties(canonical, *found, desired, preparedMass.Value()); updated.HasError())
            return updated;
        ApplyMutationVelocity(interface, found->nativeBody, mutation, desired.motion);
        if (wake && desired.motion != PhysicsMotionType::Static)
            interface.ActivateBody(found->nativeBody);
        found->policy = desired;
        return Result<void>::Success();
    }

    /** @copydoc ReadCanonicalSceneBodyPolicy */
    Result<PhysicsBodyDescriptor> ReadCanonicalSceneBodyPolicy(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                               const BodyHandle body) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (const auto handle = ValidatePhysicsHandleOwner(body, owner); handle.HasError())
            return Result<PhysicsBodyDescriptor>::Failure(handle.ErrorValue());
        const auto *record = FindSceneBody(*static_cast<CanonicalWorld *>(world.value), body);
        if (record == nullptr)
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::HandleStale));
        return Result<PhysicsBodyDescriptor>::Success(record->policy);
    }

    /** @copydoc CreateCanonicalSceneConstraint */
    Result<ConstraintHandle> CreateCanonicalSceneConstraint(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                            const PhysicsConstraintDescriptor &descriptor) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<ConstraintHandle>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (!HasCanonicalConstraintCapacity(canonical))
            return Result<ConstraintHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        const Result<CanonicalConstraintBodies> bodies = ResolveCanonicalConstraintBodies(canonical, owner, descriptor);
        if (bodies.HasError())
            return Result<ConstraintHandle>::Failure(bodies.ErrorValue());
        const CanonicalSceneBodyRecord *first = bodies.Value().first;
        const CanonicalSceneBodyRecord *second = bodies.Value().second;
        const auto *secondBody = std::get_if<PhysicsBodyAnchor>(&descriptor.second);

        JPH::BodyLockWrite firstLock(canonical.native.system->GetBodyLockInterfaceNoLock(), first->nativeBody);
        JPH::BodyLockWrite secondLock(canonical.native.system->GetBodyLockInterfaceNoLock(),
                                      second != nullptr ? second->nativeBody : JPH::BodyID{});
        if (const Result<void> locks = ValidateCanonicalConstraintLocks(firstLock, secondLock, second); locks.HasError())
            return Result<ConstraintHandle>::Failure(locks.ErrorValue());

        const PhysicsPose firstFrame = ComposePhysicsPose(first->pose, descriptor.first.localFrame);
        const PhysicsPose secondFrame = second != nullptr ? ComposePhysicsPose(second->pose, secondBody->localFrame)
                                                          : std::get<PhysicsWorldAnchor>(descriptor.second).frame;
        const Result<JPH::Ref<JPH::Constraint>> nativeConstraint =
            CreateNativeSceneConstraint(firstLock, secondLock, second, firstFrame, secondFrame, descriptor.parameters);
        if (nativeConstraint.HasError())
            return Result<ConstraintHandle>::Failure(nativeConstraint.ErrorValue());

        const JPH::BodyID secondNativeBody = second == nullptr ? JPH::BodyID{} : second->nativeBody;
        if (second != nullptr && descriptor.collisionPolicy == PhysicsJointCollisionPolicy::DisableBetweenBodies) {
            const std::uint64_t key = CollisionPairKey(first->nativeBody, secondNativeBody);
            const auto insertion = std::ranges::lower_bound(canonical.scene.disabledJointCollisionPairs, key);
            if (insertion == canonical.scene.disabledJointCollisionPairs.end() || *insertion != key)
                canonical.scene.disabledJointCollisionPairs.insert(insertion, key);
        }
        canonical.native.system->AddConstraint(nativeConstraint.Value().GetPtr());
        const std::uint32_t slot = canonical.scene.nextConstraintSlot++;
        const ConstraintHandle identity{owner, {slot, 1}};
        canonical.scene.constraints.emplace_back(
            CanonicalSceneConstraintRecord{.handle = identity,
                                           .constraint = nativeConstraint.Value(),
                                           .first = descriptor.first.body,
                                           .second = secondBody != nullptr ? secondBody->body : BodyHandle{},
                                           .firstBody = first->nativeBody,
                                           .secondBody = secondNativeBody,
                                           .collisionPolicy = descriptor.collisionPolicy});
        return Result<ConstraintHandle>::Success(identity);
    }

    /** @copydoc DestroyCanonicalSceneConstraint */
    Result<void> DestroyCanonicalSceneConstraint(const CanonicalWorldHandle world, const ConstraintHandle constraint) {
        if (world.value == nullptr)
            return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto found = std::ranges::find_if(canonical.scene.constraints, [constraint](const auto &record) {
            return record.handle == constraint;
        });
        if (found == canonical.scene.constraints.end())
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
        const JPH::BodyID firstBody = found->firstBody;
        const JPH::BodyID secondBody = found->secondBody;
        const bool disabledCollision =
            secondBody.IsInvalid() == false && found->collisionPolicy == PhysicsJointCollisionPolicy::DisableBetweenBodies;
        canonical.native.system->RemoveConstraint(found->constraint.GetPtr());
        canonical.scene.constraints.erase(found);
        if (disabledCollision) {
            const bool stillDisabled = std::ranges::any_of(canonical.scene.constraints, [firstBody, secondBody](const auto &record) {
                return !record.secondBody.IsInvalid() && record.collisionPolicy == PhysicsJointCollisionPolicy::DisableBetweenBodies &&
                       CollisionPairKey(record.firstBody, record.secondBody) == CollisionPairKey(firstBody, secondBody);
            });
            if (!stillDisabled) {
                const std::uint64_t key = CollisionPairKey(firstBody, secondBody);
                const auto pair = std::ranges::lower_bound(canonical.scene.disabledJointCollisionPairs, key);
                if (pair != canonical.scene.disabledJointCollisionPairs.end() && *pair == key)
                    canonical.scene.disabledJointCollisionPairs.erase(pair);
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
