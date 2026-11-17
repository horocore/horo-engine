#include "CanonicalPhysicsStateColdCost.h"

#include "CanonicalPhysicsStateAllocator.h"

#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        thread_local CanonicalPhysicsStateColdCost *activeCost{};
    }

    /** @copydoc
     * CanonicalPhysicsStateColdCostScope::CanonicalPhysicsStateColdCostScope */
    CanonicalPhysicsStateColdCostScope::CanonicalPhysicsStateColdCostScope(CanonicalPhysicsStateColdCost &cost) noexcept {
        if (activeCost || ActiveCanonicalPhysicsStateArena())
            return;
        cost = {};
        activeCost = &cost;
        admitted_ = true;
    }

    /** @copydoc
     * CanonicalPhysicsStateColdCostScope::~CanonicalPhysicsStateColdCostScope */
    CanonicalPhysicsStateColdCostScope::~CanonicalPhysicsStateColdCostScope() {
        if (admitted_)
            activeCost = nullptr;
    }

    /** @copydoc RecordCanonicalPhysicsStateColdAllocation */
    void RecordCanonicalPhysicsStateColdAllocation(std::size_t bytes) noexcept {
        if (!activeCost)
            return;
        if (const auto maximum = std::numeric_limits<std::size_t>::max();
            bytes > maximum - activeCost->requestedBytes || activeCost->requests == maximum) {
            activeCost->overflow = true;
            return;
        }
        activeCost->requestedBytes += bytes;
        ++activeCost->requests;
    }

    /** @copydoc CanonicalPhysicsStateColdCostActive */
    bool CanonicalPhysicsStateColdCostActive() noexcept {
        return activeCost != nullptr;
    }
}  // namespace Horo::Physics::Detail
