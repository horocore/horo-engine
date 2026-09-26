#include "CanonicalPhysicsRuntimeInternal.h"

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Translates the solver's observed motion mode to the Horo policy enum. */
        [[nodiscard]] Result<PhysicsMotionType> ReadNativeMotion(const JPH::Body &native) {
            switch (native.GetMotionType()) {
                case JPH::EMotionType::Static:
                    return Result<PhysicsMotionType>::Success(PhysicsMotionType::Static);
                case JPH::EMotionType::Kinematic:
                    return Result<PhysicsMotionType>::Success(PhysicsMotionType::Kinematic);
                case JPH::EMotionType::Dynamic:
                    return Result<PhysicsMotionType>::Success(PhysicsMotionType::Dynamic);
                default:
                    return Result<PhysicsMotionType>::Failure(
                        MakeError(PhysicsErrors::SolverFatalCondition, "Native body has an unknown motion mode."));
            }
        }

        /** @brief Reads dynamic inverse mass without inventing mass for locked translation. */
        [[nodiscard]] Result<std::optional<float>> ReadNativeMass(const JPH::Body &native, const PhysicsMotionType motion) {
            if (motion != PhysicsMotionType::Dynamic)
                return Result<std::optional<float>>::Success(std::nullopt);
            const float inverseMass = native.GetMotionProperties()->GetInverseMass();
            if (!std::isfinite(inverseMass) || inverseMass < 0.0F)
                return Result<std::optional<float>>::Failure(
                    MakeError(PhysicsErrors::SolverFatalCondition, "Native dynamic body has invalid mass."));
            return Result<std::optional<float>>::Success(inverseMass > 0.0F ? std::optional<float>{1.0F / inverseMass} : std::nullopt);
        }
    }  // namespace

    /** @copydoc ReadCanonicalSceneBodyReconciliation */
    Result<PhysicsBodyReconciliation> ReadCanonicalSceneBodyReconciliation(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                                           const BodyHandle body) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<PhysicsBodyReconciliation>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (const auto handle = ValidatePhysicsHandleOwner(body, owner); handle.HasError())
            return Result<PhysicsBodyReconciliation>::Failure(handle.ErrorValue());
        const auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto record = std::ranges::find_if(canonical.scene.bodies, [body](const auto &candidate) {
            return candidate.handle == body;
        });
        if (record == canonical.scene.bodies.end())
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
        const auto motion = ReadNativeMotion(native);
        if (motion.HasError())
            return Result<PhysicsBodyReconciliation>::Failure(motion.ErrorValue());
        const auto mass = ReadNativeMass(native, motion.Value());
        if (mass.HasError())
            return Result<PhysicsBodyReconciliation>::Failure(mass.ErrorValue());
        PhysicsBodyReconciliation result{.policy = record->policy,
                                         .state = {.body = body,
                                                   .pose = {.translation = {position.GetX(), position.GetY(), position.GetZ()},
                                                            .rotation = {rotation.GetX(), rotation.GetY(), rotation.GetZ(),
                                                                         rotation.GetW()}},
                                                   .linearVelocity = {linear.GetX(), linear.GetY(), linear.GetZ()},
                                                   .angularVelocity = {angular.GetX(), angular.GetY(), angular.GetZ()},
                                                   .activity =
                                                       native.IsActive() ? PhysicsBodyActivity::Awake : PhysicsBodyActivity::Sleeping},
                                         .observedMotion = motion.Value(),
                                         .observedShape = shape->handle,
                                         .observedMassKilograms = mass.Value(),
                                         .observedBoundsExtent = {boundsExtent.GetX(), boundsExtent.GetY(), boundsExtent.GetZ()}};
        return Result<PhysicsBodyReconciliation>::Success(std::move(result));
    }
}  // namespace Horo::Physics::Detail
