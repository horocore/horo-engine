#pragma once

/** @file D3D12Commands.h
 * @brief Target-private command recording and completion over the initialized resource context.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <cstdint>

namespace Horo::Render {
    /** @brief Non-reused recording identity bound to the resource context's existing device incarnation. */
    struct D3D12CommandId {
        std::uint64_t deviceIncarnation{0};
        std::uint64_t value{0};
        [[nodiscard]] constexpr bool operator==(const D3D12CommandId &) const noexcept = default;
    };

    /**
     * @brief Owner-thread command service borrowed from ID3D12Resources, never an independent device owner.
     *
     * Configure binds the frontend's one effective direct-queue identity before recording.
     * Native recording is available only through the target-private bridge. The caller validates
     * finite graph/workload/resource states before encoding and retains all referenced native backing
     * through the canonical completion lease. This service does not claim graph lowering,
     * enhanced barriers, interqueue waits, presentation or an interactive renderer.
     * Recording/closed tickets are abandoned with Cancel. Submitted tickets remain retained
     * until the shared context fence proves completion; Poll does not consume their identity.
     * Cancel after submission abandons observation, not GPU execution. Closing resource admission
     * cancels unsent tickets and drains submitted tickets through the same PollDrain authority.
     */
    class ID3D12Commands {
    public:
        virtual ~ID3D12Commands() = default;
        /** @brief Binds exactly one frontend-issued direct queue. @param queue Effective queue identity.
         * @return Success, or typed wrong-thread/invalid/reconfiguration failure. */
        [[nodiscard]] virtual Result<void> Configure(RenderQueueId queue) = 0;
        /** @brief Creates one bounded allocator/list pair. @return Exact recording identity or typed admission/native failure. */
        [[nodiscard]] virtual Result<D3D12CommandId> Begin() = 0;
        /** @brief Closes one native recording. @param id Current recording. @return Typed state/native result. */
        [[nodiscard]] virtual Result<void> Close(D3D12CommandId id) = 0;
        /** @brief Submits one closed recording without waiting, using the frontend's order/signal.
         * @param id Current closed ticket. @param submission Exact one-queue submission; nonempty waits are not yet supported.
         * @param lease Stable optional canonical resource lease; required by the caller for resident references.
         * One outstanding lease cannot be reused across tickets; aggregate multi-list graph retention is not implemented.
         * @param transfer Synchronous receipt, updated before irreversible Execute; never retained.
         * @param cancellation Checked before native submission. @return Typed result; failure after transfer retains completion ownership.
         */
        [[nodiscard]] virtual Result<void> Submit(D3D12CommandId id, const RenderQueueSubmission &submission,
                                                  IRenderGraphResourceLease *lease, RenderGraphLeaseTransfer &transfer,
                                                  const CancellationToken &cancellation = {}) = 0;
        /** @brief Polls one exact ticket without waiting. @param id Current submitted ticket.
         * @return True after actual fence completion, false while pending, or typed stale/native failure. */
        [[nodiscard]] virtual Result<bool> Poll(D3D12CommandId id) = 0;
        /** @brief Abandons observation and unsent work, retaining accepted work through terminal completion.
         * @param id Current ticket. @return Typed identity/thread result. */
        [[nodiscard]] virtual Result<void> Cancel(D3D12CommandId id) = 0;
    };
}  // namespace Horo::Render
