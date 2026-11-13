#include "CanonicalPhysicsStateEncode.h"

#include <Jolt/Physics/Body/BodyPair.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Retained topology resolution preserves pair endpoint meaning rather
         * than swapping keys by numeric ID. */
        bool PairIds(const CanonicalPhysicsStateBindings &bindings, PhysicsStateObjectId first, PhysicsStateObjectId second, JPH::BodyID &a,
                     JPH::BodyID &b) noexcept {
            const auto *one = FindStableStateBody(bindings, first);
            const auto *two = FindStableStateBody(bindings, second);
            if (!one || !two)
                return false;
            a = JPH::BodyID{one->nativeId};
            b = JPH::BodyID{two->nativeId};
            return true;
        }

        /** @brief Only exact stable binding resolution supplies native key fields. */
        bool Key(CanonicalPhysicsStateStream &stream, const CanonicalPhysicsStateBindings &bindings, const PhysicsStateManifoldKey &key) {
            JPH::BodyID first;
            JPH::BodyID second;
            if (!PairIds(bindings, key.first, key.second, first, second))
                return false;
            JPH::SubShapeID a;
            JPH::SubShapeID b;
            a.SetValue(key.firstSubshape);
            b.SetValue(key.secondSubshape);
            stream.Write(JPH::SubShapeIDPair{first, a, second, b});
            return true;
        }

        /** @brief Complete pinned native manifold grammar, including zero-contact
         * sensor manifolds. */
        bool Manifold(CanonicalPhysicsStateStream &stream, const CanonicalPhysicsStateBindings &bindings,
                      const PhysicsStateManifold &input) {
            if (!Key(stream, bindings, input.key))
                return false;
            auto value = input;
            stream.Write(static_cast<std::uint16_t>(value.pointCount));
            CanonicalPhysicsStateFields fields{stream, false};
            fields.Float3(value.normal);
            fields.Lambda2(value.frictionLambda);
            fields.Value(value.angularFrictionLambda);
            for (auto &point : std::span{value.points}.first(value.pointCount)) {
                fields.Float3(point.position1);
                fields.Float3(point.position2);
                fields.Value(point.normalLambda);
            }
            return !fields.Failed();
        }

        /** @brief Pair flat ranges have already passed complete typed validation before
         * native bridge encoding. */
        bool Pair(CanonicalPhysicsStateStream &stream, const CanonicalPhysicsStateBindings &bindings, const PhysicsStateSolverView &state,
                  const PhysicsStateBodyPair &input) {
            JPH::BodyID first;
            JPH::BodyID second;
            if (!PairIds(bindings, input.first, input.second, first, second))
                return false;
            stream.Write(JPH::BodyPair{first, second});
            auto value = input;
            CanonicalPhysicsStateFields fields{stream, false};
            fields.Float3(value.relativePosition);
            fields.Float3(value.relativeRotation);
            fields.Value(value.manifoldCount);
            for (const auto &manifold : state.manifolds.subspan(value.manifoldBegin, value.manifoldCount))
                if (!Manifold(stream, bindings, manifold))
                    return false;
            return !fields.Failed();
        }
    }  // namespace

    /** @copydoc EncodeCanonicalPhysicsContacts */
    Result<void> EncodeCanonicalPhysicsContacts(CanonicalPhysicsStateStream &stream, const CanonicalPhysicsStateBindings &bindings,
                                                const PhysicsStateSolverView &state) {
        stream.Write(static_cast<std::uint32_t>(state.pairs.size()));
        for (const auto &pair : state.pairs)
            if (!Pair(stream, bindings, state, pair))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        stream.Write(static_cast<std::uint32_t>(state.ccdManifolds.size()));
        for (const auto &key : state.ccdManifolds)
            if (!Key(stream, bindings, key))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
