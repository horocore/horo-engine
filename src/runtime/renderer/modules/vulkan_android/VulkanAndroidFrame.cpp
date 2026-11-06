#include "VulkanAndroidRuntime.h"

#include <algorithm>
#include <limits>

namespace Horo::Render::AndroidVulkan {
    /** @brief Nonblocking exact-fence observation; no accepted submission is treated as completion. */
    Result<bool> Runtime::Fence(VkFence fence) const {
        const auto result = vk_.fenceStatus(device_, fence);
        if (result == VK_NOT_READY)
            return Result<bool>::Success(false);
        if (result != VK_SUCCESS)
            return Result<bool>::Failure(NativeError("vkGetFenceStatus", result));
        return Result<bool>::Success(true);
    }

    /** @brief Returns an acquired unused image only after its acquisition and any submitted rendering have completed. */
    Result<void> Runtime::ReleaseImage(Slot &slot) {
        const VkReleaseSwapchainImagesInfoEXT info{.sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT,
                                                   .swapchain = surface_->swapchain,
                                                   .imageIndexCount = 1,
                                                   .pImageIndices = &slot.image};
        const auto result = vk_.releaseImages(device_, &info);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkReleaseSwapchainImagesEXT", result));
        slot.acquired = false;
        slot.returning = false;
        slot.frame = {};
        return Result<void>::Success();
    }

    /** @brief Advances one exact acquisition/render slot using independent actual fence proofs. */
    Result<bool> Runtime::PollSlot(Slot &slot) {
        if (slot.acquiring) {
            const auto ready = Fence(slot.acquireFence);
            if (ready.HasError() || !ready.Value())
                return ready;
            CompleteAcquisition(slot);
        }
        if (slot.submitted) {
            const auto ready = Fence(slot.commandFence);
            if (ready.HasError() || !ready.Value())
                return ready;
            slot.submitted = false;
        }
        if (slot.acquired && slot.returning) {
            const auto released = ReleaseImage(slot);
            if (released.HasError())
                return Result<bool>::Failure(released.ErrorValue());
        }
        return Result<bool>::Success(true);
    }

    /** @brief Polls every bounded slot and every independently outstanding present fence without early pending starvation. */
    Result<bool> Runtime::PollSlots() {
        bool complete = true;
        for (auto &slot : std::span{slots_}.first(slotCount_)) {
            const auto ready = PollSlot(slot);
            if (ready.HasError())
                return ready;
            complete = ready.Value() && complete;
        }
        if (!surface_)
            return Result<bool>::Success(complete);
        for (auto &image : std::span{surface_->images}.first(surface_->imageCount)) {
            if (!image.presentPending)
                continue;
            const auto ready = Fence(image.presentFence);
            if (ready.HasError())
                return ready;
            if (ready.Value())
                image.presentPending = false;
            complete = ready.Value() && complete;
        }
        return Result<bool>::Success(complete);
    }

    /** @brief Keeps the entire old window/surface tuple until all acquire, command and presentation debt retires. */
    Result<bool> Runtime::RetireSurface() {
        if (!surface_)
            return Result<bool>::Success(true);
        for (auto &slot : std::span{slots_}.first(slotCount_))
            slot.returning = slot.acquired || slot.acquiring;
        const auto complete = PollSlots();
        if (complete.HasError() || !complete.Value())
            return complete;
        DestroySurface();
        return Result<bool>::Success(true);
    }

    /** @brief Drives surface loss/replacement at a safe point before admitting a new frame. */
    Result<bool> Runtime::Availability(FramebufferExtent extent) {
        const auto access = Access();
        if (access.HasError())
            return Result<bool>::Failure(access.ErrorValue());
        if (active_.IsValid())
            return Result<bool>::Failure(MakeError(Errors::Invalid));
        const auto surface = EnsureSurface();
        if (surface.HasError() || !surface.Value())
            return surface;
        if (extent != surface_->extent)
            return Result<bool>::Success(false);
        const auto ready = PollSlots();
        if (ready.HasError())
            return ready;
        // This is native-output presence, not frame credit or completion. Begin owns bounded acquisition/backpressure.
        return Result<bool>::Success(true);
    }

    /** @brief Observes logical invalidation without treating retained native handles as current authority. */
    bool Runtime::NeedsRetirement() const noexcept {
        return surface_ && (surface_->retiring || !surface_->window.IsCurrent());
    }

    /** @brief Publishes loss once before old-generation debt is retired. */
    Result<void> Runtime::LoseSurface() {
        surface_->retiring = true;
        const auto snapshot = lifecycle_->Snapshot();
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        if (snapshot.Value().state == RenderSurfaceState::Lost)
            return Result<void>::Success();
        return PublishSurface(RenderSurfaceCommandKind::Lose, RenderSurfaceRealization::Lost);
    }

    /** @brief Retires an invalid old generation before importing the next exact platform publication. */
    Result<bool> Runtime::EnsureSurface() {
        if (NeedsRetirement()) {
            const auto lost = LoseSurface();
            if (lost.HasError())
                return Result<bool>::Failure(lost.ErrorValue());
            const auto retired = RetireSurface();
            if (retired.HasError() || !retired.Value())
                return retired;
            // The old output is now genuinely absent. Publish that boundary before a later poll imports a new native generation.
            return Result<bool>::Success(false);
        }
        if (surface_)
            return Result<bool>::Success(true);
        return ImportCurrentWindow();
    }

