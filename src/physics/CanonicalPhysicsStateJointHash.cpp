#include "CanonicalPhysicsStateTopologyHash.h"

#include <type_traits>

namespace Horo::Physics::Detail {
    namespace {
        void Creation(StateDetail::StateHash &hash, const CanonicalConstraintCreation &value) noexcept {
            hash.Pose(value.firstFrame);
            hash.Pose(value.secondFrame);
            for (float scalar : value.first.centerOfMass)
                hash.Float(scalar);
            for (float scalar : value.first.rotation)
                hash.Float(scalar);
            for (float scalar : value.second.centerOfMass)
                hash.Float(scalar);
            for (float scalar : value.second.rotation)
                hash.Float(scalar);
            hash.Word(static_cast<std::uint8_t>(value.floatingPoint.architecture));
            hash.Word(value.floatingPoint.value);
        }

        bool Endpoints(StateDetail::StateHash &hash, const CanonicalSceneConstraintRecord &record,
                       const CanonicalPhysicsStateBindings &bindings) noexcept {
            const auto *first = FindNativeStateBody(bindings, record.firstBody.GetIndexAndSequenceNumber());
            if (!first || first->body != record.first || record.descriptor.first.body != record.first)
                return false;
            hash.Word(first->id);
            hash.Pose(record.descriptor.first.localFrame);
            hash.Word(record.descriptor.second.index());
            if (const auto *anchor = std::get_if<PhysicsBodyAnchor>(&record.descriptor.second)) {
                const auto *second = FindNativeStateBody(bindings, record.secondBody.GetIndexAndSequenceNumber());
                if (!second || second->body != record.second || anchor->body != record.second)
                    return false;
                hash.Word(second->id);
                hash.Pose(anchor->localFrame);
            } else {
                if (!record.secondBody.IsInvalid() || record.second.IsValid())
                    return false;
                hash.Pose(std::get<PhysicsWorldAnchor>(record.descriptor.second).frame);
            }
            return true;
        }

        void Parameters(StateDetail::StateHash &hash, const PhysicsConstraintDescriptor &value) noexcept {
            hash.Word(value.parameters.index());
            hash.Word(static_cast<std::uint8_t>(value.collisionPolicy));
            std::visit([&](const auto &parameters) {
                using Type = std::decay_t<decltype(parameters)>;
                if constexpr (std::is_same_v<Type, PhysicsHingeConstraint>) {
                    hash.Float(parameters.minimumRadians);
                    hash.Float(parameters.maximumRadians);
                } else if constexpr (!std::is_same_v<Type, PhysicsFixedConstraint>) {
                    hash.Float(parameters.minimumMeters);
                    hash.Float(parameters.maximumMeters);
                }
            }, value.parameters);
        }
    }  // namespace

    /** @copydoc HashStateJoint */
    Result<void> HashStateJoint(StateDetail::StateHash &hash, const CanonicalWorld &world, const CanonicalPhysicsStateJointBinding &binding,
                                std::size_t index, const CanonicalPhysicsStateBindings &bindings) {
        if (index >= world.scene.constraints.size())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        const auto &record = world.scene.constraints[index];
        if (record.handle != binding.constraint || !record.constraint || record.collisionPolicy != record.descriptor.collisionPolicy)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        hash.Word(binding.id);
        hash.Word(static_cast<std::uint8_t>(binding.kind));
        if (!Endpoints(hash, record, bindings))
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        Parameters(hash, record.descriptor);
        Creation(hash, record.creation);
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
