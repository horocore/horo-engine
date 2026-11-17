#include "D3D12NativeCommandsInternal.h"

#include <new>

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12Detail {
    D3D12_COMMAND_LIST_TYPE CommandListType(const D3D12QueueKind kind) noexcept {
        switch (kind) {
            case D3D12QueueKind::Direct:
                return D3D12_COMMAND_LIST_TYPE_DIRECT;
            case D3D12QueueKind::Compute:
                return D3D12_COMMAND_LIST_TYPE_COMPUTE;
            case D3D12QueueKind::Copy:
                return D3D12_COMMAND_LIST_TYPE_COPY;
        }
        HORO_INVARIANT(false);
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    }

    NativeCommandRuntime::NativeCommandRuntime(ID3D12Device *device, const std::array<ID3D12CommandQueue *, 3> &queues,
                                               Resources &resources, const D3D12CommandTestHooks hooks)
        : device_(device), resources_(resources), hooks_(hooks) {
        for (std::size_t index = 0; index < queues.size(); ++index)
            nativeQueues_[index] = queues[index];
    }

    NativeCommandRuntime::~NativeCommandRuntime() {
        const bool lost = FAILED(device_->GetDeviceRemovedReason());
        for (const auto &queue : queues_)
            for (const auto &slot : queue.slots)
                HORO_INVARIANT_MSG(!slot.submitted || lost, "Drain submitted D3D12 command allocators before native destruction.");
        if (resources_.commandOwner_ == this)
            resources_.commandOwner_ = nullptr;
    }

    Result<void> NativeCommandRuntime::CheckDevice() const {
        return resources_.Check(false);
    }

    Result<void> NativeCommandRuntime::InitializeSlot(CommandQueue &queue, CommandSlot &slot) {
        HRESULT status = device_->CreateCommandAllocator(CommandListType(queue.kind), IID_PPV_ARGS(&slot.allocator));
        if (SUCCEEDED(status))
            status = device_->CreateCommandList(0, CommandListType(queue.kind), slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.list));
        if (SUCCEEDED(status))
            status = slot.list->Close();
        if (FAILED(status))
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_initialization", "Create bounded allocator/list storage.", status));
        if (mode_ == D3D12BarrierMode::Enhanced) {
            status = slot.list.As(&slot.enhanced);
            if (FAILED(status))
                return Result<void>::Failure(ResourceError("render.d3d12.enhanced_barriers_unsupported",
                                                           "The explicitly selected enhanced mode requires command-list interface seven.",
                                                           status));
        }
        slot.pins.reserve(limits_.maximumResourceReferences);
        slot.accessed.reserve(limits_.maximumResourceReferences);
        slot.waits.reserve(RenderGraphSubmissionLimits::MaximumSubmissions);
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::InitializeQueue(CommandQueue &queue, const std::uint32_t slots) {
        if (!queue.native)
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_queue", "Create every explicitly mapped native queue before command admission."));
        ComPtr<ID3D12Device> queueDevice;
        const HRESULT queriedDevice = queue.native->GetDevice(IID_PPV_ARGS(&queueDevice));
        if (FAILED(queriedDevice) || queueDevice.Get() != device_.Get())
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_queue", "Bind a native queue from this exact admitted device.", queriedDevice));
        const auto description = queue.native->GetDesc();
        if (description.Type != CommandListType(queue.kind))
            return Result<void>::Failure(ResourceError("render.d3d12.command_queue", "Bind the declared native queue class exactly."));
        const HRESULT status = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue.fence));
        if (FAILED(status))
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_fence", "Create the exact queue completion timeline.", status));
        queue.observationEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!queue.observationEvent)
            return Result<void>::Failure(ResourceError("render.d3d12.command_initialization", "Create bounded queue observation storage.",
                                                       HRESULT_FROM_WIN32(GetLastError())));
        queue.slots.resize(slots);
        for (auto &slot : queue.slots)
            if (auto initialized = InitializeSlot(queue, slot); initialized.HasError())
                return initialized;
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::ClaimResourceOwner(const D3D12CommandConfiguration &configuration) {
        if (resources_.device_.Get() != device_.Get() || resources_.deviceIncarnation_ != configuration.deviceIncarnation)
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_resource", "Use the same admitted device and canonical resource incarnation."));
        if (resources_.commandOwner_ || resources_.commandTimelineUsed_)
            return Result<void>::Failure(ResourceError("render.d3d12.command_owner_busy",
                                                       "Use the sole live command owner; issued timelines require a fresh "
                                                       "resource incarnation before replacement."));
        resources_.commandOwner_ = this;
        if (!resources_.accepting_)
            return Result<void>::Failure(ResourceError("render.d3d12.command_closed", "Canonical resource admission is already closed."));
        return Result<void>::Success();
    }

    void NativeCommandRuntime::ReserveStorage(const std::size_t queueCount) {
        queues_.reserve(queueCount);
        prepared_.reserve(queueCount * limits_.maximumPendingSubmissionsPerQueue);
        boundResources_.reserve(limits_.maximumResourceReferences);
        resourceIds_.reserve(limits_.maximumResourceReferences);
        logicalStates_.reserve(limits_.maximumResourceReferences);
        activationRecorded_.reserve(limits_.maximumResourceReferences);
        nativeStates_.reserve(limits_.maximumResourceReferences);
        const auto groupCount = queueCount * limits_.maximumPendingSubmissionsPerQueue;
        leaseGroups_.reserve(groupCount);
        for (std::size_t index = 0; index < groupCount; ++index)
            leaseGroups_.push_back(std::make_shared<CommandGraphLease>());
    }

    Result<void> NativeCommandRuntime::Initialize(const D3D12CommandConfiguration &configuration) {
        if (auto checked = CheckDevice(); checked.HasError())
            return checked;
        if (auto claimed = ClaimResourceOwner(configuration); claimed.HasError())
            return claimed;
        mode_ = configuration.barrierMode;
        limits_ = configuration.limits;
        incarnation_ = configuration.deviceIncarnation;
        if (mode_ == D3D12BarrierMode::Enhanced) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
            const HRESULT status = device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options));
            if (FAILED(status) || !options.EnhancedBarriersSupported)
                return Result<void>::Failure(ResourceError("render.d3d12.enhanced_barriers_unsupported",
                                                           "Select a device supporting the explicitly requested enhanced barrier feature.",
                                                           status));
        }
        ReserveStorage(configuration.queues.size());
        for (const auto &binding : configuration.queues) {
            CommandQueue queue;
            queue.identity = binding.queue;
            queue.kind = binding.kind;
            queue.native = nativeQueues_[static_cast<std::size_t>(binding.kind)];
            if (auto initialized = InitializeQueue(queue, limits_.maximumPendingSubmissionsPerQueue); initialized.HasError())
                return initialized;
            queues_.push_back(std::move(queue));
        }
        return Result<void>::Success();
    }

    CommandQueue *NativeCommandRuntime::FindQueue(const RenderQueueId identity) noexcept {
        const auto found = std::ranges::find(queues_, identity, &CommandQueue::identity);
        return found == queues_.end() ? nullptr : &*found;
    }

    const CommandQueue *NativeCommandRuntime::FindQueue(const RenderQueueId identity) const noexcept {
        const auto found = std::ranges::find(queues_, identity, &CommandQueue::identity);
        return found == queues_.end() ? nullptr : &*found;
    }

    void NativeCommandRuntime::ClearSlot(CommandSlot &slot) noexcept {
        slot.pins.clear();
        slot.accessed.clear();
        slot.waits.clear();
        if (slot.graphLease && slot.graphLease.use_count() == 2)
            slot.graphLease->Retire();
        slot.graphLease.reset();
        slot.signal = 0;
        slot.submitted = false;
        slot.signalPending = false;
    }

    Result<void> NativeCommandRuntime::PollQueue(CommandQueue &queue) {
        for (auto &slot : queue.slots)
            if (slot.signalPending)
                if (auto signaled = SignalSlot(queue, slot); signaled.HasError())
                    return signaled;
        const auto completed = queue.fence->GetCompletedValue();
        if (completed == UINT64_MAX)
            return Result<void>::Failure(ResourceError("render.d3d12.device_lost", "The command timeline reports actual device removal."));
        for (auto &slot : queue.slots)
            if (slot.submitted && slot.signal <= completed)
                ClearSlot(slot);
        return Result<void>::Success();
    }

    Result<void> NativeCommandRuntime::Poll() {
        if (auto checked = CheckDevice(); checked.HasError())
            return checked;
        for (auto &queue : queues_)
            if (auto polled = PollQueue(queue); polled.HasError())
                return polled;
        resources_.Reclaim();
        return Result<void>::Success();
    }

    void NativeCommandRuntime::AbortPrepared() noexcept {
        for (const auto &reference : prepared_) {
            auto &slot = queues_[reference.queue].slots[reference.slot];
            if (!slot.submitted)
                ClearSlot(slot);
        }
        prepared_.clear();
        preparedLease_.reset();
        boundResources_.clear();
        resourceIds_.clear();
        logicalStates_.clear();
        activationRecorded_.clear();
    }

    void NativeCommandRuntime::StopAdmission() noexcept {
        accepting_ = false;
        AbortPrepared();
    }

    Result<RenderBackendDrainProgress> NativeCommandRuntime::PollDrain() {
        if (auto polled = Poll(); polled.HasError())
            return Result<RenderBackendDrainProgress>::Failure(polled.ErrorValue());
        for (const auto &queue : queues_)
            for (const auto &slot : queue.slots)
                if (slot.submitted)
                    return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Pending);
        return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Drained);
    }
}  // namespace Horo::Render::D3D12Detail

namespace Horo::Render {
    Result<std::unique_ptr<ID3D12Commands>> CreateD3D12Commands(ID3D12Device *device, const std::array<ID3D12CommandQueue *, 3> &queues,
                                                                ID3D12Resources &resources, const D3D12CommandConfiguration &configuration,
                                                                const D3D12CommandTestHooks hooks) {
        auto *canonical = dynamic_cast<D3D12Detail::Resources *>(&resources);
        if (!device || !canonical)
            return Result<std::unique_ptr<ID3D12Commands>>::Failure(
                D3D12CommandError("render.d3d12.command_resource",
                                  "Provide the admitted native device and its actual canonical resource service."));
        try {
            return CreateD3D12CommandService(std::make_unique<D3D12Detail::NativeCommandRuntime>(device, queues, *canonical, hooks),
                                             configuration);
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<ID3D12Commands>>::Failure(
                D3D12CommandError("render.d3d12.command_capacity", "Unable to allocate bounded native command ownership."));
        }
    }
}  // namespace Horo::Render
#endif
