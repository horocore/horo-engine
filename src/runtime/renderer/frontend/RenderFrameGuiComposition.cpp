#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphResourceLeasePool.h"
#include "RenderSubmissionAllocator.h"

#include <array>

namespace Horo::Render {
    /** @brief Stack-bounded GUI admission and rollback state; an existing lease retains its native owner. */
    struct RenderFrameScope::GuiCompositionState {
        GuiCompositionPreparation prepared;
        std::array<GuiSampledResourceInstance, GuiCompositionPreparation::MaximumResources> resources{};
        std::array<std::uint64_t, GuiCompositionPreparation::MaximumResources> buffers{};
        std::array<Detail::RenderGraphResidentIdentity, GuiCompositionPreparation::MaximumResources * 4> pins{};
        std::optional<RenderQueueSubmission> submission;
        Detail::RenderSubmissionAllocator history;
        IRenderGraphResourceLease *lease{};
        RenderGraphLeaseTransfer transfer;
        bool extendsExisting{};
        std::size_t pinCount{};

        /** @brief Resolves/pins an exact ready staging or encoder-buffer generation before backend admission. */
        Result<void> PinBuffer(RenderFrontend &frontend, RenderBufferHandle handle, std::uint64_t &instance) {
            const auto resolved = frontend.BackendInstance(handle);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            instance = resolved.Value();
            pins[pinCount++] = {Detail::RenderResourceClass::Buffer, {handle.owner, handle.slot, handle.generation}};
            return Result<void>::Success();
        }

        /** @brief Resolves only the prepared bounded generations; backend admission still proves image contents/use ordering. */
        Result<void> Resolve(RenderFrontend &frontend) {
            for (std::size_t index = 0; index < prepared.buffers.size(); ++index)
                if (const auto resolved = PinBuffer(frontend, prepared.buffers[index], buffers[index]); resolved.HasError())
                    return resolved;
            for (std::size_t index = 0; index < prepared.resources.size(); ++index) {
                const auto &sample = prepared.resources[index];
                if (sample.uploadSource.IsValid()) {
                    if (prepared.mode != GuiCompositionMode::ExplicitGraphicsSubmission)
                        return Result<void>::Failure(MakeError(FrontendErrors::ResourceNotReady));
                    if (const auto resolved = PinBuffer(frontend, sample.uploadSource, resources[index].uploadSource); resolved.HasError())
                        return resolved;
                }
                const auto view = frontend.BackendInstance(sample.view);
                if (view.HasError())
                    return Result<void>::Failure(view.ErrorValue());
                resources[index].view = view.Value();
                pins[pinCount++] = {Detail::RenderResourceClass::TextureView,
                                    {sample.view.owner, sample.view.slot, sample.view.generation}};
                if (sample.sampler.IsValid()) {
                    const auto sampler = frontend.BackendInstance(sample.sampler);
                    if (sampler.HasError())
                        return Result<void>::Failure(sampler.ErrorValue());
                    resources[index].sampler = sampler.Value();
                    pins[pinCount++] = {Detail::RenderResourceClass::Sampler,
                                        {sample.sampler.owner, sample.sampler.slot, sample.sampler.generation}};
                }
            }
            return Result<void>::Success();
        }

        ~GuiCompositionState() {
            if (lease != nullptr && !extendsExisting && transfer.authority == RenderGraphLeaseAuthority::Frontend)
                lease->Release();
        }
    };

    /** @copydoc RenderFrameScope::PrepareGuiComposition */
    Result<void> RenderFrameScope::PrepareGuiComposition(IGuiPassExecutor &executor, GuiCompositionState &state) {
        const auto prepared = executor.PrepareComposition(frame_);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        state.prepared = prepared.Value();
        if (state.prepared.frame != frame_ || state.prepared.resources.size() > GuiCompositionPreparation::MaximumResources ||
            state.prepared.buffers.size() > GuiCompositionPreparation::MaximumResources ||
            (state.prepared.mode != GuiCompositionMode::BorrowedActiveRecording &&
             state.prepared.mode != GuiCompositionMode::ExplicitGraphicsSubmission))
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceNotReady));
        if (const auto resolved = state.Resolve(*owner_); resolved.HasError())
            return resolved;
        if (state.pinCount != 0 || state.prepared.mode == GuiCompositionMode::ExplicitGraphicsSubmission) {
            const auto lease = owner_->graphResourceLeases_->AcquireGui(std::span{state.pins}.first(state.pinCount), completionLease_);
            if (lease.HasError())
                return Result<void>::Failure(lease.ErrorValue());
            state.lease = lease.Value();
            state.extendsExisting = completionLease_ != nullptr;
            if (state.extendsExisting)
                state.transfer.authority = RenderGraphLeaseAuthority::BackendCompletion;
        }
        if (state.prepared.mode == GuiCompositionMode::ExplicitGraphicsSubmission) {
            std::array<RenderTimelinePoint, Detail::RenderSubmissionAllocator::MaximumQueues> waits{};
            const auto reserved = ReserveSubmission(RenderQueueRole::Graphics, owner_->submissionAllocator_->AdmittedPoints(waits));
            if (reserved.HasError())
                return Result<void>::Failure(reserved.ErrorValue());
            state.submission = reserved.Value();
        }
        state.history = *owner_->submissionAllocator_;
        return state.submission ? state.history.Admit(std::span{&*state.submission, 1}) : Result<void>::Success();
    }

    /** @copydoc RenderFrameScope::ComposeGui */
    Result<void> RenderFrameScope::ComposeGui(IGuiPassExecutor &executor, const CancellationToken &cancellation) {
        if (backend_ == nullptr)
            return Result<void>::Failure(MakeError(FrontendErrors::FrameNotActive));
        if (const auto owner = owner_->RequireOpenOwner(); owner.HasError())
            return owner;
        if (!executed_)
            return Result<void>::Failure(MakeError(FrontendErrors::FrameNotExecuted));
        if (guiComposed_)
            return Result<void>::Failure(MakeError(FrontendErrors::GuiAlreadyComposed));
        if (cancellation.IsCancellationRequested()) {
            Abort();
            return Result<void>::Failure(MakeError(FrontendErrors::GuiCancelled));
        }
        try {
            GuiCompositionState state;
            if (const auto prepared = PrepareGuiComposition(executor, state); prepared.HasError()) {
                Abort();
                return prepared;
            }
            const GuiCompositionRequest request{frame_,
                                                state.prepared.mode,
                                                state.submission,
                                                std::span{state.resources}.first(state.prepared.resources.size()),
                                                state.lease,
                                                &state.transfer,
                                                state.extendsExisting,
                                                std::span{state.buffers}.first(state.prepared.buffers.size())};
            if (const auto retained = backend_->PrepareGuiComposition(request); retained.HasError()) {
                Abort();
                return retained;
            }
            if (state.lease != nullptr && state.transfer.authority != RenderGraphLeaseAuthority::BackendCompletion) {
                Abort();
                return Result<void>::Failure(MakeError(FrontendErrors::ResourceNotReady));
            }
            if (const auto encoded = executor.RenderDrawData(request); encoded.HasError()) {
                Abort();
                return encoded;
            }
            if (state.lease != nullptr)
                completionLease_ = state.lease;
            *owner_->submissionAllocator_ = state.history;
            guiComposed_ = true;
            return Result<void>::Success();
        } catch (...) {  // NOSONAR(cpp:S2738) GUI exceptions must leave native completion ownership intact.
            Abort();
            return Result<void>::Failure(MakeError(FrontendErrors::FrameException));
        }
    }
}  // namespace Horo::Render
