#include "OpenGLExecutionAdapter.h"

#include "OpenGLExecutionState.h"
#include "OpenGLRenderBackendErrors.h"

#include <glad/gl.h>
#include <optional>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Builds the typed terminal frame-retirement failure at the private dispatch boundary. */
        template <typename T> Result<T> SynchronizationFailure() {
            return Result<T>::Failure(MakeError(OpenGLBackendErrors::SynchronizationFailed,
                                                "OpenGL could not establish or poll a frame completion fence; shut down the backend."));
        }

        /** @brief Preserves native command failure context in the OpenGL error domain. */
        Result<void> CommandFailure(std::string message) {
            return Result<void>::Failure(MakeError(OpenGLBackendErrors::CommandFailed, std::move(message)));
        }
    }  // namespace

    /** @copydoc ValidateOpenGLExecutionPlan */
    Result<void> ValidateOpenGLExecutionPlan(const RenderExecutionPlan &plan) {
        if (constexpr std::size_t maxPasses = 1024; plan.orderedPasses.size() > maxPasses)
            return Result<void>::Failure(
                MakeError(OpenGLBackendErrors::WorkLimit, "OpenGL executes at most 1024 ordered passes per plan."));
        for (std::size_t index = 0; index < plan.orderedPasses.size(); ++index) {
            const RenderPassDescriptor &pass = plan.orderedPasses[index];
            if (!pass.id.IsValid()) {
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::InvalidExecutionPlan, "Execution plan contains an invalid render pass ID."));
            }
            if (pass.kind != RenderPassKind::Graphics) {
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::UnsupportedPassKind, "Initial OpenGL backend supports graphics passes only."));
            }
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (pass.id == plan.orderedPasses[previous].id) {
                    return Result<void>::Failure(
                        MakeError(OpenGLBackendErrors::InvalidExecutionPlan, "Execution plan contains duplicate render pass IDs."));
                }
            }
            if (!pass.primaryOutput.has_value()) {
                continue;
            }

            if (!pass.primaryOutput->IsValid()) {
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::InvalidExecutionPlan, "Primary output attachment operations are invalid."));
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenGLExecutionAdapter::Execute */
    Result<void> OpenGLExecutionAdapter::Execute(const RenderExecutionPlan &plan, const FramebufferExtent extent) const {
        if (functions_.state.error() != GL_NO_ERROR)
            return CommandFailure("OpenGL reported an error before execution.");
        {
            Detail::OpenGLExecutionState state{functions_};
            if (functions_.state.error() != GL_NO_ERROR)
                return CommandFailure("OpenGL could not capture the caller's command state.");
            if (!state.Apply(extent))
                return CommandFailure("OpenGL draw-buffer count exceeds the fixed 64-slot isolation budget.");
            if (functions_.state.error() != GL_NO_ERROR)
                return CommandFailure("OpenGL could not prepare primary-output command state.");
            for (const RenderPassDescriptor &pass : plan.orderedPasses) {
                if (pass.primaryOutput.has_value() && pass.primaryOutput->loadOperation == AttachmentLoadOperation::Clear) {
                    const ClearColor &color = pass.primaryOutput->clearColor;
                    functions_.clearColor(color.red, color.green, color.blue, color.alpha);
                    functions_.clear(GL_COLOR_BUFFER_BIT);
                }
            }
        }
        if (functions_.state.error() != GL_NO_ERROR)
            return CommandFailure("OpenGL reported an error during execution or state restoration.");
        return Result<void>::Success();
    }

    /** @copydoc OpenGLExecutionAdapter::PollFrameSlot */
    bool OpenGLExecutionAdapter::PollFrameSlot(const std::size_t slot) noexcept {
        std::uintptr_t &fence = frameFences_[slot];
        if (fence == 0)
            return true;
        const std::uint32_t status = functions_.sync.poll(fence);
        if (status == GL_TIMEOUT_EXPIRED)
            return true;
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
            synchronizationFailed_ = true;
            return false;
        }
        functions_.sync.destroy(std::exchange(fence, 0));
        if (frameLeases_[slot] != nullptr)
            std::exchange(frameLeases_[slot], nullptr)->Release();
        return true;
    }

    /** @copydoc OpenGLExecutionAdapter::AdmitFrameSlot */
    Result<std::size_t> OpenGLExecutionAdapter::AdmitFrameSlot(const std::size_t slotCount) {
        if (synchronizationFailed_)
            return SynchronizationFailure<std::size_t>();
        std::optional<std::size_t> available;
        for (std::size_t index = 0; index < slotCount; ++index) {
            if (!PollFrameSlot(index))
                return SynchronizationFailure<std::size_t>();
            if (frameFences_[index] == 0 && frameLeases_[index] == nullptr && !available.has_value())
                available = index;
        }
        if (!available.has_value())
            return Result<std::size_t>::Failure(
                MakeError(OpenGLBackendErrors::FrameBackpressure,
                          "GPU frame streams occupy the configured frames-in-flight budget; retry without waiting."));
        return Result<std::size_t>::Success(*available);
    }

    /** @copydoc OpenGLExecutionAdapter::FenceFrame */
    bool OpenGLExecutionAdapter::FenceFrame(const std::size_t slot) noexcept {
        const std::uintptr_t replacement = functions_.sync.fence();
        if (replacement == 0) {
            synchronizationFailed_ = true;
            return false;
        }
        std::uintptr_t &fence = frameFences_[slot];
        if (fence != 0)
            functions_.sync.destroy(fence);
        fence = replacement;
        functions_.sync.flush();
        return true;
    }

    /** @copydoc OpenGLExecutionAdapter::RetainFrameLease */
    bool OpenGLExecutionAdapter::RetainFrameLease(const std::size_t slot, IRenderGraphResourceLease &lease) noexcept {
        if (slot >= frameLeases_.size() || frameLeases_[slot] != nullptr || frameFences_[slot] != 0 || synchronizationFailed_)
            return false;
        frameLeases_[slot] = &lease;
        return true;
    }

    /** @copydoc OpenGLExecutionAdapter::Reset */
    void OpenGLExecutionAdapter::Reset() noexcept {
        if (functions_.sync.IsValid()) {
            for (std::uintptr_t &fence : frameFences_) {
                if (fence != 0)
                    functions_.sync.destroy(std::exchange(fence, 0));
            }
        }
        for (auto &lease : frameLeases_) {
            if (lease != nullptr)
                std::exchange(lease, nullptr)->Release();
        }
        synchronizationFailed_ = false;
    }
}  // namespace Horo::Render::Detail
