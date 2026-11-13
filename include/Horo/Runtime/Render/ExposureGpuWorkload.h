#pragma once
/** @file ExposureGpuWorkload.h
 * @brief Fixed exposure GPU state/dispatch ABI and one closed graph operation.
 */
#include "Horo/Runtime/Render/Exposure.h"
#include "Horo/Runtime/Render/ExposureGpuKernel.h"
#include "Horo/Runtime/Render/RenderGraphExecution.h"

#include <cstddef>

namespace Horo::Render {
    /** @brief Actual GPU-written state; float32 values plus explicit split64 metadata, no CPU metering. */
    struct alignas(16) ExposureGpuState final {
        float exposureEv{}, exposureScale{};
        std::uint32_t validSamples{}, reason{};
        std::uint32_t generationLo{}, generationHi{}, sourceSubmissionLo{}, sourceSubmissionHi{};
    };

    static_assert(sizeof(ExposureGpuState) == 32);

    /** @brief Six 16-byte constant lanes: existing Exposure80 followed by generationLo/Hi, sourceSubmissionLo/Hi.
     * Native packing sets Controls.z bit31 only for explicit resetPrevious; bit0 is the clock. Portable input remains0/1. */
    struct alignas(16) ExposureGpuDispatchParameters final {
        ExposureDispatchParameters exposure;
        std::array<std::uint32_t, 4> identity{};
    };

    static_assert(sizeof(ExposureGpuDispatchParameters) == 96);
    static_assert(offsetof(ExposureGpuDispatchParameters, identity) == 80);

    /** @brief One fixed clear/build/adapt operation. Graph declares sampled scene read, histogram storage read/write,
     * previous storage read (read/write when reset), output storage write. All four resources are distinct.
     * resetPrevious performs actual GPU zero-fill before compute; previous must additionally declare CopyDestination.
     * Otherwise native journal admission requires the exact previous buffer/view/generation from a genuinely committed
     * same-queue native command at sourceSubmission N-1. No status wait or same-frame CPU readback is permitted.
     * produced is metadata for the real device output, consumed by the next submitted view; it does not prove completion.
     */
    struct RenderGraphExposureGpu final {
        RenderGraphResourceId sceneColor, histogram, previousExposure, outputExposure;
        FramebufferExtent sourceExtent;
        ExposureGpuDispatchParameters dispatch;
        ExposureState previous, produced;
        bool resetPrevious{};
        std::shared_ptr<IResidentExposureGpuKernel> kernel;
    };
    struct RenderGraphExecutionRequest;
    /** @brief Validates exact graph usages and imported exposure buffer associations.
     * @param request Actual graph request. @param pass Actual compiled pass. @param workload Closed exposure operation.
     * @return Success or typed invalid-input failure; no native execution is performed.
     */
    [[nodiscard]] Result<void> ValidateExposureGpuGraphWorkload(const RenderGraphExecutionRequest &request,
                                                                const RenderGraphExecutionPass &pass,
                                                                const RenderGraphExposureGpu &workload);
    /** @brief Validates portable closed metadata; native provenance/residency is checked separately.
     * @param workload Exact graph operation. @return Success or typed invalid/bounds failure.
     */
    [[nodiscard]] Result<void> ValidateExposureGpuWorkload(const RenderGraphExposureGpu &workload);
}  // namespace Horo::Render
