#include "CanonicalPhysicsStateAllocator.h"

#include "CanonicalPhysicsStateColdCost.h"

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Null unless this same thread is inside the bounded, synchronous
         * capture operation. */
        CanonicalPhysicsStateArena *&Route() noexcept {
            static thread_local CanonicalPhysicsStateArena *arena{};
            return arena;
        }
    }  // namespace

    /** @copydoc ActiveCanonicalPhysicsStateArena */
    CanonicalPhysicsStateArena *ActiveCanonicalPhysicsStateArena() noexcept {
        return Route();
    }

    /** @copydoc
     * CanonicalPhysicsStateAllocationScope::CanonicalPhysicsStateAllocationScope */
    CanonicalPhysicsStateAllocationScope::CanonicalPhysicsStateAllocationScope(CanonicalPhysicsStateArena &arena) noexcept {
        if (Route() || CanonicalPhysicsStateColdCostActive() || !arena.Quiescent())
            return;
        arena.Reset();
        Route() = &arena;
        arena_ = &arena;
    }

    /** @brief Every native local container must release its actual arena allocation
     * before route removal. */
    CanonicalPhysicsStateAllocationScope::~CanonicalPhysicsStateAllocationScope() {
        if (!arena_)
            return;
        if (Route() != arena_ || !arena_->Quiescent())
            std::abort();
        Route() = nullptr;
    }
}  // namespace Horo::Physics::Detail
