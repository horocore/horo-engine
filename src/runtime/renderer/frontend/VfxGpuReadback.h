#pragma once
/** @file VfxGpuReadback.h @brief Existing-queue integration facts; no new request identity, staging or result authority. */
#include "VfxGpuStorage.h"

namespace Horo::Render::Detail {
    /** @brief Reserves bounded fixed-slot logical capacity without charging another physical allocation. */
    [[nodiscard]] Result<void> ReserveVfxGpuReadback(VfxGpuStorage &storage, VfxGpuRecord &record, bool nativeReadbackAvailable);
    /** @brief Restores a never-dispatched cold reservation; no submitted request may be forgotten. */
    void ReleaseVfxGpuReadbackReservation(VfxGpuStorage &storage, VfxGpuRecord &record) noexcept;
    /** @brief Selects/creates a pending request in the single existing queue, without allocation. */
    [[nodiscard]] RenderReadbackPreparedStatus RequestVfxGpuObservation(VfxGpuStorage &storage, VfxGpuRecord &record,
                                                                        VfxGpuStepIdentity step, std::size_t observation,
                                                                        std::size_t &request) noexcept;
    /** @brief Cancels and discards a request only at a proven never-dispatched/unsubmitted boundary. */
    void AbandonVfxGpuObservation(VfxGpuStorage &storage, VfxGpuRecord &record, VfxGpuStepIdentity step) noexcept;
}  // namespace Horo::Render::Detail
