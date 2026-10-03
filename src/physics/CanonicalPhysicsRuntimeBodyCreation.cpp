#include "CanonicalPhysicsRuntimeInternal.h"

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Tests exact rest without squaring away subnormal velocity components. */
        [[nodiscard]] bool IsZeroVelocity(const Math::Vec3 velocity) noexcept {
            return velocity.x == 0.0F && velocity.y == 0.0F && velocity.z == 0.0F;
        }

        /** @brief Rejects ambiguous dormant creation before any native allocation. */
        [[nodiscard]] Result<void> ValidateInitialBodyActivity(const PhysicsSceneBodyDescriptor &descriptor, const bool sleepingEnabled) {
            if (descriptor.initialActivity > PhysicsInitialBodyActivity::Sleeping)
                return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown initial body activity."));
            if (descriptor.initialActivity == PhysicsInitialBodyActivity::Sleeping) {
                if (descriptor.body.motion == PhysicsMotionType::Static)
                    return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Static bodies cannot start sleeping."));
                if (!sleepingEnabled)
                    return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "World sleeping is disabled."));
                if (!IsZeroVelocity(descriptor.body.linearVelocity) || !IsZeroVelocity(descriptor.body.angularVelocity))
                    return Result<void>::Failure(
                        MakeError(PhysicsErrors::DescriptorInvalid, "A sleeping body must start with zero linear and angular velocity."));
            }
            return Result<void>::Success();
        }

        /** @brief Prepares native admission settings without allocating or activating a body. */
        [[nodiscard]] Result<JPH::BodyCreationSettings> PrepareSceneBodySettings(const PhysicsSceneBodyDescriptor &descriptor,
                                                                                 const CanonicalSceneShapeRecord &shape) {
            JPH::BodyCreationSettings settings(shape.shape.GetPtr(), ToNativePoint(descriptor.body.pose.translation),
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
            settings.mAllowDynamicOrKinematic = !shape.shape->MustBeStatic();
            if (const Result<void> mass = ApplyCanonicalMassPolicy(settings, descriptor.body.mass); mass.HasError())
                return Result<JPH::BodyCreationSettings>::Failure(mass.ErrorValue());
            return Result<JPH::BodyCreationSettings>::Success(std::move(settings));
        }
    }  // namespace

    /** @copydoc ApplyCanonicalMassPolicy */
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
        if (const auto activity = ValidateInitialBodyActivity(descriptor, canonical.native.system->GetPhysicsSettings().mAllowSleeping);
            activity.HasError())
            return Result<BodyHandle>::Failure(activity.ErrorValue());
        const auto shape = std::ranges::find_if(canonical.scene.shapes, [&descriptor](const auto &candidate) {
            return candidate.handle == descriptor.body.shape;
        });
        if (shape == canonical.scene.shapes.end())
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::HandleStale));
        auto prepared = PrepareSceneBodySettings(descriptor, *shape);
        if (prepared.HasError())
            return Result<BodyHandle>::Failure(prepared.ErrorValue());
        const auto &settings = prepared.Value();

        const bool awake =
            descriptor.body.motion != PhysicsMotionType::Static && descriptor.initialActivity == PhysicsInitialBodyActivity::Awake;
        const JPH::EActivation activation = awake ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
        const JPH::BodyID nativeBody = canonical.native.system->GetBodyInterface().CreateAndAddBody(settings, activation);
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

}  // namespace Horo::Physics::Detail
