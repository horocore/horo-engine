#include "D3D12NativeCommandsInternal.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12Detail {
    namespace {
        /** @brief Converts finite nanoseconds upward without overflowing the Windows finite timeout range. */
        Result<DWORD> ObservationTimeout(const std::chrono::nanoseconds timeout) {
            const auto nanoseconds = static_cast<std::uint64_t>(timeout.count());
            const auto milliseconds = nanoseconds / 1'000'000 + (nanoseconds % 1'000'000 != 0);
            if (milliseconds >= INFINITE)
                return Result<DWORD>::Failure(
                    ResourceError("render.d3d12.command_wait_failed", "Choose a finite Windows-representable CPU observation deadline."));
            return Result<DWORD>::Success(static_cast<DWORD>(milliseconds));
        }

        /** @brief Blocking observations are explicit and finite; completion still comes only from the native fence. */
        Result<bool> WaitForFence(ID3D12Fence &fence, HANDLE event, const RenderCpuWait &wait) {
            auto timeout = ObservationTimeout(wait.timeout);
            if (timeout.HasError())
                return Result<bool>::Failure(timeout.ErrorValue());
            const HRESULT status = fence.SetEventOnCompletion(wait.completion.value, event);
            if (FAILED(status))
                return Result<bool>::Failure(
                    ResourceError("render.d3d12.command_wait_failed", "Register exact fence completion observation.", status));
            const DWORD result = WaitForSingleObject(event, timeout.Value());
            if (result == WAIT_TIMEOUT)
                return Result<bool>::Success(false);
            if (result != WAIT_OBJECT_0)
                return Result<bool>::Failure(ResourceError("render.d3d12.command_wait_failed",
                                                           "Observe native fence with a finite CPU deadline.",
                                                           HRESULT_FROM_WIN32(GetLastError())));
            const auto completed = fence.GetCompletedValue();
            if (completed == UINT64_MAX)
                return Result<bool>::Failure(
                    ResourceError("render.d3d12.device_lost", "The device was removed during CPU fence observation."));
            return Result<bool>::Success(completed >= wait.completion.value);
        }
    }  // namespace

    Result<void> NativeCommandRuntime::WaitForDependencies(CommandQueue &queue, const CommandSlot &slot) {
        for (const auto &wait : slot.waits) {
            auto *producer = FindQueue(wait.queue);
            if (!producer || producer->issued < wait.value)
                return Result<void>::Failure(
                    ResourceError("render.d3d12.timeline_unissued", "Wait only for native submissions actually issued on this device."));
            const HRESULT status = queue.native->Wait(producer->fence.Get(), wait.value);
            if (FAILED(status))
                return Result<void>::Failure(ResourceError("render.d3d12.command_wait_failed",
                                                           "Queue the exact producer timeline dependency before execution.", status));
        }
        if (auto transfers = WaitForTransferAccess(queue); transfers.HasError())
            return transfers;
        return WaitForPriorResourceAccess(queue, slot);
    }

    Result<void> NativeCommandRuntime::WaitForTransferAccess(CommandQueue &queue) {
        if (resources_.lastSubmittedFence_ != 0) {
            const HRESULT status = queue.native->Wait(resources_.fence_.Get(), resources_.lastSubmittedFence_);
            if (FAILED(status))
                return Result<void>::Failure(
                    ResourceError("render.d3d12.command_wait_failed", "Order graph admission after canonical resource transfers.", status));
        }
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::WaitForPriorResourceAccess(CommandQueue &queue, const CommandSlot &slot) {
        for (const auto index : slot.accessed) {
            const auto &resource = *slot.pins[index];
            for (std::size_t dependency = 0; dependency < resource.commandFences.size(); ++dependency) {
                if (!resource.commandFences[dependency])
                    continue;
                const HRESULT status =
                    queue.native->Wait(resource.commandFences[dependency].Get(), resource.commandFenceValues[dependency]);
                if (FAILED(status))
                    return Result<void>::Failure(ResourceError("render.d3d12.command_wait_failed",
                                                               "Preserve the same resource's prior native COMMON boundary.", status));
            }
        }
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::SignalSlot(CommandQueue &queue, CommandSlot &slot) {
        const HRESULT status = queue.native->Signal(queue.fence.Get(), slot.signal);
        const HRESULT removed = device_->GetDeviceRemovedReason();
        slot.signalPending = FAILED(status);
        if (FAILED(removed))
            return Result<void>::Failure(
                ResourceError("render.d3d12.device_lost",
                              "The device was removed after command execution; retain native ownership until disposal.", removed));
        if (FAILED(status))
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_signal_failed",
                              "Commands were submitted; Poll retries signaling while retaining allocators and resource pins.", status));
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::SubmitSlot(CommandQueue &queue, CommandSlot &slot) {
        resources_.commandTimelineUsed_ = true;
        slot.submitted = true;
        queue.issued = std::max(queue.issued, slot.signal);
        ID3D12CommandList *list = slot.list.Get();
        queue.native->ExecuteCommandLists(1, &list);
        for (const auto index : slot.accessed) {
            auto &resource = *slot.pins[index];
            const auto kind = static_cast<std::size_t>(queue.kind);
            resource.commandFences[kind] = queue.fence;
            resource.commandFenceValues[kind] = slot.signal;
            resource.activationPending = false;
        }
        if (hooks_.afterExecute)
            hooks_.afterExecute(device_.Get());
        return SignalSlot(queue, slot);
    }

    Result<void> NativeCommandRuntime::SubmitPrepared(IRenderGraphResourceLease *lease, RenderGraphLeaseTransfer *transfer) {
        for (const auto &reference : prepared_) {
            auto &queue = queues_[reference.queue];
            auto &slot = queue.slots[reference.slot];
            if (auto waited = WaitForDependencies(queue, slot); waited.HasError()) {
                AbortPrepared();
                return waited;
            }
            preparedLease_->lease = lease;
            preparedLease_->owned = true;
            transfer->authority = RenderGraphLeaseAuthority::BackendCompletion;
            if (auto submitted = SubmitSlot(queue, slot); submitted.HasError()) {
                AbortPrepared();
                return submitted;
            }
        }
        AbortPrepared();
        return Result<void>::Success();
    }

    Result<bool> NativeCommandRuntime::Observe(const RenderCpuWait &wait) {
        if (auto checked = CheckDevice(); checked.HasError())
            return Result<bool>::Failure(checked.ErrorValue());
        auto *queue = FindQueue(wait.completion.queue);
        if (!queue || wait.expectedDeviceIncarnation != incarnation_ || wait.completion.value > queue->issued)
            return Result<bool>::Failure(
                ResourceError("render.d3d12.timeline_unissued", "Observe an actually submitted point from this exact native incarnation."));
        const auto completed = queue->fence->GetCompletedValue();
        if (completed == UINT64_MAX)
            return Result<bool>::Failure(
                ResourceError("render.d3d12.device_lost", "The native queue fence reports removal, not successful completion."));
        if (completed >= wait.completion.value)
            return Result<bool>::Success(true);
        if (wait.purpose == RenderCpuWaitPurpose::Poll)
            return Result<bool>::Success(false);
        return WaitForFence(*queue->fence.Get(), queue->observationEvent.get(), wait);
    }
}  // namespace Horo::Render::D3D12Detail
#endif
