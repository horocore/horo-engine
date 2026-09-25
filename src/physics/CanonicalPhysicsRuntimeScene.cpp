#include "CanonicalPhysicsRuntimeInternal.h"

#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <limits>

namespace Horo::Physics::Detail {
    namespace {
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
                                         descriptor.body.motion != PhysicsMotionType::Static || settings.mAllowDynamicOrKinematic,
                                     .sceneEntity = descriptor.sceneEntity});
        return Result<BodyHandle>::Success(identity);
    }

    /** @copydoc ResolveCanonicalBodyMutation */
    Result<PhysicsBodyDescriptor> ResolveCanonicalBodyMutation(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                               const PhysicsBodyMutation &mutation) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (const auto handle = ValidatePhysicsHandleOwner(mutation.body, owner); handle.HasError())
            return Result<PhysicsBodyDescriptor>::Failure(handle.ErrorValue());
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto *body = FindSceneBody(canonical, mutation.body);
        if (body == nullptr)
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::HandleStale));
        JPH::BodyLockRead bodyLock(canonical.native.system->GetBodyLockInterfaceNoLock(), body->nativeBody);
        if (!bodyLock.Succeeded())
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::HandleStale));
        if (mutation.wake > PhysicsBodyWakePolicy::Wake)
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown body wake policy."));
        if (!mutation.shape && !mutation.motion && !mutation.mass && !mutation.motionSafety && !mutation.linearVelocity &&
            !mutation.angularVelocity && mutation.wake != PhysicsBodyWakePolicy::Wake)
            return Result<PhysicsBodyDescriptor>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "A body mutation requires a policy field or explicit wake."));
        if (std::ranges::any_of(canonical.scene.constraints, [&mutation](const auto &constraint) {
            return constraint.first == mutation.body || constraint.second == mutation.body;
        }))
            return Result<PhysicsBodyDescriptor>::Failure(
                MakeError(PhysicsErrors::OperationUnsupported, "Constrained body mutation requires constraint reconciliation."));

        PhysicsBodyDescriptor desired = body->policy;
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
        if (const auto valid = ValidatePhysicsBodyDescriptor(desired, owner); valid.HasError())
            return Result<PhysicsBodyDescriptor>::Failure(valid.ErrorValue());
        if (desired.motion == PhysicsMotionType::Static && mutation.wake == PhysicsBodyWakePolicy::Wake && !mutation.shape &&
            !mutation.motion && !mutation.mass && !mutation.motionSafety && !mutation.linearVelocity && !mutation.angularVelocity)
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Static bodies cannot be woken."));
        const auto *shape = FindSceneShape(canonical, desired.shape);
        if (shape == nullptr)
            return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::HandleStale));
        if (desired.motion != PhysicsMotionType::Static && shape->shape->MustBeStatic())
            return Result<PhysicsBodyDescriptor>::Failure(
                MakeError(PhysicsErrors::ShapeMotionUnsupported, "The requested shape requires a static body."));
        if (desired.motion != PhysicsMotionType::Static && !body->motionStorageReserved)
            return Result<PhysicsBodyDescriptor>::Failure(
                MakeError(PhysicsErrors::OperationUnsupported, "This static body has no reserved moving-body storage."));
        if (desired.motionSafety.maximumDepenetrationSpeed != body->policy.motionSafety.maximumDepenetrationSpeed)
            return Result<PhysicsBodyDescriptor>::Failure(
                MakeError(PhysicsErrors::OperationUnsupported, "Per-body depenetration speed cannot be changed by this solver."));
        if (desired.motion != PhysicsMotionType::Static && desired.motionSafety.lockedAxes == PhysicsAxisLock::All)
            return Result<PhysicsBodyDescriptor>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "A moving body cannot lock all translation and rotation axes."));
        if (desired.motion != PhysicsMotionType::Static) {
            JPH::BodyCreationSettings settings(shape->shape.GetPtr(), ToNativePoint(body->pose.translation), ToNative(body->pose.rotation),
                                               ToNativeMotion(desired.motion), JPH::ObjectLayer{0});
            if (const auto mass = ApplyCanonicalMassPolicy(settings, desired.mass); mass.HasError())
                return Result<PhysicsBodyDescriptor>::Failure(mass.ErrorValue());
            if (const float kilograms = settings.GetMassProperties().mMass; !std::isfinite(kilograms) || kilograms <= 0.0F)
                return Result<PhysicsBodyDescriptor>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "The replacement shape cannot produce finite body mass."));
        }
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
        JPH::MassProperties preparedMass;
        if (desired.motion != PhysicsMotionType::Static) {
            JPH::BodyCreationSettings settings(shape->shape.GetPtr(), ToNativePoint(found->pose.translation),
                                               ToNative(found->pose.rotation), ToNativeMotion(desired.motion), JPH::ObjectLayer{0});
            if (const auto mass = ApplyCanonicalMassPolicy(settings, desired.mass); mass.HasError())
                return mass;
            preparedMass = settings.GetMassProperties();
        }
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

        if (desired.motion != PhysicsMotionType::Static) {
            // The owner thread retains this exact native body ID through the safe point. A lock
            // failure here is an unexpected solver invariant break and fails the world terminally.
            JPH::BodyLockWrite lock(canonical.native.system->GetBodyLockInterfaceNoLock(), found->nativeBody);
            if (!lock.Succeeded())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::SolverFatalCondition, "Resident body vanished during owner-thread mutation."));
            JPH::MotionProperties *properties = lock.GetBody().GetMotionProperties();
            properties->SetMassProperties(ToNativeAllowedDOFs(desired.motionSafety.lockedAxes), preparedMass);
            properties->SetLinearDamping(desired.motionSafety.linearDampingPerSecond);
            properties->SetAngularDamping(desired.motionSafety.angularDampingPerSecond);
            properties->SetMaxLinearVelocity(desired.motionSafety.maximumLinearSpeed);
            properties->SetMaxAngularVelocity(desired.motionSafety.maximumAngularSpeed);
        }
        if (desired.motion != PhysicsMotionType::Static && (mutation.linearVelocity || mutation.angularVelocity)) {
            JPH::Vec3 linear;
            JPH::Vec3 angular;
            interface.GetLinearAndAngularVelocity(found->nativeBody, linear, angular);
            interface.SetLinearAndAngularVelocity(found->nativeBody, mutation.linearVelocity ? ToNative(*mutation.linearVelocity) : linear,
                                                  mutation.angularVelocity ? ToNative(*mutation.angularVelocity) : angular);
        }
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

    /** @copydoc ReadCanonicalSceneBodyReconciliation */
    Result<PhysicsBodyReconciliation> ReadCanonicalSceneBodyReconciliation(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                                           const BodyHandle body) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<PhysicsBodyReconciliation>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (const auto handle = ValidatePhysicsHandleOwner(body, owner); handle.HasError())
            return Result<PhysicsBodyReconciliation>::Failure(handle.ErrorValue());
        const auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto *record = FindSceneBody(canonical, body);
        if (record == nullptr)
            return Result<PhysicsBodyReconciliation>::Failure(MakeError(PhysicsErrors::HandleStale));
        JPH::BodyLockRead lock(canonical.native.system->GetBodyLockInterfaceNoLock(), record->nativeBody);
        if (!lock.Succeeded())
            return Result<PhysicsBodyReconciliation>::Failure(MakeError(PhysicsErrors::HandleStale));
        const JPH::Body &native = lock.GetBody();
        const auto shape = std::ranges::find_if(canonical.scene.shapes, [&native](const auto &candidate) {
            return candidate.shape.GetPtr() == native.GetShape();
        });
        if (shape == canonical.scene.shapes.end())
            return Result<PhysicsBodyReconciliation>::Failure(
                MakeError(PhysicsErrors::SolverFatalCondition, "Native body shape has no resident Horo identity."));
        const auto position = native.GetPosition();
        const auto rotation = native.GetRotation();
        const auto linear = native.GetLinearVelocity();
        const auto angular = native.GetAngularVelocity();
        const auto boundsExtent = native.GetWorldSpaceBounds().GetSize();
        PhysicsMotionType motion;
        switch (native.GetMotionType()) {
            case JPH::EMotionType::Static:
                motion = PhysicsMotionType::Static;
                break;
            case JPH::EMotionType::Kinematic:
                motion = PhysicsMotionType::Kinematic;
                break;
            case JPH::EMotionType::Dynamic:
                motion = PhysicsMotionType::Dynamic;
                break;
            default:
                return Result<PhysicsBodyReconciliation>::Failure(
                    MakeError(PhysicsErrors::SolverFatalCondition, "Native body has an unknown motion mode."));
        }
        std::optional<float> mass;
        if (motion == PhysicsMotionType::Dynamic) {
            const float inverseMass = native.GetMotionProperties()->GetInverseMass();
            if (!std::isfinite(inverseMass) || inverseMass < 0.0F)
                return Result<PhysicsBodyReconciliation>::Failure(
                    MakeError(PhysicsErrors::SolverFatalCondition, "Native dynamic body has invalid mass."));
            if (inverseMass > 0.0F)
                mass = 1.0F / inverseMass;
        }
        PhysicsBodyReconciliation
            result{.policy = record->policy,
                   .state = {.body = body,
                             .pose = {.translation = {static_cast<float>(position.GetX()), static_cast<float>(position.GetY()),
                                                      static_cast<float>(position.GetZ())},
                                      .rotation = {rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()}},
                             .linearVelocity = {linear.GetX(), linear.GetY(), linear.GetZ()},
                             .angularVelocity = {angular.GetX(), angular.GetY(), angular.GetZ()},
                             .activity = native.IsActive() ? PhysicsBodyActivity::Awake : PhysicsBodyActivity::Sleeping},
                   .observedMotion = motion,
                   .observedShape = shape->handle,
                   .observedMassKilograms = mass,
                   .observedBoundsExtent = {boundsExtent.GetX(), boundsExtent.GetY(), boundsExtent.GetZ()}};
        return Result<PhysicsBodyReconciliation>::Success(std::move(result));
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

        canonical.native.system->AddConstraint(nativeConstraint.Value().GetPtr());
        const std::uint32_t slot = canonical.scene.nextConstraintSlot++;
        const ConstraintHandle identity{owner, {slot, 1}};
        canonical.scene.constraints.emplace_back(
            CanonicalSceneConstraintRecord{.handle = identity,
                                           .constraint = nativeConstraint.Value(),
                                           .first = descriptor.first.body,
                                           .second = secondBody != nullptr ? secondBody->body : BodyHandle{}});
        return Result<ConstraintHandle>::Success(identity);
    }
}  // namespace Horo::Physics::Detail
