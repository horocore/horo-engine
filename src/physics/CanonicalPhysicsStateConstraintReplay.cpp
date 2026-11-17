#include "CanonicalPhysicsStateConstraintReplay.h"

#include "CanonicalPhysicsStateFields.h"
#include "CanonicalPhysicsStateFpControl.h"

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Trusted complete native body grammar is decoded on fixed stack
         * storage before any temporary pose change. */
        Result<PhysicsStateBody> BodyState(JPH::Body &body) {
            std::array<std::byte, 192> bytes{};
            CanonicalPhysicsStateStream stream{bytes};
            body.SaveState(stream);
            if (stream.IsFailed())
                return Result<PhysicsStateBody>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            PhysicsStateBody state;
            state.hasMotion = body.GetMotionPropertiesUnchecked() != nullptr;
            CanonicalPhysicsStateFields fields{stream, true};
            CanonicalBodyFields(fields, state);
            if (fields.Failed() || !stream.IsEOF())
                return Result<PhysicsStateBody>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            return Result<PhysicsStateBody>::Success(state);
        }

        /** @brief Public Body::RestoreState writes exact COM/quaternion components
         * without origin subtraction or quaternion reconstruction.
         */
        Result<void> CreationFrame(JPH::Body &body, const CanonicalConstraintEndpointCreation &creation) {
            auto captured = BodyState(body);
            if (captured.HasError())
                return Result<void>::Failure(captured.ErrorValue());
            auto state = std::move(captured).Value();
            state.centerOfMass = creation.centerOfMass;
            state.rotation = creation.rotation;
            std::array<std::byte, 192> bytes{};
            CanonicalPhysicsStateStream stream{bytes};
            CanonicalPhysicsStateFields fields{stream, false};
            CanonicalBodyFields(fields, state);
            if (fields.Failed())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            body.RestoreState(stream);
            if (stream.IsFailed() || !stream.IsEOF())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateCanonicalConstraintCreationProfile */
    Result<void> ValidateCanonicalConstraintCreationProfile(const CanonicalWorld &world) {
        auto profile = ReadCanonicalPhysicsStateFpControl();
        if (profile.HasError())
            return Result<void>::Failure(profile.ErrorValue());
        for (const auto &constraint : world.scene.constraints)
            if (constraint.creation.floatingPoint != profile.Value())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable,
                                                       "Physics constraint creation floating-point controls "
                                                       "differ from the current closed profile."));
        return Result<void>::Success();
    }

    /** @copydoc ReplayCanonicalConstraintCreation */
    Result<JPH::Ref<JPH::Constraint>> ReplayCanonicalConstraintCreation(const CanonicalSceneConstraintRecord &source, JPH::Body &first,
                                                                        JPH::Body &second) {
        return ReplayCanonicalConstraintCreation(source.creation, source.descriptor, !source.secondBody.IsInvalid(), first, second);
    }

    /** @copydoc ReplayCanonicalConstraintCreation */
    Result<JPH::Ref<JPH::Constraint>> ReplayCanonicalConstraintCreation(const CanonicalConstraintCreation &creation,
                                                                        const PhysicsConstraintDescriptor &descriptor, bool hasSecondBody,
                                                                        JPH::Body &first, JPH::Body &second) {
        using Outcome = Result<JPH::Ref<JPH::Constraint>>;
        if (auto frame = CreationFrame(first, creation.first); frame.HasError())
            return Outcome::Failure(frame.ErrorValue());
        if (hasSecondBody)
            if (auto frame = CreationFrame(second, creation.second); frame.HasError())
                return Outcome::Failure(frame.ErrorValue());
        return BuildCanonicalSceneConstraint(first, second, creation.firstFrame, creation.secondFrame, descriptor.parameters);
    }
}  // namespace Horo::Physics::Detail
