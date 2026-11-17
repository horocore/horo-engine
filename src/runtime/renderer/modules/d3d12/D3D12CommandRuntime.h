#pragma once
/** @file D3D12CommandRuntime.h @brief Native-free transactional recording seam and portable service construction. */
#include "D3D12Commands.h"

namespace Horo::Render {
    /** @brief Native adapter owns actual allocators/lists/fences and SAME resource service authority.
     * All calls are externally serialized on the service owner thread. This seam exists for deterministic
     * admission/failure tests; a fake adapter is never evidence of native execution.
     */
    class ID3D12CommandRuntime {
    public:
        virtual ~ID3D12CommandRuntime() = default;
        /** @brief Transactionally prepares bounded native storage and required feature support.
         * @param configuration Exact copied startup policy. @return Success or typed failure with native rollback.
         */
        [[nodiscard]] virtual Result<void> Initialize(const D3D12CommandConfiguration &configuration) = 0;
        /** @brief Validates all native objects/translations and records the entire UNSENT batch.
         * @param request Canonical validated batch; every resource and slot must be pinned before Execute is possible.
         * @param cancellation Cooperative pre-execution admission. @return Prepared batch or failure; no queue Execute has run.
         * @details Does not take frontend lease ownership or retain request spans/transfer pointers beyond this call.
         * Partial preparation remains AbortPrepared-safe, including when this method returns failure.
         */
        [[nodiscard]] virtual Result<void> PrepareGraph(const RenderGraphExecutionRequest &request,
                                                        const CancellationToken &cancellation) = 0;
        /** @brief Consumes the prepared batch and starts queue execution in canonical order.
         * @param lease Stable frontend-owned lease; retained only when native completion takes ownership.
         * @param transfer Synchronous authority receipt; never retained beyond this call.
         * @return Success or typed native failure. ANY return consumes the prepared state.
         * @details Takes frontend lease authority before the first Execute. Submitted pins survive signal failure;
         * never release them or report completion based only on Execute/Close/CPU return. Unsent remainder is safely abandoned.
         */
        [[nodiscard]] virtual Result<void> SubmitPrepared(IRenderGraphResourceLease *lease, RenderGraphLeaseTransfer *transfer) = 0;
        /** @brief Releases UNSENT preparation only; no effect on already submitted pins. */
        virtual void AbortPrepared() noexcept = 0;
        /** @brief Observes a portable-validated issued point. @param wait Exact purpose/incarnation/value.
         * @return Native completion or typed failure; blocking waits obey the supplied finite timeout.
         */
        [[nodiscard]] virtual Result<bool> Observe(const RenderCpuWait &wait) = 0;
        /** @brief Polls a bounded set of real native completions. @return Success or typed native failure. */
        [[nodiscard]] virtual Result<void> Poll() = 0;
        /** @brief Closes native admission without dropping any submitted resource/allocator ownership. */
        virtual void StopAdmission() noexcept = 0;
        /** @brief Polls retirement without waiting. @return Drained only when all submitted work has real terminal evidence. */
        [[nodiscard]] virtual Result<RenderBackendDrainProgress> PollDrain() = 0;
    };

    /** @brief Creates the portable owner around an actual native adapter, or a deliberate fake in focused tests.
     * @param runtime Exclusive inert adapter ownership; no native handle is exposed by this factory.
     * @param configuration Finite queue/mode/device policy copied during construction.
     * @return Initialized command service or original failure; unsupported platforms never manufacture a native adapter.
     */
    [[nodiscard]] Result<std::unique_ptr<ID3D12Commands>> CreateD3D12CommandService(std::unique_ptr<ID3D12CommandRuntime> runtime,
                                                                                    const D3D12CommandConfiguration &configuration);
}  // namespace Horo::Render
