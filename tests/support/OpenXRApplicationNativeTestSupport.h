#pragma once
/** @file OpenXRApplicationNativeTestSupport.h
 * @brief Reuses existing official scripts and the private graphics port for real-loader application lifecycle tests.
 */
#include "VulkanEditorGraphicsBridge.h"
#include "support/OpenXRApplicationNativeScriptTestSupport.h"

namespace Horo::Tests::OpenXRApplication {
    namespace Native = Horo::XR::OpenXRInternal;
    namespace RenderNative = Horo::Render::Detail;

    /** @brief Supplies explicit prepared native facts only; unsupported recording methods fail rather than simulate completion. */
    class PreparedGraphicsPort : public RenderNative::IVulkanEditorGraphicsPort, public Render::IVulkanLoaderPort {
    public:
        std::array<std::string, 1> instanceExtensions{"VK_KHR_get_physical_device_properties2"};
        std::array<std::string, 1> deviceExtensions{"VK_KHR_external_memory"};
        bool available{true};

        Result<RenderNative::VulkanEditorDevice> QueryDevice() const override {
            if (!available)
                return Result<RenderNative::VulkanEditorDevice>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
            return Result<RenderNative::VulkanEditorDevice>::Success({.instance = OpenXR::NativeHandle<VkInstance>(2),
                                                                      .physical = OpenXR::NativeHandle<VkPhysicalDevice>(7),
                                                                      .device = OpenXR::NativeHandle<VkDevice>(3),
                                                                      .graphicsQueue = OpenXR::NativeHandle<VkQueue>(4),
                                                                      .graphicsFamily = 0,
                                                                      .frames = 2,
                                                                      .outputFormat = VK_FORMAT_R8G8B8A8_UNORM,
                                                                      .enabled = {{1, 3, 0}, instanceExtensions, deviceExtensions}});
        }

        Result<RenderNative::VulkanGuiSampleBinding> BorrowSample(const Render::GuiSampledResourceInstance &) const override {
            return Result<RenderNative::VulkanGuiSampleBinding>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        }

        Result<RenderNative::VulkanFrameRetentionSlot> FrameSlot(Render::FrameToken) const override {
            return Result<RenderNative::VulkanFrameRetentionSlot>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        }

        Result<void> PreparePrimaryOutput(Render::FrameToken, Render::PrimaryOutputAttachment) override {
            return Unavailable();
        }

        Result<void> ExecuteGui(const Render::GuiCompositionRequest &, RenderNative::IVulkanGuiEncoder &) override {
            return Unavailable();
        }

        Result<void> WriteFrameBuffer(Render::FrameToken, std::uint64_t, std::span<const std::byte>) override {
            return Unavailable();
        }

        Result<void> AttachDeviceChild(RenderNative::IVulkanEditorDeviceChild &) override {
            return Result<void>::Success();
        }

        void DetachDeviceChild(RenderNative::IVulkanEditorDeviceChild &) noexcept override {}

        Result<void> AcquireLoader() override {
            return Unavailable();
        }

        PFN_vkGetInstanceProcAddr GetInstanceProcAddr() noexcept override {
            return nullptr;
        }

        Result<std::vector<std::string>> RequiredPresentationExtensions() override {
            return Result<std::vector<std::string>>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        }

        Result<bool> SupportsPresentation(VkPhysicalDevice, std::uint32_t) override {
            return Result<bool>::Success(false);
        }

        Result<bool> IsDriverAllowed(const VkPhysicalDeviceProperties &) override {
            return Result<bool>::Success(false);
        }

        void ReleaseLoader() noexcept override {}

    private:
        static Result<void> Unavailable() {
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        }
    };
}  // namespace Horo::Tests::OpenXRApplication
