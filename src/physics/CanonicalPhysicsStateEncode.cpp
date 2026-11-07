#include "CanonicalPhysicsStateEncode.h"

#include <Jolt/Physics/Body/BodyID.h>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief No native body ID can be invented by an external state packet. */
        Result<void> Bodies(CanonicalPhysicsStateStream &stream, const CanonicalPhysicsStateBindings &bindings,
                            const PhysicsStateSolverView &state) {
            if (state.bodies.size() != bindings.stableBodies.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            auto count = static_cast<std::uint32_t>(state.bodies.size());
            stream.Write(count);
            CanonicalPhysicsStateFields fields{stream, false};
            for (std::size_t index = 0; index < state.bodies.size(); ++index) {
                auto body = state.bodies[index];
                const auto &binding = bindings.stableBodies[index];
                if (body.id != binding.id || body.hasMotion != binding.hasMotion)
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                stream.Write(JPH::BodyID{binding.nativeId});
                fields.Value(body.active);
                CanonicalBodyFields(fields, body);
            }
            return Result<void>::Success();
        }

        /** @brief Exact joint kind and native manager index come only from the actual
         * candidate's retained inventory. */
        Result<void> Joints(CanonicalPhysicsStateStream &stream, const CanonicalPhysicsStateBindings &bindings,
                            const PhysicsStateSolverView &state) {
            if (state.constraints.size() != bindings.stableJoints.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            auto count = static_cast<std::uint32_t>(state.constraints.size());
            stream.Write(count);
            CanonicalPhysicsStateFields fields{stream, false};
            for (std::size_t index = 0; index < state.constraints.size(); ++index) {
                auto joint = state.constraints[index];
                const auto &binding = bindings.stableJoints[index];
                if (joint.id != binding.id || joint.kind != binding.kind)
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                stream.Write(binding.nativeIndex);
                CanonicalJointFields(fields, joint);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EncodeCanonicalPhysicsState */
    Result<void> EncodeCanonicalPhysicsState(CanonicalPhysicsStateStream &stream, const CanonicalPhysicsStateBindings &bindings,
                                             const PhysicsStateSolverView &state) {
        stream.Clear();
        stream.Write(JPH::EStateRecorderState::All);
        auto global = state;
        CanonicalPhysicsStateFields fields{stream, false};
        fields.Value(global.previousDelta);
        fields.Vector(global.gravity);
        if (auto bodies = Bodies(stream, bindings, state); bodies.HasError())
            return bodies;
        if (auto contacts = EncodeCanonicalPhysicsContacts(stream, bindings, state); contacts.HasError())
            return contacts;
        if (auto joints = Joints(stream, bindings, state); joints.HasError())
            return joints;
        if (stream.IsFailed())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        stream.Rewind();
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
