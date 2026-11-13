#pragma once

/** @file D3D12NativeCommands.h @brief Target-private construction over the exact admitted device, queues and resource authority. */
#include "D3D12CommandRuntime.h"
#include "D3D12Resources.h"

#if defined(_WIN32) && defined(_M_X64)
#include <array>
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace Horo::Render {
    /** @brief Optional deterministic failure injection at the irreversible native submission boundary. */
    struct D3D12CommandTestHooks final {
        void (*afterExecute)(ID3D12Device *) noexcept {nullptr};
    };

    /** @brief Creates the portable owner around real bounded native command storage.
     * @param device Exact initialized native device. @param queues Native Direct/Compute/Copy order; uncreated queues are null.
     * @param resources SAME native resource service for this device; it must outlive the returned service and its drain.
     * @param configuration Explicit logical queue mapping, incarnation, translation mode and bounds.
     * @param hooks Empty for production; callbacks cannot reenter service methods.
     * @details Exactly one command service may borrow a resource incarnation. Failed initialization releases its claim;
     * after any graph execution, replacement requires a fresh canonical resource incarnation so timelines cannot restart.
     * Canonical device-local resources enter/leave each native submission in COMMON. The adapter explicitly
     * realizes declared imported logical states without discarding contents, then flushes back to COMMON before its signal.
     * Explicit non-COMMON external releases and overlapping native graph allocations are rejected before execution.
     * Upload heaps remain GENERIC_READ with read-compatible access only. First placed-resource activation may use the
     * legacy aliasing API at the proven boundary even in Enhanced mode; enhanced access begins only afterward.
     * Native copies and prior graph references are ordered by exact GPU fence waits; no CPU idle wait is inserted.
     * All finite command slots, scratch vectors and graph-retention groups are allocated at startup.
     * @return Owned initialized service or typed failure; no native/software discovery or mode fallback.
     */
    [[nodiscard]] Result<std::unique_ptr<ID3D12Commands>> CreateD3D12Commands(ID3D12Device *device,
                                                                              const std::array<ID3D12CommandQueue *, 3> &queues,
                                                                              ID3D12Resources &resources,
                                                                              const D3D12CommandConfiguration &configuration,
                                                                              D3D12CommandTestHooks hooks = {});
}  // namespace Horo::Render
#endif
