#include "CanonicalPhysicsRuntimeInternal.h"

#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        [[nodiscard]] const CanonicalSceneShapeRecord *FindSceneShape(const CanonicalWorld &world, const ShapeHandle handle) {
            const auto found = std::ranges::find_if(world.sceneShapes, [handle](const auto &shape) {
                return shape.handle == handle;
            });
            return found == world.sceneShapes.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] const CanonicalSceneBodyRecord *FindSceneBody(const CanonicalWorld &world, const BodyHandle handle) {
            const auto found = std::ranges::find_if(world.sceneBodies, [handle](const auto &body) {
                return body.handle == handle;
            });
            return found == world.sceneBodies.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] Result<void> AddCanonicalCompoundChildren(CanonicalWorld &world, const PhysicsWorldId owner,
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
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            const float defaultMass = settings.GetMassProperties().mMass;
            if (!std::isfinite(defaultMass) || defaultMass <= 0.0F)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Canonical scene density could not derive a finite body mass."));
            settings.mMassPropertiesOverride.mMass = defaultMass * (density->kilogramsPerCubicMeter / 1'000.0F);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<CanonicalConstraintBodies> ResolveCanonicalConstraintBodies(CanonicalWorld &world, const PhysicsWorldId owner,
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
            return world.nextSceneConstraintSlot != std::numeric_limits<std::uint32_t>::max() &&
                   world.sceneConstraints.size() < world.maximumConstraints;
        }

        [[nodiscard]] Result<void> ValidateCanonicalConstraintLocks(const JPH::BodyLockWrite &firstLock,
                                                                    const JPH::BodyLockWrite &secondLock,
                                                                    const CanonicalSceneBodyRecord *second) {
            if (!firstLock.Succeeded() || (second != nullptr && !secondLock.Succeeded()))
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<JPH::Ref<JPH::Constraint>> CreateNativeSceneConstraint(
            JPH::BodyLockWrite &firstLock, JPH::BodyLockWrite &secondLock, const CanonicalSceneBodyRecord *second,
            const PhysicsPose &firstFrame, const PhysicsPose &secondFrame,
            const std::variant<PhysicsFixedConstraint, PhysicsDistanceConstraint> &parameters) {
            JPH::Ref<JPH::Constraint> nativeConstraint;
            std::visit([&nativeConstraint, &firstLock, &secondLock, second, &firstFrame, &secondFrame](const auto &value) {
                using Parameter = std::decay_t<decltype(value)>;
                JPH::Body &body1 = firstLock.GetBody();
                JPH::Body &body2 = second != nullptr ? secondLock.GetBody() : JPH::Body::sFixedToWorld;
                if constexpr (std::is_same_v<Parameter, PhysicsFixedConstraint>) {
                    JPH::FixedConstraintSettings settings;
                    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
                    settings.mAutoDetectPoint = false;
                    settings.mPoint1 = ToNativePoint(firstFrame.translation);
                    settings.mAxisX1 = ToNative(firstFrame.rotation.Rotate({1.0F, 0.0F, 0.0F}));
                    settings.mAxisY1 = ToNative(firstFrame.rotation.Rotate({0.0F, 1.0F, 0.0F}));
                    settings.mPoint2 = ToNativePoint(secondFrame.translation);
                    settings.mAxisX2 = ToNative(secondFrame.rotation.Rotate({1.0F, 0.0F, 0.0F}));
                    settings.mAxisY2 = ToNative(secondFrame.rotation.Rotate({0.0F, 1.0F, 0.0F}));
                    nativeConstraint = settings.Create(body1, body2);
                } else {
                    static_assert(std::is_same_v<Parameter, PhysicsDistanceConstraint>);
                    JPH::DistanceConstraintSettings settings;
                    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
                    settings.mPoint1 = ToNativePoint(firstFrame.translation);
                    settings.mPoint2 = ToNativePoint(secondFrame.translation);
                    settings.mMinDistance = value.minimumMeters;
                    settings.mMaxDistance = value.maximumMeters;
                    nativeConstraint = settings.Create(body1, body2);
                }
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
        if (canonical.nextSceneShapeSlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.sceneShapes.size() >= canonical.maximumShapes)
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (const Result<void> valid = ValidatePhysicsShapeDescriptor(descriptor); valid.HasError())
            return Result<ShapeHandle>::Failure(valid.ErrorValue());
        const Result<JPH::Ref<JPH::Shape>> nativeShape = CreateNativeShape(descriptor);
        if (nativeShape.HasError())
            return Result<ShapeHandle>::Failure(nativeShape.ErrorValue());

        const std::uint32_t slot = canonical.nextSceneShapeSlot++;
        const ShapeHandle identity{owner, {slot, 1}};
        canonical.sceneShapes.push_back({.handle = identity, .shape = nativeShape.Value()});
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
        if (canonical.nextSceneShapeSlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.sceneShapes.size() >= canonical.maximumShapes)
            return Result<ShapeHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));

        JPH::StaticCompoundShapeSettings settings;
        if (const Result<void> children = AddCanonicalCompoundChildren(canonical, owner, instances, settings); children.HasError())
            return Result<ShapeHandle>::Failure(children.ErrorValue());
        const JPH::ShapeSettings::ShapeResult created = settings.Create();
        if (created.HasError())
            return Result<ShapeHandle>::Failure(
                MakeError(PhysicsErrors::ShapeArtifactInvalid, "Canonical solver rejected the scene compound shape."));

        const std::uint32_t slot = canonical.nextSceneShapeSlot++;
        const ShapeHandle identity{owner, {slot, 1}};
        canonical.sceneShapes.push_back({.handle = identity, .shape = created.Get()});
        return Result<ShapeHandle>::Success(identity);
    }

    /** @copydoc CreateCanonicalSceneBody */
    Result<BodyHandle> CreateCanonicalSceneBody(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                const PhysicsSceneBodyDescriptor &descriptor) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (canonical.nextSceneBodySlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.sceneBodies.size() >= canonical.maximumBodies)
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

        if (const Result<void> mass = ApplyCanonicalMassPolicy(settings, descriptor.body.mass); mass.HasError())
            return Result<BodyHandle>::Failure(mass.ErrorValue());

        const JPH::BodyID nativeBody = canonical.system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (nativeBody.IsInvalid())
            return Result<BodyHandle>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Canonical solver rejected the scene body admission."));
        if (nativeBody.GetIndex() >= canonical.nativeFixtureIndices.size()) {
            canonical.system->GetBodyInterface().RemoveBody(nativeBody);
            canonical.system->GetBodyInterface().DestroyBody(nativeBody);
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }

        const std::uint32_t slot = canonical.nextSceneBodySlot++;
        const BodyHandle identity{owner, {slot, 1}};
        canonical.sceneBodies.push_back({.handle = identity, .nativeBody = nativeBody, .pose = descriptor.body.pose});
        return Result<BodyHandle>::Success(identity);
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

        JPH::BodyLockWrite firstLock(canonical.system->GetBodyLockInterfaceNoLock(), first->nativeBody);
        JPH::BodyLockWrite secondLock(canonical.system->GetBodyLockInterfaceNoLock(),
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

        canonical.system->AddConstraint(nativeConstraint.Value().GetPtr());
        const std::uint32_t slot = canonical.nextSceneConstraintSlot++;
        const ConstraintHandle identity{owner, {slot, 1}};
        canonical.sceneConstraints.push_back({.handle = identity, .constraint = nativeConstraint.Value()});
        return Result<ConstraintHandle>::Success(identity);
    }
}  // namespace Horo::Physics::Detail
