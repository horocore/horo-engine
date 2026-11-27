#pragma once

/** @file VulkanUiPaintCommands.h @brief Exact descriptor preparation and native recording of admitted UI paint passes. */
#include "VulkanResourceRuntime.h"
#include "VulkanUiPaintKernel.h"

#include <optional>
#include <vector>

namespace Horo::Render::Detail {
    /** @brief Read-only graph transaction state captured before any native render pass starts. */
    struct VulkanUiPaintResources final {
        const VulkanResourceRuntime &owner;
        std::span<const RenderGraphResourceInstance> identities;
        std::span<const VulkanGraphResource> resources;
        std::span<const VulkanGraphResourceState> states;
    };

    /** @brief Bounded command data borrowed until synchronous recording; the canonical frame lease retains its owners. */
    struct VulkanPreparedUiPaint final {
        VkBuffer vertices{};
        VkBuffer indices{};
        VkImageView color{};
        VkImageView stencil{};
        std::span<const VkDescriptorSet> sets;
    };

    /** @brief Selected-device native UI kernel, fixed sampler bank and cold per-retention-slot descriptor storage. */
    class VulkanUiPaintCommands final : public IResidentUiPaintKernel {
    public:
        static constexpr auto FormatCount = static_cast<std::size_t>(RenderTextureFormat::Depth32FloatStencil8) + 1;
        using FormatFeatures = std::array<VkFormatFeatureFlags, FormatCount>;

        /** @brief Realizes one complete offscreen kernel outside an active frame.
         * @param device Actual selected device and format/alignment capabilities.
         * @param resolve Borrowed selected device dispatch.
         * @param cooked Exact verified native program and reflection.
         * @param formats Actual native sample feature table for the device.
         * @param frames Configured native graph retention slot count.
         * @return Fully initialized owner or typed refusal and complete rollback. */
        [[nodiscard]] static Result<std::shared_ptr<VulkanUiPaintCommands>> Create(const VulkanUiPaintDevice &device,
                                                                                   const VulkanShaderRuntime::Resolver &resolve,
                                                                                   const CookedUiPaintKernel &cooked,
                                                                                   const FormatFeatures &formats, std::size_t frames);
        ~VulkanUiPaintCommands() override;
        VulkanUiPaintCommands(const VulkanUiPaintCommands &) = delete;
        VulkanUiPaintCommands &operator=(const VulkanUiPaintCommands &) = delete;

        /** @copydoc IResidentUiPaintKernel::RequiresGraphNeutralSample */
        [[nodiscard]] bool RequiresGraphNeutralSample() const noexcept override {
            return true;
        }

        /** @brief Checks that the actual native owner and incarnation still match. */
        [[nodiscard]] bool Matches(VkDevice device, std::uint64_t incarnation) const noexcept;

        /** @brief Admits exact attachments, buffered publication and sampled resources before populating one proven-free set range.
         * @param paint Exact already validated portable paint program.
         * @param bindings Graph's retained resource owners and current contents journal.
         * @param frame Free native graph retention slot.
         * @param firstDraw Exclusive descriptor range offset reserved by this exact graph.
         * @return Bounded recording packet or typed refusal before any native command. */
        [[nodiscard]] Result<VulkanPreparedUiPaint> Prepare(const RenderGraphUiPaint &paint, const VulkanUiPaintResources &bindings,
                                                            std::size_t frame, std::size_t firstDraw) const;

        /** @brief Records one already admitted offscreen paint operation synchronously with no new allocation. */
        void Encode(VkCommandBuffer command, const RenderGraphUiPaint &paint, const VulkanPreparedUiPaint &prepared) const noexcept;

        /** @brief Revokes all native objects only after canonical command drain. */
        void Revoke() noexcept;

    private:
        struct Dispatch final {
            PFN_vkCreateDescriptorPool createPool{};
            PFN_vkDestroyDescriptorPool destroyPool{};
            PFN_vkAllocateDescriptorSets allocateSets{};
            PFN_vkUpdateDescriptorSets updateSets{};
            PFN_vkCreateSampler createSampler{};
            PFN_vkDestroySampler destroySampler{};
            PFN_vkCmdBeginRendering begin{};
            PFN_vkCmdEndRendering end{};
            PFN_vkCmdBindPipeline bindPipeline{};
            PFN_vkCmdBindDescriptorSets bindSet{};
            PFN_vkCmdBindVertexBuffers bindVertices{};
            PFN_vkCmdBindIndexBuffer bindIndices{};
            PFN_vkCmdSetViewport viewport{};
            PFN_vkCmdSetScissor scissor{};
            PFN_vkCmdSetStencilReference reference{};
            PFN_vkCmdDrawIndexed draw{};
        };

        VulkanUiPaintCommands(const VulkanUiPaintDevice &device, Dispatch native, FormatFeatures formats,
                              std::shared_ptr<VulkanUiPaintKernel> kernel) noexcept;
        [[nodiscard]] static std::optional<Dispatch> LoadDispatch(VkDevice device, const VulkanShaderRuntime::Resolver &resolve);
        [[nodiscard]] Result<void> PrepareDescriptors(std::size_t frames);
        [[nodiscard]] Result<void> PrepareSamplers();
        [[nodiscard]] bool CanFilter(const VulkanGraphResource &texture, const UiPaintDraw &draw) const noexcept;
        void BindDraw(VkDescriptorSet set, VkBuffer uniforms, VkImageView source, VkImageView coverage, VkSampler sampler) const noexcept;
        void EncodeDraw(VkCommandBuffer command, const UiPaintDraw &draw, VkDescriptorSet set, std::uint32_t parameter,
                        bool hasStencil) const noexcept;

        VkDevice device_{};
        FramebufferExtent maximumExtent_;
        Dispatch native_;
        FormatFeatures formats_;
        std::shared_ptr<VulkanUiPaintKernel> kernel_;
        VkDescriptorPool pool_{};
        std::vector<VkDescriptorSet> sets_;
        std::array<VkSampler, 12> samplers_{};
    };
}  // namespace Horo::Render::Detail
