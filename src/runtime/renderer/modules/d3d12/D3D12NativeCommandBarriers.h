#pragma once
#include "D3D12ResourceInternals.h"
#include "Horo/Runtime/Render/RenderGraphSynchronization.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12Detail {
    /** @brief Validates logical operation and native queue compatibility without recording. */
    [[nodiscard]] Result<void> ValidateCommandState(const RenderGraphLogicalState &state, D3D12QueueKind queue);
    /** @brief Records one whole-resource barrier in the explicitly selected translation mode. */
    void RecordCommandBarrier(ID3D12GraphicsCommandList &commands, ID3D12GraphicsCommandList7 *enhanced, Resource &resource,
                              const RenderGraphLogicalState &before, const RenderGraphLogicalState &after, D3D12QueueKind queue);
    /** @brief Activates a placed resource only at its COMMON boundary before first native access. */
    void RecordCommandActivation(ID3D12GraphicsCommandList &commands, Resource &resource);
}  // namespace Horo::Render::D3D12Detail
#endif
