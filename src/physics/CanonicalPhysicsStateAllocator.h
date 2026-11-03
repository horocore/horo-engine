#pragma once

#include "CanonicalPhysicsStateArena.h"

namespace Horo::Physics::Detail {
    /** @brief Borrows only this thread's joined capture route; other
     * threads/allocations retain existing authority. */
    [[nodiscard]] CanonicalPhysicsStateArena *ActiveCanonicalPhysicsStateArena() noexcept;

    /** @brief Scoped nonreentrant route under existing process allocator hooks,
     * never a replacement global allocator owner. */
    class CanonicalPhysicsStateAllocationScope final {
    public:
        /** @brief Installs only a quiescent scratch owner on an otherwise unbound
         * capture lane. */
        explicit CanonicalPhysicsStateAllocationScope(CanonicalPhysicsStateArena &arena) noexcept;
        ~CanonicalPhysicsStateAllocationScope();
        CanonicalPhysicsStateAllocationScope(const CanonicalPhysicsStateAllocationScope &) = delete;
        CanonicalPhysicsStateAllocationScope &operator=(const CanonicalPhysicsStateAllocationScope &) = delete;

        /** @brief False is ordinary nested/busy admission and must fail before
         * entering Jolt. */
        [[nodiscard]] bool Admitted() const noexcept {
            return arena_ != nullptr;
        }

    private:
        CanonicalPhysicsStateArena *arena_{};
    };
}  // namespace Horo::Physics::Detail
