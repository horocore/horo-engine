#pragma once
/** @file OpenXRApplicationNativeTestSupport.h
 * @brief Reuses existing official scripts and the private graphics port for real-loader application lifecycle tests.
 */
#include "VulkanEditorGraphicsBridge.h"
#include "support/OpenXRActionBindingTestSupport.h"
#include "support/OpenXRCensusTestSupport.h"
#include "support/OpenXRClockTestSupport.h"
#include "support/OpenXREventScriptTestSupport.h"
#include "support/OpenXRFrameScriptTestSupport.h"
#include "support/OpenXRSelectedVulkanTestSupport.h"
#include "support/OpenXRSwapchainTestSupport.h"

namespace Horo::Tests::OpenXRApplication {
    namespace Native = Horo::XR::OpenXRInternal;
    namespace RenderNative = Horo::Render::Detail;

    /** @brief Keeps the exact existing script authorities alive until all loaded application children unload. */
    struct ApplicationNativeScript final {
        static inline thread_local ApplicationNativeScript *active{};
        OpenXR::Script native;
        OpenXR::CensusScript census;
        OpenXR::GraphicsScript graphics;
        OpenXRAction::ActionScript actions;
        OpenXR::ClockScript clock;
        OpenXR::EventScript events;
        OpenXR::FrameScript frames;
        OpenXR::SwapchainScript swapchain;

        ApplicationNativeScript() {
            REQUIRE(active == nullptr);
            native.legacyVulkan = true;
            native.clockExtension = true;
            native.extensionCount = 2;
            events.active = &events;
            active = this;
        }

        ApplicationNativeScript(const ApplicationNativeScript &) = delete;
        ApplicationNativeScript &operator=(const ApplicationNativeScript &) = delete;

        ~ApplicationNativeScript() {
            CHECK(native.liveInstances == 0);
            CHECK(native.liveSessions == 0);
            CHECK(actions.liveSets == 0);
            CHECK(actions.liveActions == 0);
            CHECK(actions.tracking.spaces == 0);
            events.active = nullptr;
            active = nullptr;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Resolve(XrInstance instance, const char *name, PFN_xrVoidFunction *output) {
            *output = nullptr;
            if (!active)
                return XR_ERROR_INITIALIZATION_FAILED;
            if (active->native.absent == name || (instance != XR_NULL_HANDLE && active->native.instanceDispatchAbsent == name))
                return XR_ERROR_FUNCTION_UNSUPPORTED;
            const std::array<PFN_xrGetInstanceProcAddr, 7> resolvers{&OpenXR::GraphicsScript::GetProc,    &OpenXR::CensusScript::GetProc,
                                                                     &OpenXR::EventScript::GetProc,       &OpenXR::FrameScript::GetProc,
                                                                     &OpenXR::ClockScript::Resolve,       &OpenXR::SwapchainScript::GetProc,
                                                                     &OpenXRAction::ActionScript::GetProc};
            for (const auto resolver : resolvers) {
                const auto result = resolver(instance, name, output);
                if (result == XR_SUCCESS && *output)
                    return result;
            }
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
    };

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
