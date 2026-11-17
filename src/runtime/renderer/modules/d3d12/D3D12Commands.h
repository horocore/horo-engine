#pragma once
/** @file D3D12Commands.h
 * @brief Target-private bounded D3D12 graph submission and exact Horo timeline service.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <memory>

namespace Horo::Render {
    /** @brief Existing native queue classes; logical roles remain frontend-owned. */
    enum class D3D12QueueKind {
        Direct,
        Compute,
        Copy
    };
    /** @brief Fixed native translation mode; Enhanced requires actual feature/list support, never fallback. */
    enum class D3D12BarrierMode {
        Legacy,
        Enhanced
    };

    /** @brief Exact frontend identity associated with one already-created native queue class. */
    struct D3D12CommandQueueBinding final {
        RenderQueueId queue;
        D3D12QueueKind kind{D3D12QueueKind::Direct};
    };

    /** @brief Explicit startup bounds; native storage and portable history are allocated before admission opens. */
    struct D3D12CommandLimits final {
        std::uint32_t maximumPendingSubmissionsPerQueue{8};
        std::uint32_t maximumTransitions{4096};
        std::uint32_t maximumBufferCopies{1024};
        std::uint32_t maximumResourceReferences{4096};
        std::uint32_t maximumTimelineRanges{128};
        /** @brief Validates finite implementation ceilings. @return False for zero or excessive capacities. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Synchronous startup borrow; the service copies queue bindings and retains no configuration spans. */
    struct D3D12CommandConfiguration final {
        std::uint64_t deviceIncarnation{};
        std::span<const D3D12CommandQueueBinding> queues;
        D3D12BarrierMode barrierMode{D3D12BarrierMode::Legacy};
        D3D12CommandLimits limits;
    };

    /** @brief Owner-thread service borrowed from the admitted D3D12 device session.
     * No native handles, state inference, independent queue policy or presentation authority escapes.
     * Shutdown closes admission before polling retained native submissions; no ordinary call waits for GPU idle.
     */
    class ID3D12Commands {
    public:
        virtual ~ID3D12Commands() = default;
        /** @brief Validates and records one complete canonical graph batch before executing any native work.
         * @param request Existing frontend-owned graph, exact resource instances, lease and explicit submissions.
         * @param cancellation Checked before preparation and again before native submission; never rewinds submitted work.
         * @return Original typed validation/native error or accepted submission; no completion is fabricated.
         * @details Requires explicit submissions. On pre-submission failure, frontend lease ownership remains unchanged.
         * Once SubmitPrepared starts, native ownership survives every failure until proven retirement/device loss.
         */
        [[nodiscard]] virtual Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request,
                                                        const CancellationToken &cancellation = {}) = 0;
        /** @brief Observes one exact admitted point with the common finite CPU-wait policy.
         * @param wait Existing Horo purpose/timeout plus exact nonzero expectedDeviceIncarnation.
         * @return Completion, pending/timeout, or typed stale/unissued/native failure; no future value is admitted.
         */
        [[nodiscard]] virtual Result<bool> Observe(const RenderCpuWait &wait) = 0;
        /** @brief Nonblocking bounded retirement on the owner thread. @return Success or preserved native failure. */
        [[nodiscard]] virtual Result<void> Poll() = 0;
        /** @brief Permanently closes new work; submitted resource/allocator pins remain owned.
         * @return Success or wrong-thread failure; repeated owner-thread calls are harmless.
         */
        [[nodiscard]] virtual Result<void> BeginShutdown() = 0;
        /** @brief Polls native retirement without blocking or destroying pending parents.
         * @return Existing drain status or typed failure; the device session remains alive until drained.
         */
        [[nodiscard]] virtual Result<RenderBackendDrainProgress> PollDrain() = 0;
    };

    /** @brief Builds an actionable backend-scoped error retaining an optional native cause.
     * @param code Stable literal error identity. @param message Safe fixed remediation text.
     * @param nativeCause Optional native status, zero when no native operation ran. @return Owned typed error.
     */
    [[nodiscard]] Error D3D12CommandError(const char *code, const char *message, std::int64_t nativeCause = 0);
}  // namespace Horo::Render
