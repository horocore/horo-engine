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
        if (admission && (lifecycle.closing || lifecycle.failedSubmission || lifecycle.uploadingVfx))
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
            vfxDraws.reserve(MaximumVfxGraphDraws);
            barriers.reserve(RenderGraphLimits::HardMaxUsages * 3);
            updates.reserve(RenderGraphLimits::HardMaxUsages);
            submissions.reserve(RenderGraphSubmissionLimits::MaximumSubmissions);
            retention.resize(frames);
            for (auto &pins : retention)
                pins.resources.reserve(RenderGraphLimits::HardMaxResources +
                                       (MultiviewExtractionLimits::HardMaximumInstances + MaximumVfxGraphDraws) * 3);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(
                MakeError(VulkanBackendErrors::ResourceCreationFailed, "Bounded graph storage allocation failed."));
        }
        return Result<void>::Success();
    }

    namespace {
        bool ValidQueues(std::span<const VulkanCommandQueue> queues) {
            for (std::size_t index = 0; index < queues.size(); ++index)
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (queues[index].family == queues[previous].family)
                        return false;
            return true;
        }
    }  // namespace

    Result<void> VulkanGraphRuntime::Impl::InitializeNativeDispatch(const VulkanGraphInitialization &request) {
        Dispatch dispatch{.barrier = std::bit_cast<PFN_vkCmdPipelineBarrier2>(request.resolver(request.device, "vkCmdPipelineBarrier2")),
                          .copy = std::bit_cast<PFN_vkCmdCopyBuffer2>(request.resolver(request.device, "vkCmdCopyBuffer2")),
                          .beginRendering = std::bit_cast<PFN_vkCmdBeginRendering>(request.resolver(request.device, "vkCmdBeginRendering")),
                          .endRendering = std::bit_cast<PFN_vkCmdEndRendering>(request.resolver(request.device, "vkCmdEndRendering"))};
        if (!dispatch.barrier || !dispatch.copy || !dispatch.beginRendering || !dispatch.endRendering)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::EntryPointMissing));
        dispatch.copyImage = std::bit_cast<PFN_vkCmdCopyBufferToImage2>(request.resolver(request.device, "vkCmdCopyBufferToImage2"));
        native = dispatch;
        LoadMaterialDispatch(request.device, request.resolver);
        return Result<void>::Success();
    }

    Result<void> VulkanGraphRuntime::Impl::InitializeCommandOwners(const VulkanGraphInitialization &request) {
        if (const auto initialized = commands.Initialize(request.device, request.queues, RenderGraphSubmissionLimits::MaximumSubmissions,
                                                         request.timelineDifference, request.deviceIncarnation, request.resolver);
            initialized.HasError())
            return initialized;
        multiview.Initialize(request.device, request.deviceIncarnation, request.vertexLayerEnabled, request.resolver);
        if (request.properties)
            return vfxGpu.Initialize(request.device, *request.properties, request.frames, request.deviceIncarnation, request.queues,
                                     request.resources, commands, request.resolver);
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::Initialize */
    Result<void> VulkanGraphRuntime::Initialize(const VulkanGraphInitialization &request) {
        if (impl_->owner != std::thread::id{} || request.frames == 0 || request.frames > 8 || !request.resolver)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidConfig));
        if (!ValidQueues(request.queues))
            return Result<void>::Failure(Impl::Unsupported("Distinct queue identities require distinct native families."));
        if (const auto storage = impl_->ReserveStorage(request.queues, request.frames); storage.HasError())
            return storage;
        if (const auto dispatch = impl_->InitializeNativeDispatch(request); dispatch.HasError())
            return dispatch;
        if (const auto commands = impl_->InitializeCommandOwners(request); commands.HasError())
            return commands;
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
        if (const auto retired = impl_->vfxGpu.CollectRetired(); retired.HasError())
            return retired;
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

    Result<void> VulkanGraphRuntime::Impl::ValidateGraphAdmission(const RenderGraphExecutionRequest &request) const {
        if (auto access = Access(); access.HasError())
            return access;
        if (request.frame != lifecycle.frame || !lifecycle.frame.IsValid() || lifecycle.recorded)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        if (auto valid = ValidateRenderGraphExecutionRequest(request); valid.HasError())
            return valid;
        if (request.submissions.empty())
            return Result<void>::Failure(Unsupported("Vulkan requires exact submission evidence."));
        if (!request.resources.empty() && (request.lease == nullptr || request.transfer == nullptr))
            return Result<void>::Failure(Invalid("Resident native graph requires the frontend completion lease."));
        return commands.ValidateBatch(request.submissions);
    }

    Result<void> VulkanGraphRuntime::Impl::ExecuteGraphCommands(const RenderGraphExecutionRequest &request) {
        if (auto valid = ValidateGraphAdmission(request); valid.HasError())
            return valid;
        auto *free = lifecycle.pins;
        if (!free || free->references || free->lease)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        if (auto captured = resources->CaptureGraphResources(request, bindings, *free); captured.HasError()) {
            free->resources.clear();
            return captured;
        }
        if (auto prepared = Prepare(request, *free); prepared.HasError()) {
            free->resources.clear();
            return prepared;
        }
        if (auto recorded = Record(request, *free); recorded.HasError()) {
            Abort();
            free->resources.clear();
            return recorded;
        }
        lifecycle.recorded = true;
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::ExecuteGraph */
    Result<void> VulkanGraphRuntime::ExecuteGraph(const RenderGraphExecutionRequest &request) {
        auto result = impl_->ExecuteGraphCommands(request);
        if (result.HasError())
            impl_->vfxGpu.Reject(request);
        return result;
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
                if (accepted.ErrorValue().code.Value() == VulkanBackendErrors::DeviceLost.code.Value())
                    impl_->vfxGpu.Uncertain(submission.ticket);
                impl_->lifecycle.failedSubmission = true;
                impl_->Abort();
                return accepted;
            }
            impl_->vfxGpu.Finish(submission.ticket, true);
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
        vfxGpu.Abort();
        multiview.Reset();
        for (const auto &submission : submissions)
            commands.Abort(submission.ticket);
        submissions.clear();
        updates.clear();
        materialPassStateCount = 0;
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

    /** @copydoc VulkanGraphRuntime::UpdateVfxFrame */
    Result<void> VulkanGraphRuntime::UpdateVfxFrame(const NativeVfxFrameUpdate &update) {
        if (const auto access = impl_->Access(); access.HasError())
            return access;
        if (impl_->lifecycle.frame.IsValid() || update.deviceIncarnation != impl_->deviceIncarnation)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));

        struct UploadScope final {
            bool &active;

            explicit UploadScope(bool &value) noexcept : active(value) {}

            UploadScope(const UploadScope &) = delete;
            UploadScope &operator=(const UploadScope &) = delete;

            ~UploadScope() {
                active = false;
            }
        };

        impl_->lifecycle.uploadingVfx = true;
        const UploadScope scope{impl_->lifecycle.uploadingVfx};
        const auto completion = impl_->resources->BufferCompletion(update.instance);
        if (completion.HasError())
            return Result<void>::Failure(completion.ErrorValue());
        if (completion.Value().IsValid()) {
            const auto retired =
                impl_->commands.Observe({.completion = completion.Value(), .expectedDeviceIncarnation = update.deviceIncarnation});
            if (retired.HasError())
                return Result<void>::Failure(retired.ErrorValue());
            if (!retired.Value())
                return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
        }
        if (impl_->lifecycle.closing)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        return impl_->resources->UpdateVfxFrame(update, impl_->lifecycle.closing);
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
        impl_->vfxGpu.Close();
        impl_->Abort();
        return impl_->commands.BeginShutdown();
    }

    /** @copydoc VulkanGraphRuntime::PollDrain */
    Result<RenderBackendDrainProgress> VulkanGraphRuntime::Impl::DrainOwners() {
        const auto particles = vfxGpu.Drain();
        if (particles.HasError())
            return Result<RenderBackendDrainProgress>::Failure(particles.ErrorValue());
        if (!particles.Value())
            return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Pending);
        return commands.PollDrain();
    }

    void VulkanGraphRuntime::Impl::FinishDrained() noexcept {
        for (const auto &pins : retention)
            if (pins.references != 0 || pins.lease)
                std::terminate();
        resources = nullptr;
        owner = {};
    }

    Result<RenderBackendDrainProgress> VulkanGraphRuntime::PollDrain() {
        using enum Horo::Render::RenderBackendDrainProgress;
        if (impl_->owner == std::thread::id{})
            return Result<RenderBackendDrainProgress>::Success(Drained);
        if (const auto access = impl_->Access(false); access.HasError())
            return Result<RenderBackendDrainProgress>::Failure(access.ErrorValue());
        if (!impl_->lifecycle.closing)
            return Result<RenderBackendDrainProgress>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        if (impl_->lifecycle.uploadingVfx)
            return Result<RenderBackendDrainProgress>::Success(Pending);
        auto drained = impl_->DrainOwners();
        if (drained.HasValue() && drained.Value() == Drained)
            impl_->FinishDrained();
        return drained;
    }
}  // namespace Horo::Render::Detail
