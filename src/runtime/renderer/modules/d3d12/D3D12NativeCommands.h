#pragma once

/** @file D3D12NativeCommands.h
 * @brief Non-installed native recording bridge for already admitted command tickets.
 */
#include "D3D12Resources.h"

#if defined(_WIN32) && defined(_M_X64)
struct ID3D12GraphicsCommandList;

namespace Horo::Render {
    /** @brief Borrows a list only while its exact ticket is Recording on the context owner thread.
     * @param resources Existing initialized context; no device or identity is created.
     * @param id Exact active recording. @return Borrowed native list or typed stale/state/thread failure.
     * @details Native callers validate all commands and retain referenced backing through submission completion.
     * The borrow ends at Close/Cancel/shutdown and cannot be retained across a service operation.
     */
    [[nodiscard]] Result<ID3D12GraphicsCommandList *> BorrowD3D12CommandList(ID3D12Resources &resources, D3D12CommandId id);
}  // namespace Horo::Render
#endif
