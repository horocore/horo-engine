#include "CanonicalPhysicsStateDecode.h"
#include "PhysicsStateValidationInternal.h"

#include <Jolt/Physics/Body/BodyPair.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>

namespace Horo::Physics::Detail {
    namespace {
        using namespace StateDetail;

        /** @brief Converts actual private body IDs to stable checkpoint keys without
         * serializing native IDs. */
        bool PairIds(const CanonicalPhysicsStateBindings &bindings, JPH::BodyID first, JPH::BodyID second, PhysicsStateObjectId &a,
                     PhysicsStateObjectId &b) noexcept {
            const auto *one = FindNativeStateBody(bindings, first.GetIndexAndSequenceNumber());
            const auto *two = FindNativeStateBody(bindings, second.GetIndexAndSequenceNumber());
            if (!one || !two)
                return false;
            a = one->id;
            b = two->id;
            return true;
        }

        /** @brief Subshape codes are retained only with exact solver/immutable-shape
         * compatibility. */
        bool ManifoldKey(CanonicalPhysicsStateStream &stream, const CanonicalPhysicsStateBindings &bindings, PhysicsStateManifoldKey &key) {
            JPH::SubShapeIDPair native;
            stream.Read(native);
            key.firstSubshape = native.GetSubShapeID1().GetValue();
            key.secondSubshape = native.GetSubShapeID2().GetValue();
            return PairIds(bindings, native.GetBody1ID(), native.GetBody2ID(), key.first, key.second);
        }

        /** @brief Exact pinned contact payload, including every normal/friction
         * warm-start channel. */
        bool Manifold(CanonicalPhysicsStateStream &stream, const CanonicalPhysicsStateBindings &bindings, PhysicsStateManifold &value) {
            value = {};
            if (!ManifoldKey(stream, bindings, value.key))
                return false;
            std::uint16_t points{};
            stream.Read(points);
            if (points > value.points.size())
                return false;
            value.pointCount = points;
            CanonicalPhysicsStateFields fields{stream, true};
            fields.Float3(value.normal);
            fields.Lambda2(value.frictionLambda);
            fields.Value(value.angularFrictionLambda);
            for (auto &point : std::span{value.points}.first(points)) {
                fields.Float3(point.position1);
                fields.Float3(point.position2);
                fields.Value(point.normalLambda);
            }
            return !fields.Failed();
        }

        /** @brief A complete native pair is admitted before its already bounded
         * manifold prefix is filled. */
        bool Pair(CanonicalPhysicsStateStream &stream, const CanonicalPhysicsStateBindings &bindings, SolverStorage &storage,
                  PhysicsStateBodyPair &value, std::uint32_t &manifoldCount) {
            JPH::BodyPair native;
            stream.Read(native);
            value = {};
            if (!PairIds(bindings, native.mBodyA, native.mBodyB, value.first, value.second))
                return false;
            CanonicalPhysicsStateFields fields{stream, true};
            fields.Float3(value.relativePosition);
            fields.Float3(value.relativeRotation);
            fields.Value(value.manifoldCount);
            value.manifoldBegin = manifoldCount;
            if (value.manifoldCount > storage.manifolds.size() - manifoldCount)
                return false;
            for (auto &manifold : std::span{storage.manifolds}.subspan(manifoldCount, value.manifoldCount))
                if (!Manifold(stream, bindings, manifold))
                    return false;
            manifoldCount += value.manifoldCount;
            return !fields.Failed();
        }

        /** @brief Stable body IDs may differ from native allocation order; canonicalize
         * values, never their physical meaning. */
        void Canonicalize(SolverStorage &storage, std::uint32_t pairCount, std::uint32_t manifoldCount, std::uint32_t ccdCount) noexcept {
            auto pairs = std::span{storage.pairs}.first(pairCount);
            auto manifolds = std::span{storage.manifolds}.first(manifoldCount);
            std::ranges::sort(pairs, [](const auto &a, const auto &b) {
                return std::pair{a.first, a.second} < std::pair{b.first, b.second};
            });
            std::ranges::sort(manifolds, {}, &PhysicsStateManifold::key);
            std::ranges::sort(std::span{storage.ccdManifolds}.first(ccdCount));
            std::uint32_t next{};
            for (auto &pair : pairs) {
                pair.manifoldBegin = next;
                next += pair.manifoldCount;
            }
        }
    }  // namespace

    /** @copydoc DecodeCanonicalPhysicsContacts */
    Result<void> DecodeCanonicalPhysicsContacts(CanonicalPhysicsStateStream &stream, const CanonicalPhysicsStateBindings &bindings,
                                                SolverStorage &staging, std::uint32_t &pairCount, std::uint32_t &manifoldCount,
                                                std::uint32_t &ccdCount) {
        pairCount = manifoldCount = ccdCount = 0;
        stream.Read(pairCount);
        if (pairCount > staging.pairs.size())
            return Invalid("Native Physics contact pairs exceed prepared snapshot storage.");
        for (auto &pair : std::span{staging.pairs}.first(pairCount))
            if (!Pair(stream, bindings, staging, pair, manifoldCount))
                return Invalid("Native Physics contact pair or manifold escaped its "
                               "retained schema.");
        stream.Read(ccdCount);
        if (ccdCount > staging.ccdManifolds.size())
            return Invalid("Native Physics CCD contacts exceed prepared snapshot storage.");
        for (auto &key : std::span{staging.ccdManifolds}.first(ccdCount))
            if (!ManifoldKey(stream, bindings, key))
                return Invalid("Native Physics CCD contact escaped its retained topology.");
        Canonicalize(staging, pairCount, manifoldCount, ccdCount);
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
