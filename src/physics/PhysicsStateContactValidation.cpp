#include "PhysicsStateValidationInternal.h"

namespace Horo::Physics::StateDetail {
    namespace {
        /** @brief Validates full four-point native state; gameplay summaries are not a
         * replacement. */
        bool ValidPoints(const PhysicsStateManifold &manifold) noexcept {
            if (manifold.pointCount > manifold.points.size())
                return false;
            for (std::size_t index = 0; index < manifold.points.size(); ++index) {
                const auto &point = manifold.points[index];
                if (index >= manifold.pointCount) {
                    if (point != PhysicsStateContactPoint{})
                        return false;
                } else if (!Finite(point.position1) || !Finite(point.position2) || !std::isfinite(point.normalLambda) ||
                           point.normalLambda < 0)
                    return false;
            }
            return true;
        }

        /** @brief Compressed rotations are finite unit-ball coordinates; their fourth
         * component is implicit nonnegative w. */
        bool CompressedRotation(const std::array<float, 3> &rotation) noexcept {
            double squared{};
            for (float value : rotation)
                squared += static_cast<double>(value) * value;
            return Finite(rotation) && squared <= 1.00001;
        }

        /** @brief Cached local contact normal is a required unit direction, not an
         * arbitrary finite vector. */
        bool Normal(const std::array<float, 3> &normal) noexcept {
            double squared{};
            for (float value : normal)
                squared += static_cast<double>(value) * value;
            return Finite(normal) && std::abs(squared - 1.0) <= 1.0e-4;
        }

        /** @brief One manifold's pair ownership and complete numeric payload are
         * checked before native cache allocation. */
        bool ValidManifold(const PhysicsStateManifold &manifold, const PhysicsStateBodyPair &pair) noexcept {
            return manifold.key.first == pair.first && manifold.key.second == pair.second && Normal(manifold.normal) &&
                   Finite(manifold.frictionLambda) && std::isfinite(manifold.angularFrictionLambda) && ValidPoints(manifold);
        }

        /** @brief Range is validated by subtraction before indexing any flat manifold
         * storage. */
        bool ValidRange(const PhysicsStateBodyPair &pair, std::size_t next, std::size_t size) noexcept {
            return pair.manifoldBegin == next && next <= size && pair.manifoldCount <= size - next;
        }

        /** @brief Separates pair identity and geometry admission from flat range
         * traversal. */
        bool ValidPair(const PhysicsStateSolverView &state, const PhysicsStateBodyPair &pair,
                       std::pair<PhysicsStateObjectId, PhysicsStateObjectId> previous) noexcept {
            const std::pair key{pair.first, pair.second};
            return previous < key && PairExists(state.bodies, pair.first, pair.second) && Finite(pair.relativePosition) &&
                   CompressedRotation(pair.relativeRotation);
        }

        /** @brief Manifold keys are canonical within their real body pair; duplicates
         * are never merged. */
        bool ValidManifolds(const PhysicsStateSolverView &state, const PhysicsStateBodyPair &pair) noexcept {
            std::optional<PhysicsStateManifoldKey> previous;
            for (const auto &manifold : state.manifolds.subspan(pair.manifoldBegin, pair.manifoldCount)) {
                if (!ValidManifold(manifold, pair) || (previous && !(*previous < manifold.key)))
                    return false;
                previous = manifold.key;
            }
            return true;
        }

        /** @brief CCD contact keys have separate semantics and cannot be folded into
         * warm-start manifolds. */
        bool ValidCcd(const PhysicsStateSolverView &state) noexcept {
            std::optional<PhysicsStateManifoldKey> previous;
            for (const auto &key : state.ccdManifolds) {
                if (!PairExists(state.bodies, key.first, key.second) || (previous && !(*previous < key)))
                    return false;
                if (const auto ordinary = std::ranges::lower_bound(state.manifolds, key, {}, &PhysicsStateManifold::key);
                    ordinary != state.manifolds.end() && ordinary->key == key)
                    return false;
                previous = key;
            }
            return true;
        }
    }  // namespace

    /** @copydoc ValidateContacts */
    Result<void> ValidateContacts(const PhysicsStateSolverView &state) {
        std::pair<PhysicsStateObjectId, PhysicsStateObjectId> previous{};
        std::size_t next{};
        for (const auto &pair : state.pairs) {
            const std::pair key{pair.first, pair.second};
            if (!ValidPair(state, pair, previous))
                return Invalid("Physics snapshot contact pair keys or relative "
                               "transforms are invalid.");
            if (!ValidRange(pair, next, state.manifolds.size()) || !ValidManifolds(state, pair))
                return Invalid("Physics snapshot manifold range, ownership, ordering or "
                               "warm-start values are invalid.");
            next += pair.manifoldCount;
            previous = key;
        }
        if (next != state.manifolds.size() || !ValidCcd(state))
            return Invalid("Physics snapshot contains unowned manifolds or invalid CCD "
                           "cache keys.");
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::StateDetail
