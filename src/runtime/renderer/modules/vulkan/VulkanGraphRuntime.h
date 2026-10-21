#pragma once

/** @file VulkanGraphRuntime.h
 * @brief Device-private translation of canonical graph submissions and resource states.
 */
#include "VulkanResourceRuntime.h"

namespace Horo::Render::Detail {
    /** @brief Bounded render-owner graph recorder; native parents outlive its explicit drain. */
    class VulkanGraphRuntime final {
    public:
        VulkanGraphRuntime();
        ~VulkanGraphRuntime();
        VulkanGraphRuntime(const VulkanGraphRuntime &) = delete;
        VulkanGraphRuntime &operator=(const VulkanGraphRuntime &) = delete;
        /** @brief Prepares fixed graph scratch/retention storage and required Vulkan1.3 dispatch.
         * @param device Admitted native device.
         * @param queues Exact Horo queue topology with native family ownership.
         * @param frames Maximum simultaneously retained graph admissions.
         * @param timelineDifference Selected-device timeline semaphore value-difference limit.
         * @param deviceIncarnation Canonical non-reused selected-device identity.
         * @param resources Same-device resource identity registry; remains alive through drain.
         * @param resolver Same-device dispatch resolver.
         * @return Success or original typed rollback-safe failure. */
        [[nodiscard]] Result<void> Initialize(VkDevice device, std::span<const VulkanCommandQueue> queues, std::uint32_t frames,
                                              std::uint64_t timelineDifference, std::uint64_t deviceIncarnation,
                                              VulkanResourceRuntime &resources,
                                              const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver);
        [[nodiscard]] Result<void> BeginFrame(FrameToken frame);
        [[nodiscard]] Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request);
        /** @brief Records one bounded reflected material pass using this runtime's existing submission owner.
         * @param request Exact frame, offscreen target, immutable draws and explicit frontend submission/lease.
         * @return Success with synchronous lease transfer or typed validation/native recording failure. */
        [[nodiscard]] Result<void> ExecuteMaterialPass(const RenderMaterialExecutionRequest &request);
        /** @brief Submits exact copied Horo records; partial success retains accepted work and closes admission on failure. */
        [[nodiscard]] Result<void> SubmitFrame(FrameToken frame);
        void AbortFrame(FrameToken frame) noexcept;
        [[nodiscard]] Result<bool> ObserveSubmission(const RenderCpuWait &wait);
        [[nodiscard]] Result<void> BeginShutdown();
        [[nodiscard]] Result<RenderBackendDrainProgress> PollDrain();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Render::Detail
