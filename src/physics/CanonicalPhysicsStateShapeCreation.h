#pragma once

#include "CanonicalPhysicsStateColdCost.h"
#include "CanonicalPhysicsStateFpControl.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <limits>
#include <optional>

namespace Horo::Physics::Detail {
    /** @brief Actual retained native geometry with conservative immutable
     * native-request reservation. No pointer is a compatibility identity. */
    struct CanonicalPhysicsStateCreatedShape final {
        JPH::Ref<JPH::Shape> shape;
        std::size_t nativeRequestedBytes{};
        std::optional<PhysicsStateFpControl> creationControl;
    };

    /** @brief The pinned synchronous shape/math constructor path contains no FP-control override or worker dispatch.
     * Both boundary samples must agree; unknown arithmetic provenance disables state admission, not ordinary geometry.
     */
    inline std::optional<PhysicsStateFpControl> CanonicalShapeCreationControl(const Result<PhysicsStateFpControl> &before,
                                                                              const Result<PhysicsStateFpControl> &after) noexcept {
        if (before.HasError() || after.HasError() || before.Value() != after.Value())
            return std::nullopt;
        return before.Value();
    }

    /** @brief Pinned inline shape builders have no jobs/worker dispatch; all native
     * allocation requests use this joined owner observer. */
    template <class Create> Result<CanonicalPhysicsStateCreatedShape> ObserveCanonicalPhysicsShapeCreation(Create create) {
        using Outcome = Result<CanonicalPhysicsStateCreatedShape>;
        CanonicalPhysicsStateColdCost cost;
        CanonicalPhysicsStateColdCostScope observation{cost};
        if (!observation.Admitted())
            return Outcome::Failure(MakeError(PhysicsErrors::InvalidState));
        const auto before = ReadCanonicalPhysicsStateFpControl();
        auto created = create();
        const auto after = ReadCanonicalPhysicsStateFpControl();
        if (created.HasError())
            return Outcome::Failure(created.ErrorValue());
        if (cost.overflow)
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        return Outcome::Success({std::move(created).Value(), cost.requestedBytes, CanonicalShapeCreationControl(before, after)});
    }

    /** @brief Retained DAG reservations may conservatively count shared children
     * more than once; arithmetic never wraps. */
    inline bool AddCanonicalPhysicsShapeReservation(std::size_t child, std::size_t &reservation) noexcept {
        if (child > std::numeric_limits<std::size_t>::max() - reservation)
            return false;
        reservation += child;
        return true;
    }
}  // namespace Horo::Physics::Detail
