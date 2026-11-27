#pragma once

#include "CanonicalPhysicsStateRestore.h"

namespace Horo::Physics::Detail {
    /** @brief Byte range overlap uses subtraction rather than overflowing
     * pointer-end arithmetic. */
    inline bool CanonicalStateRangesOverlap(std::span<const std::byte> a, std::span<const std::byte> b) noexcept {
        if (a.empty() || b.empty())
            return false;
        const auto first = reinterpret_cast<std::uintptr_t>(a.data());
        const auto second = reinterpret_cast<std::uintptr_t>(b.data());
        return first <= second ? second - first < a.size() : first - second < b.size();
    }

    /** @brief All scratch and staging buffers are independent; expected/actual
     * recapture buffers cannot alias and fake equality. */
    inline bool CanonicalStateRestoreScratchDisjoint(const PhysicsStateSolverView &state,
                                                     const CanonicalPhysicsStateRestoreScratch &scratch) noexcept {
        const auto &storage = scratch.recapture;
        const std::array writable{std::span<const std::byte>{scratch.expectedWire},
                                  std::span<const std::byte>{scratch.actualWire},
                                  std::span<const std::byte>{scratch.native.temporary},
                                  std::span<const std::byte>{scratch.native.recorder},
                                  std::as_bytes(scratch.native.activeBodies),
                                  std::as_bytes(std::span{storage.bodies}),
                                  std::as_bytes(std::span{storage.activeBodyOrder}),
                                  std::as_bytes(std::span{storage.constraints}),
                                  std::as_bytes(std::span{storage.pairs}),
                                  std::as_bytes(std::span{storage.manifolds}),
                                  std::as_bytes(std::span{storage.ccdManifolds}),
                                  std::as_bytes(std::span{storage.activeBodyScratch})};
        const std::array inputs{std::as_bytes(state.bodies), std::as_bytes(state.activeBodyOrder), std::as_bytes(state.constraints),
                                std::as_bytes(state.pairs),  std::as_bytes(state.manifolds),       std::as_bytes(state.ccdManifolds)};
        for (std::size_t a = 0; a < writable.size(); ++a) {
            for (std::size_t b = a + 1; b < writable.size(); ++b)
                if (CanonicalStateRangesOverlap(writable[a], writable[b]))
                    return false;
            for (auto input : inputs)
                if (CanonicalStateRangesOverlap(writable[a], input))
                    return false;
        }
        return true;
    }
}  // namespace Horo::Physics::Detail
