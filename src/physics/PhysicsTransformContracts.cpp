#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsTransformAuthority.h"

#include <cmath>
#include <utility>

namespace Horo::Physics {
    namespace {
        /** @brief Checks one closed static-update policy. */
        [[nodiscard]] bool IsKnownStaticPolicy(const PhysicsStaticTransformUpdatePolicy policy) noexcept {
            return policy == PhysicsStaticTransformUpdatePolicy::UpdateBroadphase || policy == PhysicsStaticTransformUpdatePolicy::Rebuild;
        }

        /** @brief Checks one closed dynamic operation. */
        [[nodiscard]] bool IsKnownDynamicOperation(const PhysicsDynamicTransformOperation operation) noexcept {
            return operation == PhysicsDynamicTransformOperation::Teleport || operation == PhysicsDynamicTransformOperation::Reset;
        }

        /** @brief Checks one closed teleport velocity policy. */
        [[nodiscard]] bool IsKnownVelocityPolicy(const PhysicsTeleportVelocityPolicy policy) noexcept {
            return policy == PhysicsTeleportVelocityPolicy::Preserve || policy == PhysicsTeleportVelocityPolicy::Reset;
        }

        /** @brief Confirms a typed command's shared identity and exact consuming frame. */
        Result<void> ValidateCommandIdentity(const PhysicsTransformCommandIdentity &identity, const PhysicsWorldId expectedWorld,
                                             const std::uint64_t expectedSceneGeneration, const std::uint64_t expectedSimulationTick) {
            if (expectedSceneGeneration == 0 || expectedSimulationTick == 0)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Transform admission frame is invalid."));
            if (identity.protocolVersion != PhysicsTransformAuthorityProtocolVersion || identity.simulationTick == 0 ||
                identity.sceneGeneration == 0 || identity.sourceSequence == 0 || !identity.source.IsValid())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::CommandOrderInvalid, "Transform command identity or ordering evidence is incomplete."));
            if (const Result<void> owner = ValidatePhysicsHandleOwner(identity.body, expectedWorld); owner.HasError())
                return owner;
            if (identity.sceneGeneration != expectedSceneGeneration || identity.simulationTick != expectedSimulationTick)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::CommandOrderInvalid, "Transform command targets another fixed-tick admission frame."));
            return Result<void>::Success();
        }

        /** @brief Validates the shared admission frame before checking one command payload. */
        template <typename Command, typename PayloadValidator>
        Result<void> ValidateTransformCommand(const Command &command, const PhysicsWorldId expectedWorld,
                                              const std::uint64_t expectedSceneGeneration, const std::uint64_t expectedSimulationTick,
                                              PayloadValidator &&validatePayload) {
            if (const Result<void> identity =
                    ValidateCommandIdentity(command.identity, expectedWorld, expectedSceneGeneration, expectedSimulationTick);
                identity.HasError())
                return identity;
            return std::forward<PayloadValidator>(validatePayload)(command);
        }

        /** @brief Validates a static pose and its explicit broadphase/rebuild policy. */
        Result<void> ValidateStaticTransformPayload(const PhysicsStaticTransformCommand &command) {
            if (const Result<void> pose = ValidatePhysicsPose(command.authoredPose); pose.HasError())
                return pose;
            if (!IsKnownStaticPolicy(command.updatePolicy))
                return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown static transform update policy."));
            return Result<void>::Success();
        }

        /** @brief Validates a kinematic target pose. */
        Result<void> ValidateKinematicTransformPayload(const PhysicsKinematicTargetCommand &command) {
            return ValidatePhysicsPose(command.targetPose);
        }

        /** @brief Validates a dynamic target pose and its explicit operation policies. */
        Result<void> ValidateDynamicTransformPayload(const PhysicsDynamicTransformCommand &command) {
            if (const Result<void> pose = ValidatePhysicsPose(command.targetPose); pose.HasError())
                return pose;
            if (!IsKnownDynamicOperation(command.operation) || !IsKnownVelocityPolicy(command.velocityPolicy))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Unknown dynamic transform operation or velocity policy."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EvaluatePhysicsInterpolation */
    Result<PhysicsPose> EvaluatePhysicsInterpolation(const PhysicsInterpolationEndpoints &endpoints, const float alpha) {
        if (endpoints.sceneGeneration == 0 || endpoints.originGeneration == 0 || endpoints.currentTick == 0 || !std::isfinite(alpha) ||
            alpha < 0.0F || alpha > 1.0F ||
            (endpoints.hasPreviousTick && (endpoints.previousTick == 0 || endpoints.previousTick >= endpoints.currentTick ||
                                           endpoints.currentTick - endpoints.previousTick != 1)))
            return Result<PhysicsPose>::Failure(MakeError(PhysicsErrors::DescriptorInvalid,
                                                          "Interpolation requires a completed consecutive tick pair and alpha in [0, 1]."));
        if (const auto handle = ValidatePhysicsHandleOwner(endpoints.body, endpoints.body.world); handle.HasError())
            return Result<PhysicsPose>::Failure(handle.ErrorValue());
        if (const auto valid = ValidatePhysicsPose(endpoints.currentPose); valid.HasError())
            return Result<PhysicsPose>::Failure(valid.ErrorValue());
        if (!endpoints.hasPreviousTick)
            return Result<PhysicsPose>::Success(endpoints.currentPose);
        if (const auto valid = ValidatePhysicsPose(endpoints.previousPose); valid.HasError())
            return Result<PhysicsPose>::Failure(valid.ErrorValue());
        return Result<PhysicsPose>::Success(
            {.translation = Math::Lerp(endpoints.previousPose.translation, endpoints.currentPose.translation, alpha),
             .rotation = Math::Slerp(endpoints.previousPose.rotation, endpoints.currentPose.rotation, alpha)});
    }

    /** @copydoc ResolvePhysicsTransformAuthority */
    Result<PhysicsTransformAuthority> ResolvePhysicsTransformAuthority(const PhysicsMotionType motion) {
        using enum PhysicsMotionType;
        using enum PhysicsTransformAuthority;
        switch (motion) {
            case Static:
                return Result<PhysicsTransformAuthority>::Success(StaticScene);
            case Kinematic:
                return Result<PhysicsTransformAuthority>::Success(KinematicTarget);
            case Dynamic:
                return Result<PhysicsTransformAuthority>::Success(DynamicSolver);
        }
        return Result<PhysicsTransformAuthority>::Failure(
            MakeError(PhysicsErrors::OperationUnsupported, "Unknown body motion mode has no transform authority."));
    }

    /** @copydoc ValidatePhysicsTransformCommandIdentity */
    Result<void> ValidatePhysicsTransformCommandIdentity(const PhysicsTransformCommandIdentity &identity,
                                                         const PhysicsWorldId expectedWorld, const std::uint64_t expectedSceneGeneration,
                                                         const std::uint64_t expectedSimulationTick) {
        return ValidateCommandIdentity(identity, expectedWorld, expectedSceneGeneration, expectedSimulationTick);
    }

    /** @copydoc ValidatePhysicsStaticTransformCommand */
    Result<void> ValidatePhysicsStaticTransformCommand(const PhysicsStaticTransformCommand &command, const PhysicsWorldId expectedWorld,
                                                       const std::uint64_t expectedSceneGeneration,
                                                       const std::uint64_t expectedSimulationTick) {
        return ValidateTransformCommand(command, expectedWorld, expectedSceneGeneration, expectedSimulationTick,
                                        ValidateStaticTransformPayload);
    }

    /** @copydoc ValidatePhysicsKinematicTargetCommand */
    Result<void> ValidatePhysicsKinematicTargetCommand(const PhysicsKinematicTargetCommand &command, const PhysicsWorldId expectedWorld,
                                                       const std::uint64_t expectedSceneGeneration,
                                                       const std::uint64_t expectedSimulationTick) {
        return ValidateTransformCommand(command, expectedWorld, expectedSceneGeneration, expectedSimulationTick,
                                        ValidateKinematicTransformPayload);
    }

    /** @copydoc ValidatePhysicsDynamicTransformCommand */
    Result<void> ValidatePhysicsDynamicTransformCommand(const PhysicsDynamicTransformCommand &command, const PhysicsWorldId expectedWorld,
                                                        const std::uint64_t expectedSceneGeneration,
                                                        const std::uint64_t expectedSimulationTick) {
        return ValidateTransformCommand(command, expectedWorld, expectedSceneGeneration, expectedSimulationTick,
                                        ValidateDynamicTransformPayload);
    }

    /** @copydoc ValidatePhysicsDynamicTransformSnapshot */
    Result<void> ValidatePhysicsDynamicTransformSnapshot(const PhysicsDynamicTransformSnapshot &snapshot,
                                                         const PhysicsWorldId expectedWorld, const std::uint64_t expectedSceneGeneration,
                                                         const std::uint64_t expectedCompletedTick) {
        if (expectedSceneGeneration == 0 || expectedCompletedTick == 0 ||
            snapshot.protocolVersion != PhysicsTransformAuthorityProtocolVersion || snapshot.completedTick == 0 ||
            snapshot.sceneGeneration == 0)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Dynamic transform snapshot metadata is invalid."));
        if (const Result<void> owner = ValidatePhysicsHandleOwner(snapshot.state.body, expectedWorld); owner.HasError())
            return owner;
        if (snapshot.completedTick != expectedCompletedTick || snapshot.sceneGeneration != expectedSceneGeneration)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::QuerySnapshotStale, "Dynamic transform snapshot is not from the completed admission tick."));
        if (snapshot.state.activity == PhysicsBodyActivity::Static)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "A dynamic snapshot cannot describe static activity."));
        return ValidatePhysicsBodyState(snapshot.state, expectedWorld);
    }

    /** @copydoc ValidatePhysicsDirectTransformWrite */
    Result<void> ValidatePhysicsDirectTransformWrite(const PhysicsDirectTransformWrite &write, const PhysicsWorldId expectedWorld,
                                                     const std::uint64_t expectedSceneGeneration) {
        if (expectedSceneGeneration == 0 || write.protocolVersion != PhysicsTransformAuthorityProtocolVersion || write.sceneGeneration == 0)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Direct transform write metadata is invalid."));
        if (const Result<void> owner = ValidatePhysicsHandleOwner(write.body, expectedWorld); owner.HasError())
            return owner;
        if (write.sceneGeneration != expectedSceneGeneration)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::CommandOrderInvalid, "Direct transform write targets another scene generation."));
        return ValidatePhysicsPose(write.pose);
    }
}  // namespace Horo::Physics
