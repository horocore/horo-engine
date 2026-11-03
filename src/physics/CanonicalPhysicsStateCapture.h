#pragma once

#include "CanonicalPhysicsRuntime.h"
#include "CanonicalPhysicsStateBinding.h"
#include "CanonicalPhysicsStateBounds.h"
#include "PhysicsStateSolverStorage.h"

namespace Horo::Physics::Detail {
    /** @brief Optional owner observation of actual allocator routing; not
     * capture/publication authority. */
    struct CanonicalPhysicsStateCaptureMetrics final {
        std::size_t nativeTemporaryAllocations{}, nativeTemporaryBytes{}, nativeForeignReallocations{};
    };

    /** @brief Actual preallocated native temporaries are distinct from retained
     * public typed state. */
    struct CanonicalPhysicsStateCaptureScratch final {
        std::span<std::byte> temporary;
        std::span<std::byte> recorder;
        std::span<std::uint32_t> activeBodies;
        CanonicalPhysicsStateCaptureMetrics *metrics{};
    };

    /** @brief Checks exact native construction settings and every retained scratch
     * prefix before native allocation can run. */
    [[nodiscard]] Result<CanonicalPhysicsStateBounds> ValidateCanonicalPhysicsStateScratch(
        CanonicalWorldHandle world, const PhysicsWorldSettings &settings, const StateDetail::SolverStorage &staging,
        const CanonicalPhysicsStateCaptureScratch &scratch);

    /** @brief Captures the real pinned solver into private typed staging at a
     * joined owner boundary.
     * @details No native callbacks or public publication occur. All expected
     * insufficient-budget failures happen before calling native SaveState. Native
     * temporary pointer arrays use existing allocator hooks routed into the
     * cold-prepared scratch arena; all temporary containers die inside that scope.
     */
    [[nodiscard]] Result<PhysicsStateSolverView> CaptureCanonicalPhysicsState(CanonicalWorldHandle world,
                                                                              const PhysicsWorldSettings &settings,
                                                                              const CanonicalPhysicsStateBindings &bindings,
                                                                              StateDetail::SolverStorage &staging,
                                                                              CanonicalPhysicsStateCaptureScratch scratch);
}  // namespace Horo::Physics::Detail
