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
        if (admission && (closing || failedSubmission))
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        return Result<void>::Success();
    }

    const VulkanCommandQueue *VulkanGraphRuntime::Impl::Queue(const RenderQueueId id) const noexcept {
        const auto found = std::ranges::find(queues, id, &VulkanCommandQueue::identity);
        return found == queues.end() ? nullptr : std::to_address(found);
    }

    VulkanGraphRuntime::VulkanGraphRuntime() : impl_(std::make_unique<Impl>()) {}

    VulkanGraphRuntime::~VulkanGraphRuntime() {
        if (impl_->owner != std::thread::id{}) {
            auto closing = BeginShutdown();
            auto drained = PollDrain();
            if (closing.HasError() || drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
                std::terminate();
        }
    }

    /** @brief Loads optional material command dispatch without changing graph-only admission. */
    void VulkanGraphRuntime::Impl::LoadMaterialDispatch(VkDevice device, const VulkanShaderRuntime::Resolver &resolver) {
        native.bindPipeline = std::bit_cast<PFN_vkCmdBindPipeline>(resolver(device, "vkCmdBindPipeline"));
        native.bindSets = std::bit_cast<PFN_vkCmdBindDescriptorSets>(resolver(device, "vkCmdBindDescriptorSets"));
        native.bindVertices = std::bit_cast<PFN_vkCmdBindVertexBuffers>(resolver(device, "vkCmdBindVertexBuffers"));
        native.bindIndex = std::bit_cast<PFN_vkCmdBindIndexBuffer>(resolver(device, "vkCmdBindIndexBuffer"));
        native.viewport = std::bit_cast<PFN_vkCmdSetViewport>(resolver(device, "vkCmdSetViewport"));
        native.scissor = std::bit_cast<PFN_vkCmdSetScissor>(resolver(device, "vkCmdSetScissor"));
        native.drawIndexed = std::bit_cast<PFN_vkCmdDrawIndexed>(resolver(device, "vkCmdDrawIndexed"));
    }

    /** @copydoc VulkanGraphRuntime::Initialize */
    Result<void> VulkanGraphRuntime::Initialize(const VkDevice device, const std::span<const VulkanCommandQueue> queues,
                                                const std::uint32_t frames, const std::uint64_t timelineDifference,
                                                const std::uint64_t deviceIncarnation, VulkanResourceRuntime &resources,
                                                const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver,
                                                const bool vertexLayerEnabled) {
        if (impl_->owner != std::thread::id{} || frames == 0 || frames > 8 || !resolver)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidConfig));
        for (std::size_t index = 0; index < queues.size(); ++index)
            for (std::size_t previous = 0; previous < index; ++previous)
                if (queues[index].family == queues[previous].family)
                    return Result<void>::Failure(
                        Impl::Unsupported("Distinct Horo queues require distinct native families in this whole-resource ownership path."));
        Impl::Dispatch dispatch{.barrier = std::bit_cast<PFN_vkCmdPipelineBarrier2>(resolver(device, "vkCmdPipelineBarrier2")),
                                .copy = std::bit_cast<PFN_vkCmdCopyBuffer2>(resolver(device, "vkCmdCopyBuffer2")),
                                .beginRendering = std::bit_cast<PFN_vkCmdBeginRendering>(resolver(device, "vkCmdBeginRendering")),
                                .endRendering = std::bit_cast<PFN_vkCmdEndRendering>(resolver(device, "vkCmdEndRendering"))};
        if (!dispatch.barrier || !dispatch.copy || !dispatch.beginRendering || !dispatch.endRendering)
            return Result<void>::Failure(
                MakeError(VulkanBackendErrors::EntryPointMissing, "Graph execution requires Vulkan1.3 core command dispatch."));
        try {
            impl_->queues.assign(queues.begin(), queues.end());
            impl_->bindings.resize(RenderGraphLimits::HardMaxResources);
            impl_->states.resize(RenderGraphLimits::HardMaxResources);
            impl_->identities.reserve(RenderGraphLimits::HardMaxResources);
            impl_->used.resize(RenderGraphLimits::HardMaxResources);
            impl_->discarded.resize(RenderGraphLimits::HardMaxResources);
            impl_->passes.resize(RenderGraphLimits::HardMaxPasses);
            impl_->barriers.reserve(RenderGraphLimits::HardMaxUsages * 3);
            impl_->updates.reserve(RenderGraphLimits::HardMaxUsages);
            impl_->submissions.reserve(RenderGraphSubmissionLimits::MaximumSubmissions);
            impl_->retention.resize(frames);
            for (auto &pins : impl_->retention)
                pins.resources.reserve(RenderGraphLimits::HardMaxResources + MultiviewExtractionLimits::HardMaximumInstances * 3);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(
                MakeError(VulkanBackendErrors::ResourceCreationFailed, "Bounded graph storage allocation failed."));
        }
        auto initialized = impl_->commands.Initialize(device, queues, RenderGraphSubmissionLimits::MaximumSubmissions, timelineDifference,
                                                      deviceIncarnation, resolver);
        if (initialized.HasError())
            return initialized;
        impl_->native = dispatch;
        impl_->LoadMaterialDispatch(device, resolver);
        impl_->multiview.Initialize(device, deviceIncarnation, vertexLayerEnabled, resolver);
        impl_->resources = &resources;
        impl_->deviceIncarnation = deviceIncarnation;
        impl_->owner = std::this_thread::get_id();
        impl_->closing = false;
        impl_->failedSubmission = false;
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::BeginFrame */
    Result<void> VulkanGraphRuntime::BeginFrame(const FrameToken frame) {
        if (auto access = impl_->Access(); access.HasError())
            return access;
        if (!frame.IsValid() || impl_->frame.IsValid())
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        if (auto polled = impl_->commands.Poll(); polled.HasError())
            return polled;
        impl_->frame = frame;
        impl_->multiview.Reset();
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::ExecuteGraph */
    Result<void> VulkanGraphRuntime::ExecuteGraph(const RenderGraphExecutionRequest &request) {
        if (auto access = impl_->Access(); access.HasError())
            return access;
        if (request.frame != impl_->frame || !impl_->frame.IsValid() || impl_->recorded)
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
        auto free = std::ranges::find_if(impl_->retention, [](const VulkanCommandRetention &pins) {
            return pins.references == 0;
        });
        if (free == impl_->retention.end())
            return Result<void>::Failure(MakeError(VulkanBackendErrors::SubmissionBusy));
        auto captured = impl_->resources->CaptureGraphResources(request, impl_->bindings, *free);
        if (captured.HasError()) {
            free->resources.clear();
            return captured;
        }
        auto prepared = impl_->Prepare(request, *free);
        if (prepared.HasError()) {
            free->resources.clear();
            return prepared;
        }
        auto recorded = impl_->Record(request, *free);
        if (recorded.HasError()) {
            impl_->Abort();
            free->resources.clear();
            return recorded;
        }
        impl_->recorded = true;
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::SubmitFrame */
    Result<void> VulkanGraphRuntime::SubmitFrame(const FrameToken frame) {
        if (auto access = impl_->Access(); access.HasError())
            return access;
        if (!frame.IsValid() || frame != impl_->frame)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        for (auto &submission : impl_->submissions) {
            auto accepted = impl_->commands.Submit(submission.ticket, submission.value);
            if (accepted.HasError()) {
                impl_->failedSubmission = true;
                impl_->Abort();
                return accepted;
            }
            for (const auto &update : std::span{impl_->updates}.subspan(submission.updates.offset, submission.updates.count))
                *update.target = update.value;
        }
        impl_->submissions.clear();
        impl_->frame = {};
        impl_->recorded = false;
        return Result<void>::Success();
    }

    void VulkanGraphRuntime::Impl::Abort() noexcept {
        multiview.Reset();
        for (const auto &submission : submissions)
            commands.Abort(submission.ticket);
        submissions.clear();
        recorded = false;
        frame = {};
    }

    /** @copydoc VulkanGraphRuntime::AbortFrame */
    void VulkanGraphRuntime::AbortFrame(const FrameToken frame) noexcept {
        if (impl_->owner == std::this_thread::get_id() && frame == impl_->frame)
            impl_->Abort();
    }

    /** @copydoc VulkanGraphRuntime::ObserveSubmission */
    Result<bool> VulkanGraphRuntime::ObserveSubmission(const RenderCpuWait &wait) {
        if (auto access = impl_->Access(false); access.HasError())
            return Result<bool>::Failure(std::move(access).ErrorValue());
        if (impl_->frame.IsValid() && wait.purpose != RenderCpuWaitPurpose::Poll)
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
        impl_->closing = true;
        impl_->Abort();
        return impl_->commands.BeginShutdown();
    }

    /** @copydoc VulkanGraphRuntime::PollDrain */
    Result<RenderBackendDrainProgress> VulkanGraphRuntime::PollDrain() {
        if (impl_->owner == std::thread::id{})
            return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Drained);
        if (auto access = impl_->Access(false); access.HasError())
            return Result<RenderBackendDrainProgress>::Failure(std::move(access).ErrorValue());
        if (!impl_->closing)
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
