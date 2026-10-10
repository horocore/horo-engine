#pragma once

/** @file OpenGLExecutionAdapter.h
 * @brief Private ownership of bounded OpenGL command translation and frame retirement.
 */

#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "OpenGLBackendInternal.h"

namespace Horo::Render::Detail {
    /** @brief Validates backend-owned plan metadata before any native encoding.
     * @param plan Borrowed ordered plan; its active token is validated by the backend lifecycle owner.
     * @return Typed malformed, unsupported or bounded-work failure, or success.
     */
    [[nodiscard]] Result<void> ValidateOpenGLExecutionPlan(const RenderExecutionPlan &plan);

    /** @brief Context-owner-only command adaptation with fixed storage for eight outstanding frame streams. */
    class OpenGLExecutionAdapter final {
    public:
        /** @brief Borrows non-throwing state/sync dispatch from the non-moving backend owner. */
        explicit OpenGLExecutionAdapter(const OpenGLCommandFunctions &functions) noexcept : functions_(functions) {}

        OpenGLExecutionAdapter(const OpenGLExecutionAdapter &) = delete;
        OpenGLExecutionAdapter &operator=(const OpenGLExecutionAdapter &) = delete;

        /** @brief Encodes a validated plan with scoped primary-output state isolation.
         * @param plan Borrowed ordered pass view, previously validated by the lifecycle owner.
         * @param extent Signed-native-compatible active framebuffer extent.
         * @return Typed native command/state failure or success.
         * @pre Owning backend context is current on its owner thread.
         */
        [[nodiscard]] Result<void> Execute(const RenderExecutionPlan &plan, FramebufferExtent extent) const;
        /** @brief Polls pending streams without waiting and returns a free slot.
         * @param slotCount Validated frames-in-flight budget, from one to eight.
         * @return Free slot or typed backpressure/synchronization failure.
         */
        [[nodiscard]] Result<std::size_t> AdmitFrameSlot(std::size_t slotCount);
        /** @brief Fences all preceding context work and nonblockingly flushes it.
         * @param slot Slot reserved by AdmitFrameSlot for the active frame.
         * @return False if fence creation fails; failure remains sticky until Reset.
         */
        [[nodiscard]] bool FenceFrame(std::size_t slot) noexcept;
        /**
         * @brief Transfers one stable registry lease into the admitted frame's completion slot.
         * @param slot Exact reserved frame slot, not already retaining a graph stream.
         * @param lease Frontend-owned lease that outlives native completion or context shutdown.
         * @return False for an occupied, invalid, or failed completion slot, before encoding.
         */
        [[nodiscard]] bool RetainFrameLease(std::size_t slot, IRenderGraphResourceLease &lease) noexcept;
        /** @brief Deletes owned fences without waiting; called before native context destruction on its owner thread. */
        void Reset() noexcept;

    private:
        /** @brief Polls one owned fence once, releasing its lease only after native completion. */
        [[nodiscard]] bool PollFrameSlot(std::size_t slot) noexcept;
        const OpenGLCommandFunctions &functions_;
        std::array<std::uintptr_t, 8> frameFences_{};
        std::array<IRenderGraphResourceLease *, 8> frameLeases_{};
        bool synchronizationFailed_{false};
    };
}  // namespace Horo::Render::Detail
