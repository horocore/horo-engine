#pragma once

/** @file D3D12NativeResources.h
 * @brief Target-private native construction seam; never installed or exported to consumers.
 */
#include "D3D12Resources.h"

#if defined(_WIN32) && defined(_M_X64)
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace Horo::Render {
    /** @brief Optional deterministic native failure seam used only by resource qualification tests.
     * Runs on the owner thread at explicit native boundaries; callbacks cannot reenter frontend/registry APIs.
     */
    struct D3D12ResourceTestHooks {
        void (*afterExecute)(ID3D12Device *) noexcept {nullptr};
        void (*beforeReadbackCompletion)(ID3D12Device *) noexcept {nullptr};
    };

    /** @brief Retains the exact initialized device/direct queue; creates bounded descriptor/fence storage.
     * @param device Initialized device. @param queue Its direct queue. @param limits Explicit host bounds.
     * @param testHooks Empty for production; bounded owner-thread failure injection for native tests.
     * @return Owned resource service or typed native failure; no adapter selection or fallback.
     */
    [[nodiscard]] Result<std::unique_ptr<ID3D12Resources>> CreateD3D12Resources(ID3D12Device *device, ID3D12CommandQueue *queue,
                                                                                const D3D12ResourceLimits &limits,
                                                                                D3D12ResourceTestHooks testHooks = {});
}  // namespace Horo::Render
#endif
