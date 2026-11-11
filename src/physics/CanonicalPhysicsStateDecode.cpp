#include "CanonicalPhysicsStateDecode.h"

#include "PhysicsStateValidationInternal.h"

#include <Jolt/Physics/Body/BodyID.h>

namespace Horo::Physics::Detail {
    namespace {
        using namespace StateDetail;

        /** @brief The private native recorder must contain exactly every resident bound
         * body. */
        Result<std::uint32_t> Bodies(CanonicalPhysicsStateStream &stream, const CanonicalPhysicsStateBindings &bindings,
                                     SolverStorage &storage) {
            std::uint32_t count{};
            stream.Read(count);
            if (count != bindings.nativeBodies.size() || count > storage.bodies.size())
                return Result<std::uint32_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            CanonicalPhysicsStateFields fields{stream, true};
            for (auto &body : std::span{storage.bodies}.first(count)) {
                JPH::BodyID nativeId;
                stream.Read(nativeId);
                const auto *binding = FindNativeStateBody(bindings, nativeId.GetIndexAndSequenceNumber());
                if (!binding)
                    return Result<std::uint32_t>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                body = {.id = binding->id, .hasMotion = binding->hasMotion};
                fields.Value(body.active);
                CanonicalBodyFields(fields, body);
            }
            auto bodies = std::span{storage.bodies}.first(count);
            std::ranges::sort(bodies, {}, &PhysicsStateBody::id);
            return Result<std::uint32_t>::Success(count);
        }

        /** @brief Constraint indexes are checked against the exact retained native
         * manager, not guessed from Horo order. */
        Result<std::uint32_t> Joints(CanonicalPhysicsStateStream &stream, const CanonicalPhysicsStateBindings &bindings,
                                     SolverStorage &storage) {
            std::uint32_t count{};
            stream.Read(count);
            if (count != bindings.nativeJoints.size() || count > storage.constraints.size())
                return Result<std::uint32_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            CanonicalPhysicsStateFields fields{stream, true};
            for (std::uint32_t index = 0; index < count; ++index) {
                std::uint32_t nativeIndex{};
                fields.Value(nativeIndex);
                if (nativeIndex >= bindings.nativeJoints.size())
                    return Result<std::uint32_t>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                const auto &binding = bindings.nativeJoints[nativeIndex];
                auto &joint = storage.constraints[index];
                joint = {.id = binding.id, .kind = binding.kind};
                CanonicalJointFields(fields, joint);
            }
            std::ranges::sort(std::span{storage.constraints}.first(count), {}, &PhysicsStateConstraint::id);
            return Result<std::uint32_t>::Success(count);
        }

        /** @brief Active-body order is semantic native state; it is copied unchanged
         * under stable identity mapping. */
        Result<void> ActiveOrder(const CanonicalPhysicsStateBindings &bindings, SolverStorage &staging,
                                 std::span<const std::uint32_t> activeNativeBodyOrder) {
            if (activeNativeBodyOrder.size() > staging.activeBodyOrder.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            for (std::size_t index = 0; index < activeNativeBodyOrder.size(); ++index) {
                const auto *binding = FindNativeStateBody(bindings, activeNativeBodyOrder[index]);
                if (!binding)
                    return Invalid("Physics native active order escaped its retained body inventory.");
                staging.activeBodyOrder[index] = binding->id;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc DecodeCanonicalPhysicsState */
    Result<PhysicsStateSolverView> DecodeCanonicalPhysicsState(CanonicalPhysicsStateStream &stream,
                                                               const CanonicalPhysicsStateBindings &bindings,
                                                               StateDetail::SolverStorage &staging,
                                                               std::span<const std::uint32_t> activeNativeBodyOrder) {
        stream.Rewind();
        auto selection = JPH::EStateRecorderState::None;
        stream.Read(selection);
        if (selection != JPH::EStateRecorderState::All)
            return Result<PhysicsStateSolverView>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        PhysicsStateSolverView candidate;
        CanonicalPhysicsStateFields fields{stream, true};
        fields.Value(candidate.previousDelta);
        fields.Vector(candidate.gravity);
        auto bodies = Bodies(stream, bindings, staging);
        if (bodies.HasError())
            return Result<PhysicsStateSolverView>::Failure(bodies.ErrorValue());
        std::uint32_t pairCount{};
        std::uint32_t manifoldCount{};
        std::uint32_t ccdCount{};
        if (auto contacts = DecodeCanonicalPhysicsContacts(stream, bindings, staging, pairCount, manifoldCount, ccdCount);
            contacts.HasError())
            return Result<PhysicsStateSolverView>::Failure(contacts.ErrorValue());
        auto joints = Joints(stream, bindings, staging);
        if (joints.HasError())
            return Result<PhysicsStateSolverView>::Failure(joints.ErrorValue());
        if (auto active = ActiveOrder(bindings, staging, activeNativeBodyOrder); active.HasError())
            return Result<PhysicsStateSolverView>::Failure(active.ErrorValue());
        candidate.bodies = std::span{staging.bodies}.first(bodies.Value());
        candidate.activeBodyOrder = std::span{staging.activeBodyOrder}.first(activeNativeBodyOrder.size());
        candidate.constraints = std::span{staging.constraints}.first(joints.Value());
        candidate.pairs = std::span{staging.pairs}.first(pairCount);
        candidate.manifolds = std::span{staging.manifolds}.first(manifoldCount);
        candidate.ccdManifolds = std::span{staging.ccdManifolds}.first(ccdCount);
        if (stream.IsFailed() || !stream.IsEOF())
            return Result<PhysicsStateSolverView>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (auto valid = ValidatePhysicsStateSolver(candidate, staging.limits, staging.activeBodyScratch); valid.HasError())
            return Result<PhysicsStateSolverView>::Failure(valid.ErrorValue());
        return Result<PhysicsStateSolverView>::Success(candidate);
    }
}  // namespace Horo::Physics::Detail
