#include "D3D12CommandInternals.h"
#include "D3D12NativeCommands.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12Detail {
    /** @copydoc CommandService::~CommandService */
    CommandService::~CommandService() {
        // Resources has already proved completion or actual device removal.
        for (auto &recording : recordings_) {
            recording.list.Reset();
            recording.allocator.Reset();
            ReleaseLease(recording);
        }
    }

    /** @copydoc CommandService::Initialize */
    Result<void> CommandService::Initialize() {
        if (context_.testHooks_.initialCommandId != 0)
            nextId_ = context_.testHooks_.initialCommandId;
        recordings_.resize(context_.limits_.maximumCommandBatches);
        for (auto &recording : recordings_) {
            HRESULT status = context_.device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&recording.allocator));
            if (SUCCEEDED(status))
                status = context_.device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, recording.allocator.Get(), nullptr,
                                                             IID_PPV_ARGS(&recording.list));
            if (SUCCEEDED(status))
                status = recording.list->Close();
            if (FAILED(status))
                return Result<void>::Failure(
                    ResourceError("render.d3d12.command_initialize_failed", "Prepare the bounded command allocator/list slots.", status));
        }
        return Result<void>::Success();
    }

    /** @copydoc CommandService::Configure */
    Result<void> CommandService::Configure(RenderQueueId queue) {
        if (auto checked = context_.Check(); checked.HasError())
            return checked;
        if (!queue.IsValid() || (queue_.IsValid() && queue_ != queue))
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_queue_invalid", "Bind one exact frontend direct queue before command recording."));
        queue_ = queue;
        return Result<void>::Success();
    }

    /** @copydoc CommandService::Find */
    Result<CommandService::Recording *> CommandService::Find(D3D12CommandId id) {
        if (auto checked = context_.CheckOwner(); checked.HasError())
            return Result<Recording *>::Failure(std::move(checked).ErrorValue());
        if (id.deviceIncarnation == context_.deviceIncarnation_ && id.value != 0) {
            for (auto &recording : recordings_)
                if (recording.state != State::Free && recording.id == id)
                    return Result<Recording *>::Success(&recording);
        }
        return Result<Recording *>::Failure(
            ResourceError("render.d3d12.command_invalid", "Use an outstanding command ticket from this exact device incarnation."));
    }

    /** @copydoc CommandService::Begin */
    Result<D3D12CommandId> CommandService::Begin() {
        if (auto checked = context_.Check(); checked.HasError())
            return Result<D3D12CommandId>::Failure(std::move(checked).ErrorValue());
        if (!queue_.IsValid())
            return Result<D3D12CommandId>::Failure(
                ResourceError("render.d3d12.command_queue_invalid", "Configure the effective queue first."));
        if (auto polled = context_.Poll(); polled.HasError())
            return Result<D3D12CommandId>::Failure(std::move(polled).ErrorValue());
        const auto free = std::ranges::find_if(recordings_, [](const Recording &entry) {
            return entry.state == State::Free;
        });
        if (free == recordings_.end() || nextId_ == std::numeric_limits<std::uint64_t>::max())
            return Result<D3D12CommandId>::Failure(
                ResourceError("render.d3d12.command_capacity", "Cancel completed/unsent tickets or drain pending commands."));
        HRESULT status = free->allocator->Reset();
        if (SUCCEEDED(status))
            status = free->list->Reset(free->allocator.Get(), nullptr);
        if (FAILED(status)) {
            free->state = State::Failed;
            return Result<D3D12CommandId>::Failure(
                ResourceError("render.d3d12.command_reset_failed", "Reset an unused native recording slot.", status));
        }
        free->id = {context_.deviceIncarnation_, nextId_++};
        free->state = State::Recording;
        free->abandoned = false;
        free->signalPending = false;
        return Result<D3D12CommandId>::Success(free->id);
    }

    /** @copydoc CommandService::Borrow */
    Result<ID3D12GraphicsCommandList *> CommandService::Borrow(D3D12CommandId id) {
        auto found = Find(id);
        if (found.HasError())
            return Result<ID3D12GraphicsCommandList *>::Failure(std::move(found).ErrorValue());
        if (auto checked = context_.Check(); checked.HasError())
            return Result<ID3D12GraphicsCommandList *>::Failure(std::move(checked).ErrorValue());
        if (found.Value()->state != State::Recording)
            return Result<ID3D12GraphicsCommandList *>::Failure(
                ResourceError("render.d3d12.command_state", "Borrow only an open command recording."));
        return Result<ID3D12GraphicsCommandList *>::Success(found.Value()->list.Get());
    }

    /** @copydoc CommandService::Close */
    Result<void> CommandService::Close(D3D12CommandId id) {
        auto borrowed = Borrow(id);
        if (borrowed.HasError())
            return Result<void>::Failure(std::move(borrowed).ErrorValue());
        auto *recording = Find(id).Value();
        const HRESULT status = borrowed.Value()->Close();
        recording->state = FAILED(status) ? State::Failed : State::Closed;
        if (FAILED(status))
            return Result<void>::Failure(ResourceError("render.d3d12.command_close_failed", "Discard the failed recording.", status));
        return Result<void>::Success();
    }

    /** @copydoc CommandService::Submit */
    Result<void> CommandService::Submit(D3D12CommandId id, const RenderQueueSubmission &submission, IRenderGraphResourceLease *lease,
                                        RenderGraphLeaseTransfer &transfer, const CancellationToken &cancellation) {
        auto found = Find(id);
        if (found.HasError())
            return Result<void>::Failure(std::move(found).ErrorValue());
        if (auto checked = context_.Check(); checked.HasError())
            return checked;
        if (auto admitted = ValidateSubmission(*found.Value(), submission, lease, transfer, cancellation); admitted.HasError())
            return admitted;
        if (auto polled = context_.Poll(); polled.HasError())
            return polled;
        if (auto checked = context_.Check(); checked.HasError())
            return checked;
        if (auto admitted = ValidateSubmission(*found.Value(), submission, lease, transfer, cancellation); admitted.HasError())
            return admitted;
        if (context_.nextFence_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(ResourceError("render.d3d12.command_capacity", "The shared native fence timeline is exhausted."));
        auto &recording = *found.Value();
        recording.fence = context_.nextFence_++;
        recording.lease = lease;
        recording.state = State::Submitted;
        lastOrder_ = submission.order.value;
        lastSignal_ = submission.signal.value;
        transfer.authority = RenderGraphLeaseAuthority::BackendCompletion;
        ID3D12CommandList *lists[]{recording.list.Get()};
        context_.queue_->ExecuteCommandLists(1, lists);
        if (context_.testHooks_.afterExecute)
            context_.testHooks_.afterExecute(context_.device_.Get());
        const HRESULT status = Signal(recording);
        recording.signalPending = FAILED(status);
        if (auto checked = context_.Check(false); checked.HasError())
            return checked;
        if (FAILED(status))
            return Result<void>::Failure(ResourceError("render.d3d12.signal_failed",
                                                       "Commands accepted; retain their lease and retry signaling through Poll.", status));
        return Result<void>::Success();
    }

    /** @copydoc CommandService::ValidateSubmission */
    Result<void> CommandService::ValidateSubmission(const Recording &recording, const RenderQueueSubmission &submission,
                                                    IRenderGraphResourceLease *lease, const RenderGraphLeaseTransfer &transfer,
                                                    const CancellationToken &cancellation) const {
        if (recording.state != State::Closed || transfer.authority != RenderGraphLeaseAuthority::Frontend)
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_state", "Submit a closed ticket with an untransferred receipt."));
        if (!submission.IsValid() || submission.queue != queue_ || submission.order.value <= lastOrder_ ||
            submission.signal.value <= lastSignal_)
            return Result<void>::Failure(ResourceError("render.d3d12.command_submission_invalid",
                                                       "Preserve the configured queue and strictly increasing frontend order/signal."));
        if (lease && std::ranges::any_of(recordings_, [lease](const Recording &entry) {
            return entry.lease == lease;
        }))
            return Result<void>::Failure(ResourceError("render.d3d12.command_lease_invalid",
                                                       "One outstanding frontend lease has exactly one command completion owner."));
        if (!submission.waits.empty())
            return Result<void>::Failure(ResourceError("render.d3d12.command_wait_unsupported",
                                                       "GPU timeline wait lowering is not implemented; no queue remapping is performed."));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(ResourceError("render.d3d12.cancelled", "Command submission cancelled before native execution."));
        return Result<void>::Success();
    }

    /** @copydoc CommandService::Poll */
    Result<bool> CommandService::Poll(D3D12CommandId id) {
        auto found = Find(id);
        if (found.HasError())
            return Result<bool>::Failure(std::move(found).ErrorValue());
        if (found.Value()->abandoned)
            return Result<bool>::Failure(ResourceError("render.d3d12.command_invalid", "Cancelled command observations cannot be reused."));
        if (found.Value()->state != State::Submitted && found.Value()->state != State::Complete)
            return Result<bool>::Failure(ResourceError("render.d3d12.command_state", "Poll only a submitted command ticket."));
        if (auto polled = context_.Poll(); polled.HasError())
            return Result<bool>::Failure(std::move(polled).ErrorValue());
        return Result<bool>::Success(found.Value()->state == State::Complete);
    }

    /** @copydoc CommandService::Cancel */
    Result<void> CommandService::Cancel(D3D12CommandId id) {
        auto found = Find(id);
        if (found.HasError())
            return Result<void>::Failure(std::move(found).ErrorValue());
        auto &recording = *found.Value();
        if (recording.state == State::Recording) {
            const HRESULT status = recording.list->Close();
            if (FAILED(status)) {
                recording.state = State::Failed;
                return Result<void>::Failure(
                    ResourceError("render.d3d12.command_close_failed", "The failed recording slot requires context recreation.", status));
            }
        }
        if (recording.state == State::Failed)
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_state", "A failed recording slot requires context recreation."));
        if (recording.state == State::Submitted)
            recording.abandoned = true;
        else
            recording.state = State::Free;
        return Result<void>::Success();
    }

    /** @copydoc CommandService::RetrySignals */
    Result<void> CommandService::RetrySignals() {
        for (auto &recording : recordings_) {
            if (!recording.signalPending)
                continue;
            const HRESULT status = Signal(recording);
            if (FAILED(status))
                return Result<void>::Failure(
                    ResourceError("render.d3d12.signal_failed", "Retain accepted commands until completion signaling succeeds.", status));
            recording.signalPending = false;
        }
        return Result<void>::Success();
    }

    /** @copydoc CommandService::Signal */
    HRESULT CommandService::Signal(const Recording &recording) noexcept {
        if (context_.testHooks_.commandSignalFailures != 0) {
            --context_.testHooks_.commandSignalFailures;
            return E_FAIL;
        }
        return context_.queue_->Signal(context_.fence_.Get(), recording.fence);
    }

    /** @copydoc CommandService::ReleaseLease */
    void CommandService::ReleaseLease(Recording &recording) noexcept {
        if (auto *lease = std::exchange(recording.lease, nullptr))
            lease->Release();
    }

    /** @copydoc CommandService::Reclaim */
    void CommandService::Reclaim(std::uint64_t completed) noexcept {
        for (auto &recording : recordings_) {
            if (recording.state == State::Submitted && !recording.signalPending && recording.fence <= completed) {
                recording.state = recording.abandoned ? State::Free : State::Complete;
                ReleaseLease(recording);
            }
        }
    }

    /** @copydoc CommandService::StopAdmission */
    void CommandService::StopAdmission() noexcept {
        for (auto &recording : recordings_) {
            if (recording.state == State::Submitted)
                recording.abandoned = true;
            else
                recording.state = State::Free;
        }
    }

    /** @copydoc CommandService::HasSubmitted */
    bool CommandService::HasSubmitted() const noexcept {
        return std::ranges::any_of(recordings_, [](const Recording &entry) {
            return entry.state == State::Submitted;
        });
    }
}  // namespace Horo::Render::D3D12Detail

namespace Horo::Render {
    /** @copydoc BorrowD3D12CommandList */
    Result<ID3D12GraphicsCommandList *> BorrowD3D12CommandList(ID3D12Resources &resources, D3D12CommandId id) {
        auto *commands = dynamic_cast<D3D12Detail::CommandService *>(resources.Commands());
        if (!commands)
            return Result<ID3D12GraphicsCommandList *>::Failure(
                D3D12InitializationError("render.d3d12.commands_unavailable", "Use the native resource context on its owner thread."));
        return commands->Borrow(id);
    }
}  // namespace Horo::Render
#endif
