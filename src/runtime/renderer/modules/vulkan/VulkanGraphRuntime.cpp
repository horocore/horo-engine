#include "VulkanGraphRuntimeInternal.h"

#include <algorithm>
#include <bit>
#include <exception>
#include <limits>

namespace Horo::Render::Detail {
    /** @copydoc VulkanGraphRuntime::Impl::Access */
    Result<void> VulkanGraphRuntime::Impl::Access(const bool admission) const {
        if (owner == std::thread::id{})
            return Result<void>::Failure(MakeError(VulkanBackendErrors::NotInitialized));
        if (owner != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(VulkanBackendErrors::WrongThread));
        if (admission && (lifecycle.closing || lifecycle.failedSubmission))
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        return Result<void>::Success();
    }

    const VulkanCommandQueue *VulkanGraphRuntime::Impl::Queue(const RenderQueueId id) const noexcept {
        const auto found = std::ranges::find(queues, id, &VulkanCommandQueue::identity);
        return found == queues.end() ? nullptr : std::to_address(found);
    }

    VulkanGraphRuntime::VulkanGraphRuntime() : impl_(std::make_unique<Impl>()) {}

    VulkanGraphRuntime::~VulkanGraphRuntime() {
        try {
            if (impl_->owner != std::thread::id{}) {
                auto closing = BeginShutdown();
                auto drained = PollDrain();
                if (closing.HasError() || drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
                    std::terminate();
            }

        } catch (...) {
            // Destruction cannot release native owners without proven terminal retirement.
            std::terminate();
        }
    }

    /** @copydoc VulkanGraphRuntime::Impl::ReserveStorage */
    Result<void> VulkanGraphRuntime::Impl::ReserveStorage(const std::span<const VulkanCommandQueue> sourceQueues,
                                                          const std::uint32_t frames) {
        try {
            queues.assign(sourceQueues.begin(), sourceQueues.end());
            bindings.resize(RenderGraphLimits::HardMaxResources);
            states.resize(RenderGraphLimits::HardMaxResources);
            identities.reserve(RenderGraphLimits::HardMaxResources);
            used.resize(RenderGraphLimits::HardMaxResources);
            discarded.resize(RenderGraphLimits::HardMaxResources);
            passes.resize(RenderGraphLimits::HardMaxPasses);
            barriers.reserve(RenderGraphLimits::HardMaxUsages * 3);
            updates.reserve(RenderGraphLimits::HardMaxUsages);
            submissions.reserve(RenderGraphSubmissionLimits::MaximumSubmissions);
            retention.resize(frames);
            for (auto &pins : retention)
                pins.resources.reserve(RenderGraphLimits::HardMaxResources + MultiviewExtractionLimits::HardMaximumInstances * 3);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(
                MakeError(VulkanBackendErrors::ResourceCreationFailed, "Bounded graph storage allocation failed."));
        }
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::Initialize */
    Result<void> VulkanGraphRuntime::Initialize(const VulkanGraphInitialization &request) {
        if (impl_->owner != std::thread::id{} || request.frames == 0 || request.frames > 8 || !request.resolver)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidConfig));
        for (std::size_t index = 0; index < request.queues.size(); ++index)
            for (std::size_t previous = 0; previous < index; ++previous)
                if (request.queues[index].family == request.queues[previous].family)
                    return Result<void>::Failure(
                        Impl::Unsupported("Distinct Horo queues require distinct native families in this whole-resource ownership path."));
        Impl::Dispatch dispatch{.barrier =
                                    std::bit_cast<PFN_vkCmdPipelineBarrier2>(request.resolver(request.device, "vkCmdPipelineBarrier2")),
                                .copy = std::bit_cast<PFN_vkCmdCopyBuffer2>(request.resolver(request.device, "vkCmdCopyBuffer2")),
                                .beginRendering =
                                    std::bit_cast<PFN_vkCmdBeginRendering>(request.resolver(request.device, "vkCmdBeginRendering")),
                                .endRendering =
                                    std::bit_cast<PFN_vkCmdEndRendering>(request.resolver(request.device, "vkCmdEndRendering"))};
        if (!dispatch.barrier || !dispatch.copy || !dispatch.beginRendering || !dispatch.endRendering)
            return Result<void>::Failure(
                MakeError(VulkanBackendErrors::EntryPointMissing, "Graph execution requires Vulkan1.3 core command dispatch."));
        if (const auto storage = impl_->ReserveStorage(request.queues, request.frames); storage.HasError())
            return storage;
        if (auto initialized = impl_->commands.Initialize(request.device, request.queues, RenderGraphSubmissionLimits::MaximumSubmissions,
                                                          request.timelineDifference, request.deviceIncarnation, request.resolver);
            initialized.HasError())
            return initialized;
        dispatch.copyImage = std::bit_cast<PFN_vkCmdCopyBufferToImage2>(request.resolver(request.device, "vkCmdCopyBufferToImage2"));
        impl_->native = dispatch;
        impl_->LoadMaterialDispatch(request.device, request.resolver);
        impl_->multiview.Initialize(request.device, request.deviceIncarnation, request.vertexLayerEnabled, request.resolver);
        impl_->resources = &request.resources;
        impl_->deviceIncarnation = request.deviceIncarnation;
        impl_->owner = std::this_thread::get_id();
        impl_->lifecycle.closing = false;
        impl_->lifecycle.failedSubmission = false;
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::BeginFrame */
    Result<void> VulkanGraphRuntime::BeginFrame(const FrameToken frame) {
        if (auto access = impl_->Access(); access.HasError())
            return access;
        if (!frame.IsValid() || impl_->lifecycle.frame.IsValid())
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        if (auto polled = impl_->commands.Poll(); polled.HasError())
            return polled;
        const auto slot = std::ranges::find_if(impl_->retention, [](const VulkanCommandRetention &pins) {
            return pins.references == 0 && pins.lease == nullptr;
        });
        if (slot == impl_->retention.end())
            return Result<void>::Failure(MakeError(VulkanBackendErrors::SubmissionBusy));
        slot->resources.clear();
        impl_->lifecycle.pins = std::addressof(*slot);
        impl_->lifecycle.frame = frame;
        impl_->multiview.Reset();
        impl_->guiPrepared = false;
        impl_->guiRecorded = false;
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::FrameSlot */
    Result<VulkanFrameRetentionSlot> VulkanGraphRuntime::FrameSlot(const FrameToken frame) const {
        if (const auto access = impl_->Access(); access.HasError())
            return Result<VulkanFrameRetentionSlot>::Failure(access.ErrorValue());
        if (!frame.IsValid() || frame != impl_->lifecycle.frame || impl_->lifecycle.pins == nullptr)
            return Result<VulkanFrameRetentionSlot>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        return Result<VulkanFrameRetentionSlot>::Success({static_cast<std::uint32_t>(impl_->lifecycle.pins - impl_->retention.data()),
                                                          static_cast<std::uint32_t>(impl_->retention.size())});
    }

    /** @copydoc VulkanGraphRuntime::CanWriteFrameInputs */
    bool VulkanGraphRuntime::CanWriteFrameInputs(const FrameToken frame) const noexcept {
        return impl_->owner == std::this_thread::get_id() && !impl_->lifecycle.closing && !impl_->lifecycle.failedSubmission &&
               frame.IsValid() && frame == impl_->lifecycle.frame && impl_->lifecycle.pins != nullptr && !impl_->lifecycle.recorded &&
               impl_->lifecycle.pins->references == 0;
    }

    /** @copydoc VulkanGraphRuntime::ExecuteGraph */
    Result<void> VulkanGraphRuntime::ExecuteGraph(const RenderGraphExecutionRequest &request) {
        if (auto access = impl_->Access(); access.HasError())
            return access;
        if (request.frame != impl_->lifecycle.frame || !impl_->lifecycle.frame.IsValid() || impl_->lifecycle.recorded)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        if (auto valid = ValidateRenderGraphExecutionRequest(request); valid.HasError())
            return valid;
        if (request.submissions.empty())
            return Result<void>::Failure(Impl::Unsupported("Vulkan requires explicit Horo submission/timeline evidence."));
        if (!request.resources.empty() && (request.lease == nullptr || request.transfer == nullptr))
            return Result<void>::Failure(
                Impl::Invalid("Resident native graph work requires the frontend completion lease and synchronous transfer receipt."));
        if (auto timelines = impl_->commands.ValidateBatch(request.submissions); timelines.HasError())
            return timelines;
        auto *free = impl_->lifecycle.pins;
        if (free == nullptr || free->references != 0 || free->lease != nullptr)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        if (auto captured = impl_->resources->CaptureGraphResources(request, impl_->bindings, *free); captured.HasError()) {
            free->resources.clear();
            return captured;
        }
        if (auto prepared = impl_->Prepare(request, *free); prepared.HasError()) {
            free->resources.clear();
            return prepared;
        }
        if (auto recorded = impl_->Record(request, *free); recorded.HasError()) {
            impl_->Abort();
            free->resources.clear();
            return recorded;
        }
        impl_->lifecycle.recorded = true;
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::SubmitFrame */
    Result<void> VulkanGraphRuntime::SubmitFrame(const FrameToken frame) {
        if (auto access = impl_->Access(); access.HasError())
            return access;
        if (!frame.IsValid() || frame != impl_->lifecycle.frame)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        for (const auto &submission : impl_->submissions) {
            if (auto accepted = impl_->commands.Submit(submission.ticket, submission.value, submission.presentationSignal);
                accepted.HasError()) {
                impl_->lifecycle.failedSubmission = true;
                impl_->Abort();
                return accepted;
            }
            for (const auto &update : std::span{impl_->updates}.subspan(submission.updates.offset, submission.updates.count))
                *update.target = update.value;
        }
        impl_->submissions.clear();
        impl_->updates.clear();
        impl_->barriers.clear();
        impl_->lifecycle.frame = {};
        impl_->lifecycle.pins = nullptr;
        impl_->lifecycle.recorded = false;
        return Result<void>::Success();
    }

    void VulkanGraphRuntime::Impl::Abort() noexcept {
        multiview.Reset();
        for (const auto &submission : submissions)
            commands.Abort(submission.ticket);
        submissions.clear();
        updates.clear();
        barriers.clear();
        if (lifecycle.pins != nullptr && lifecycle.pins->references == 0) {
            lifecycle.pins->resources.clear();
            if (lifecycle.pins->lease != nullptr)
                std::exchange(lifecycle.pins->lease, nullptr)->Release();
        }
        guiPrepared = false;
        guiRecorded = false;
        lifecycle.pins = nullptr;
        lifecycle.recorded = false;
        lifecycle.frame = {};
    }

    /** @copydoc VulkanGraphRuntime::AbortFrame */
    void VulkanGraphRuntime::AbortFrame(const FrameToken frame) noexcept {
        if (impl_->owner == std::this_thread::get_id() && frame == impl_->lifecycle.frame)
            impl_->Abort();
    }

    /** @copydoc VulkanGraphRuntime::ObserveSubmission */
    Result<bool> VulkanGraphRuntime::ObserveSubmission(const RenderCpuWait &wait) {
        if (auto access = impl_->Access(false); access.HasError())
            return Result<bool>::Failure(std::move(access).ErrorValue());
        if (impl_->lifecycle.frame.IsValid() && wait.purpose != RenderCpuWaitPurpose::Poll)
            return Result<bool>::Failure(
                MakeError(VulkanBackendErrors::TimelineInvalid, "An active frame cannot issue a blocking CPU wait."));
        return impl_->commands.Observe(wait);
    }

    /** @copydoc VulkanGraphRuntime::MultiviewSupport */
    MultiviewCapabilities VulkanGraphRuntime::MultiviewSupport() const noexcept {
        return impl_->owner == std::thread::id{} ? MultiviewCapabilities{} : impl_->multiview.Capabilities();
    }

    /** @copydoc VulkanGraphRuntime::BeginShutdown */
    Result<void> VulkanGraphRuntime::BeginShutdown() {
        if (impl_->owner == std::thread::id{})
            return Result<void>::Success();
        if (auto access = impl_->Access(false); access.HasError())
            return access;
        impl_->lifecycle.closing = true;
        impl_->Abort();
        return impl_->commands.BeginShutdown();
    }

    /** @copydoc VulkanGraphRuntime::PollDrain */
    Result<RenderBackendDrainProgress> VulkanGraphRuntime::PollDrain() {
        if (impl_->owner == std::thread::id{})
            return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Drained);
        if (auto access = impl_->Access(false); access.HasError())
            return Result<RenderBackendDrainProgress>::Failure(std::move(access).ErrorValue());
        if (!impl_->lifecycle.closing)
            return Result<RenderBackendDrainProgress>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        auto drained = impl_->commands.PollDrain();
        if (drained.HasValue() && drained.Value() == RenderBackendDrainProgress::Drained) {
            for (const auto &pins : impl_->retention)
                if (pins.references != 0 || pins.lease != nullptr)
                    std::terminate();
            impl_->resources = nullptr;
            impl_->owner = {};
        }
        return drained;
    }
}  // namespace Horo::Render::Detail
