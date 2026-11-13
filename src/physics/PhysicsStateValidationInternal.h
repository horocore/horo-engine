#pragma once

#include "Horo/Physics/PhysicsStateSnapshot.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace Horo::Physics::StateDetail {
    /** @brief Hard representation ceilings are checked before cold allocation as
     * well as typed admission. */
    inline bool ValidStateLimits(const PhysicsStateLimits &limits) noexcept {
        return limits.maximumBodies <= MaximumPhysicsBodies && limits.maximumConstraints <= MaximumPhysicsConstraints &&
               limits.maximumPairs <= MaximumPhysicsStateCachePairs && limits.maximumManifolds <= MaximumPhysicsStateCacheManifolds &&
               limits.maximumCcdManifolds <= MaximumPhysicsStateCacheManifolds;
    }

    /** @brief Shared typed schema rejection; never invokes native deserialization.
     */
    inline Result<void> Invalid(const char *message) {
        return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, message));
    }

    /** @brief Checks every component without vector arithmetic overflow or hidden
     * SIMD lanes. */
    template <std::size_t N> bool Finite(const std::array<float, N> &values) noexcept {
        return std::ranges::all_of(values, [](float value) {
            return std::isfinite(value);
        });
    }

    /** @brief Ignores signed-zero representation only when checking an unused
     * field's numeric value. */
    template <std::size_t N> bool Zero(const std::array<float, N> &values) noexcept {
        return std::ranges::all_of(values, [](float value) {
            return value == 0.0F;
        });
    }

    /** @brief Required rotations retain exact input bits while validating
     * unit-length representation. */
    inline bool UnitRotation(const std::array<float, 4> &values) noexcept {
        double length{};
        for (float value : values)
            length += static_cast<double>(value) * value;
        return Finite(values) && std::abs(length - 1.0) <= 1.0e-5;
    }

    /** @brief Stable checkpoint body lookup; native IDs and physical array
     * positions have no authority. */
    inline const PhysicsStateBody *FindBody(std::span<const PhysicsStateBody> bodies, PhysicsStateObjectId id) noexcept {
        const auto found = std::ranges::lower_bound(bodies, id, {}, &PhysicsStateBody::id);
        return found != bodies.end() && found->id == id ? &*found : nullptr;
    }

    /** @brief Validates a complete body pair against actual typed body inventory.
     */
    inline bool PairExists(std::span<const PhysicsStateBody> bodies, PhysicsStateObjectId first, PhysicsStateObjectId second) noexcept {
        return first != 0 && first != second && FindBody(bodies, first) && FindBody(bodies, second);
    }

    /** @brief Copies no payload; compares exact object representation ranges only
     * for scratch alias rejection. */
    template <class T> bool Overlaps(std::span<std::uint8_t> scratch, std::span<const T> values) noexcept {
        if (scratch.empty() || values.empty())
            return false;
        const auto a = reinterpret_cast<std::uintptr_t>(scratch.data());
        const auto b = reinterpret_cast<std::uintptr_t>(values.data());
        return a <= b ? b - a < scratch.size_bytes() : a - b < values.size_bytes();
    }

    [[nodiscard]] Result<void> ValidateBodies(std::span<const PhysicsStateBody> bodies);
    [[nodiscard]] Result<void> ValidateConstraints(std::span<const PhysicsStateConstraint> constraints);
    [[nodiscard]] Result<void> ValidateContacts(const PhysicsStateSolverView &state);
}  // namespace Horo::Physics::StateDetail