    /** @brief Acquires only the actual committed drawable generation supplied by the sole platform window owner. */
    Result<bool> Runtime::ImportCurrentWindow() {
        const auto window = windows_.Snapshot();
        if (window.HasError())
            return Result<bool>::Failure(window.ErrorValue());
        const auto &facts = window.Value();
        if (!facts.available || facts.extent.width == 0 || facts.extent.height == 0)
            return Result<bool>::Success(false);
        auto lease = windows_.Acquire(facts.window);
        if (lease.HasError())
            return Result<bool>::Failure(lease.ErrorValue());
        const auto created = CreateSurface(std::move(lease).Value());
        if (created.HasError())
            return Result<bool>::Failure(created.ErrorValue());
        return Result<bool>::Success(true);
    }

    /** @brief Handles expected WSI loss as unavailable while preserving genuine native failures. */
    Result<bool> Runtime::AcquireFailure(VkResult result) {
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
            const auto lost = LoseSurface();
            if (lost.HasError())
                return Result<bool>::Failure(lost.ErrorValue());
            return Result<bool>::Success(false);
        }
        return Result<bool>::Failure(NativeError("vkAcquireNextImageKHR", result));
    }

    /** @brief Starts timeout-zero acquisition with an actual unsignaled fence; no native callback or wait is hidden. */
    Result<bool> Runtime::AcquireImage() {
        auto free = std::ranges::find_if(std::span{slots_}.first(slotCount_), [](const Slot &slot) {
            return !slot.acquired && !slot.acquiring && !slot.submitted;
        });
        if (free == std::span{slots_}.first(slotCount_).end())
            return Result<bool>::Success(false);
        auto result = vk_.resetFences(device_, 1, &free->acquireFence);
        if (result != VK_SUCCESS)
            return Result<bool>::Failure(NativeError("vkResetFences(acquire)", result));
        result = vk_.acquire(device_, surface_->swapchain, 0, VK_NULL_HANDLE, free->acquireFence, &free->image);
        if (result == VK_NOT_READY || result == VK_TIMEOUT)
            return Result<bool>::Success(false);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return AcquireFailure(result);
        return PublishAcquisition(*free, result);
    }

    /** @brief Retains successful native acquisition before validating its image identity; invalid identities force whole-chain retirement.
     */
    Result<bool> Runtime::PublishAcquisition(Slot &slot, VkResult result) {
        slot.acquiring = true;
        if (slot.image >= surface_->imageCount) {
            surface_->retiring = true;
            slot.returning = true;
            return Result<bool>::Failure(
                MakeError(Errors::Invalid,
                          "Native acquisition returned an out-of-range image; its actual fence must retire before chain destruction."));
        }
        if (result == VK_SUBOPTIMAL_KHR)
            surface_->retiring = true;
        return ObserveAcquired(slot);
    }

    /** @brief Completes exact acquisition-fence debt; an invalid native image is never released through a fabricated valid index. */
    void Runtime::CompleteAcquisition(Slot &slot) noexcept {
        slot.acquiring = false;
        slot.acquired = slot.image < surface_->imageCount;
        slot.returning = slot.returning && slot.acquired;
    }

    /** @brief Requires both acquisition and previous presentation completion before reusing image-bound synchronization. */
    Result<bool> Runtime::ObserveAcquired(Slot &slot) {
        const auto ready = PollSlot(slot);
        if (ready.HasError() || !ready.Value())
            return ready;
        auto &image = surface_->images[slot.image];
        if (image.presentPending) {
            const auto presented = Fence(image.presentFence);
            if (presented.HasError())
                return presented;
            if (!presented.Value())
                return Result<bool>::Success(false);
            image.presentPending = false;
        }
        return Result<bool>::Success(true);
    }

    /** @brief Issues a frame token only for the exact currently retained drawable generation. */
    Result<FrameToken> Runtime::Begin(const FrameDescriptor &frame) {
        const auto available = Availability(frame.outputExtent);
        if (available.HasError())
            return Result<FrameToken>::Failure(available.ErrorValue());
        if (!available.Value())
            return Result<FrameToken>::Failure(MakeError(Errors::Pending));
        const auto prepared = PrepareAcquisition();
        if (prepared.HasError())
            return Result<FrameToken>::Failure(prepared.ErrorValue());
        if (!prepared.Value())
            return Result<FrameToken>::Failure(MakeError(Errors::Pending));
        if (nextFrame_ == std::numeric_limits<std::uint64_t>::max())
            return Result<FrameToken>::Failure(MakeError(Errors::Capacity));
        for (auto &slot : std::span{slots_}.first(slotCount_)) {
            if (!ReadySlot(slot))
                continue;
            active_ = {nextFrame_++};
            slot.frame = active_;
            encoded_ = false;
            return Result<FrameToken>::Success(active_);
        }
        return Result<FrameToken>::Failure(MakeError(Errors::Pending));
    }

    /** @brief Acquires only when the host actually begins a frame, never during paused readiness/retirement polling. */
    Result<bool> Runtime::PrepareAcquisition() {
        for (const auto &slot : std::span{slots_}.first(slotCount_))
            if (ReadySlot(slot))
                return Result<bool>::Success(true);
        return AcquireImage();
    }

    /** @brief Looks up only an exact currently active frame slot. */
    Slot *Runtime::Active(FrameToken frame) noexcept {
        if (!frame.IsValid() || frame != active_)
            return nullptr;
        const auto slot = std::ranges::find(std::span{slots_}.first(slotCount_), frame, &Slot::frame);
        return slot == std::span{slots_}.first(slotCount_).end() ? nullptr : &*slot;
    }

    /** @brief Prevents semaphore reuse until the exact previous presentation fence has completed. */
    bool Runtime::ReadySlot(const Slot &slot) const noexcept {
        return slot.acquired && !slot.returning && !slot.submitted && !surface_->images[slot.image].presentPending;
    }

    /** @brief Validates the complete closed clear plan before any native command recording. */
    Result<void> Runtime::Execute(const RenderExecutionPlan &plan) {
        const auto access = Access();
        if (access.HasError())
            return access;
        auto *slot = Active(plan.frame);
        if (!slot || encoded_)
            return Result<void>::Failure(MakeError(Errors::Invalid));
        if (!surface_->window.IsCurrent())
            return Result<void>::Failure(MakeError(Errors::Pending));
        if (plan.orderedPasses.size() != 1)
            return Result<void>::Failure(MakeError(Errors::Unsupported));
        const auto &pass = plan.orderedPasses.front();
        const auto valid = ValidatePass(pass);
        if (valid.HasError())
            return valid;
        const auto recorded = Record(*slot, *pass.primaryOutput);
        if (recorded.HasValue())
            encoded_ = true;
        return recorded;
    }

    /** @brief Records one bounded real Vulkan 1.1 render-pass clear. */
    Result<void> Runtime::Record(Slot &slot, const PrimaryOutputAttachment &attachment) {
        auto result = vk_.resetCommand(slot.command, 0);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkResetCommandBuffer", result));
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        result = vk_.beginCommand(slot.command, &begin);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkBeginCommandBuffer", result));
        const VkClearValue clear{
            .color = {{attachment.clearColor.red, attachment.clearColor.green, attachment.clearColor.blue, attachment.clearColor.alpha}}};
        const VkRenderPassBeginInfo pass{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                         .renderPass = surface_->renderPass,
                                         .framebuffer = surface_->images[slot.image].framebuffer,
                                         .renderArea = {{0, 0}, {surface_->extent.width, surface_->extent.height}},
                                         .clearValueCount = 1,
                                         .pClearValues = &clear};
        vk_.beginPass(slot.command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vk_.endPass(slot.command);
        result = vk_.endCommand(slot.command);
        if (result != VK_SUCCESS)
            return Result<void>::Failure(NativeError("vkEndCommandBuffer", result));
        return Result<void>::Success();
    }

    /** @brief Stops unsent frame admission while preserving actual acquire and accepted native completion debt. */
    void Runtime::Abort(FrameToken frame) noexcept {
        if (owner_ != std::this_thread::get_id())
            return;
        auto *slot = Active(frame);
        if (!slot)
            return;
        slot->returning = true;
        slot->frame = {};
        active_ = {};
        encoded_ = false;
    }

    /** @brief Closes admission without waiting for any GPU or presentation work. */
    Result<void> Runtime::Close() {
        if (owner_ == std::thread::id{})
            owner_ = std::this_thread::get_id();
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(Errors::WrongThread));
        closing_ = true;
        Abort(active_);
        if (surface_)
            surface_->retiring = true;
        return Result<void>::Success();
    }

    /** @brief Destroys parents only after exact acquire/render/present fences prove all generation debt terminal. */
    Result<RenderBackendDrainProgress> Runtime::Drain() {
        if (owner_ != std::this_thread::get_id())
            return Result<RenderBackendDrainProgress>::Failure(MakeError(Errors::WrongThread));
        if (!closing_)
            return Result<RenderBackendDrainProgress>::Failure(MakeError(Errors::Invalid));
        const auto retired = RetireSurface();
        if (retired.HasError())
            return Result<RenderBackendDrainProgress>::Failure(retired.ErrorValue());
        if (!retired.Value())
            return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Pending);
        if (lifecycle_) {
            const auto snapshot = lifecycle_->Snapshot();
            if (snapshot.HasError())
                return Result<RenderBackendDrainProgress>::Failure(snapshot.ErrorValue());
            if (snapshot.Value().state != RenderSurfaceState::Unattached) {
                const auto closed = PublishSurface(RenderSurfaceCommandKind::Close, RenderSurfaceRealization::Unattached);
                if (closed.HasError())
                    return Result<RenderBackendDrainProgress>::Failure(closed.ErrorValue());
            }
        }
        Rollback();
        initialized_ = false;
        return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Drained);
    }
}  // namespace Horo::Render::AndroidVulkan
