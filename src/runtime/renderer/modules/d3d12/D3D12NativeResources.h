#pragma once

/** @file D3D12NativeResources.h
 * @brief Target-private native construction seam; never installed or exported to consumers.
 */
#include "D3D12Resources.h"

#if defined(_WIN32) && defined(_M_X64)
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace Horo::Render {
    /** @brief Retains the exact initialized device/direct queue; creates bounded descriptor/fence storage.
     * @param device Initialized device. @param queue Its direct queue. @param limits Explicit host bounds.
     * @return Owned resource service or typed native failure; no adapter selection or fallback.
     */
    [[nodiscard]] Result<std::unique_ptr<ID3D12Resources>> CreateD3D12Resources(ID3D12Device *device, ID3D12CommandQueue *queue,
                                                                                const D3D12ResourceLimits &limits);
}  // namespace Horo::Render
#endif
