#pragma once

/** @file VulkanUiPaintKernel.h @brief Private selected-device pipeline owner for canonical Runtime UI paint. */
#include "Horo/Runtime/Render/UiPaintKernel.h"
#include "Horo/Runtime/Render/UiPaintWorkload.h"
#include "VulkanShaderRuntime.h"

#include <array>

namespace Horo::Render::Detail {
    /** @brief Actual selected-device evidence for the exact native attachment formats of one cook. */
    struct VulkanUiPaintDevice final {
        VkDevice device{};
        std::uint64_t incarnation{};
        VkPhysicalDeviceLimits limits{};
        VkFormatFeatureFlags colorFeatures{};
        VkFormatFeatureFlags stencilFeatures{};
    };

    /** @brief Cold Vulkan pipeline/layout owner shared by exact paint passes and their submitted completion leases.
     * @details Descriptor allocation and frame recording belong to the graph runtime. The device owner revokes outstanding
     * kernel objects only after native command drain. Recording uses a negative-height viewport to preserve the canonical
     * HLSL top-left projection and keeps scissors in native top-left pixels. */
    class VulkanUiPaintKernel final : public IResidentUiPaintKernel {
    public:
        /** @brief Realizes the actual Vulkan artifact and all six closed paint variants.
         * @param selected Selected logical device and queried native capabilities.
         * @param resolve Owning dispatch resolver, borrowed synchronously.
         * @param cooked Exact package, target attachment formats and same-operation reflection.
         * @return Whole kernel or original typed validation/creation failure.
         * @pre Render owner lane outside recording. */
        [[nodiscard]] static Result<std::shared_ptr<VulkanUiPaintKernel>> Create(const VulkanUiPaintDevice &selected,
                                                                                 const VulkanShaderRuntime::Resolver &resolve,
                                                                                 const CookedUiPaintKernel &cooked);
        ~VulkanUiPaintKernel() override;
        /** @copydoc IResidentUiPaintKernel::RequiresGraphNeutralSample */
        [[nodiscard]] bool RequiresGraphNeutralSample() const noexcept override {
            return true;
        }
        VulkanUiPaintKernel(const VulkanUiPaintKernel &) = delete;
        VulkanUiPaintKernel &operator=(const VulkanUiPaintKernel &) = delete;

        /** @brief Returns the exact pipeline, or null for a revoked kernel or incompatible operation/stencil pair. */
        [[nodiscard]] VkPipeline Pipeline(UiPaintOperation operation, bool stencilAttached) const noexcept;

        /** @brief Returns the checked dynamic uniform, paint, coverage and sampler binding numbers in canonical role order. */
        [[nodiscard]] const std::array<std::uint32_t, 4> &Bindings() const noexcept {
            return bindings_;
        }

        /** @brief Borrows the exact selected set layout while this owner is live. */
        [[nodiscard]] VkDescriptorSetLayout SetLayout() const noexcept {
            return setLayout_;
        }

        /** @brief Borrows the Vulkan graphics pipeline layout shared by the fixed variants. */
        [[nodiscard]] VkPipelineLayout Layout() const noexcept {
            return layout_;
        }

        /** @brief Checks both the actual device and its private non-reused incarnation. */
        [[nodiscard]] bool Matches(VkDevice currentDevice, std::uint64_t currentIncarnation) const noexcept;

        /** @brief Returns the authorized native output color format. */
        [[nodiscard]] VkFormat ColorFormat() const noexcept {
            return color_;
        }

        /** @brief Returns the authorized native stencil attachment format. */
        [[nodiscard]] VkFormat StencilFormat() const noexcept {
            return stencil_;
        }

        /** @brief Releases exact native objects once after actual GPU drain; performs no wait. */
        void Revoke() noexcept;

    private:
        enum class Variant : std::uint8_t {
            Color,
            StenciledColor,
            Push,
            Pop,
            Multiply,
            StenciledMultiply,
            Count
        };

        struct Dispatch final {
            PFN_vkCreateShaderModule createModule{};
            PFN_vkDestroyShaderModule destroyModule{};
            PFN_vkCreateDescriptorSetLayout createSetLayout{};
            PFN_vkDestroyDescriptorSetLayout destroySetLayout{};
            PFN_vkCreatePipelineLayout createLayout{};
            PFN_vkDestroyPipelineLayout destroyLayout{};
            PFN_vkCreateGraphicsPipelines createPipelines{};
            PFN_vkDestroyPipeline destroyPipeline{};
        };

        VulkanUiPaintKernel(const VulkanUiPaintDevice &selected, Dispatch native, std::array<std::uint32_t, 4> bindings, VkFormat color,
                            VkFormat stencil) noexcept;
        [[nodiscard]] Result<void> PrepareLayouts();
        [[nodiscard]] Result<void> PreparePipelines(const CompiledShaderArtifact &artifact);

        VkDevice device_{};
        std::uint64_t incarnation_{};
        Dispatch native_;
        std::array<std::uint32_t, 4> bindings_;
        VkFormat color_{}, stencil_{};
        VkDescriptorSetLayout setLayout_{};
        VkPipelineLayout layout_{};
        std::array<VkPipeline, static_cast<std::size_t>(Variant::Count)> pipelines_{};
    };
}  // namespace Horo::Render::Detail
