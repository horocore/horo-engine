#pragma once
/** @file OpenXRApplicationVulkanTestSupport.h
 * @brief Composes existing scripted dispatch with the real Vulkan resource, shader, and graph owners.
 */
#include "../renderer/VulkanShaderDispatchFixture.h"
#include "VulkanExternalGraphBackendTestSupport.h"
#include "support/OpenXRApplicationNativeTestSupport.h"
#include "support/ShaderReflectionCookTestSupport.h"

#include <utility>

namespace Horo::Tests::OpenXRApplication {
    /** @brief Keeps native shader callbacks alive until the real resource owner has retired all programs. */
    struct ShaderDriverScope final {
        Driver state;

        ShaderDriverScope() {
            REQUIRE(driver == nullptr);
            driver = &state;
            state.specializationCount = 0;
            state.admittedAttributeCounts = {1, 3};
            state.admittedUniformRanges = {1296, 2624};
        }

        ShaderDriverScope(const ShaderDriverScope &) = delete;
        ShaderDriverScope &operator=(const ShaderDriverScope &) = delete;

        ~ShaderDriverScope() {
            driver = nullptr;
        }
    };

    /** @brief Forwards frame-slot and uniform writes to the same graph/resource owner; WSI remains unavailable. */
    class GraphGraphicsPort final : public PreparedGraphicsPort {
    public:
        std::size_t multiviewUniformWrites{};

        explicit GraphGraphicsPort(Render::VulkanGraphTest::NativeGraphFixture &native) : native_(native) {}

        Result<RenderNative::VulkanEditorDevice> QueryDevice() const override {
            auto device = PreparedGraphicsPort::QueryDevice();
            if (device.HasError())
                return device;
            auto value = std::move(device).Value();
            value.device = OpenXR::NativeHandle<VkDevice>(1);
            value.graphicsQueue = native_.commandState.queues[0].native;
            value.graphicsFamily = native_.commandState.queues[0].family;
            value.deviceResolver = &Render::VulkanGraphTest::GraphResolver;
            value.graphicsIdentity = native_.commandState.queues[0].identity;
            return Result<RenderNative::VulkanEditorDevice>::Success(std::move(value));
        }

        Result<RenderNative::VulkanFrameRetentionSlot> FrameSlot(const Render::FrameToken frame) const override {
            return native_.graph.FrameSlot(frame);
        }

        Result<void> WriteFrameBuffer(const Render::FrameToken frame, const std::uint64_t instance,
                                      const std::span<const std::byte> bytes) override {
            if (!native_.graph.CanWriteFrameInputs(frame))
                return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
            REQUIRE((bytes.size() == 1296 || bytes.size() == 2624));
            if (bytes.size() == 2624) {
                CHECK(std::ranges::all_of(bytes.subspan(2612), [](const std::byte value) {
                    return value == std::byte{};
                }));
                ++multiviewUniformWrites;
            }
            return native_.resources.WriteHostFrameBuffer(instance, bytes);
        }

        Result<void> PreparePrimaryOutput(const Render::FrameToken frame, const Render::PrimaryOutputAttachment output) override {
            if (!output.IsValid() || native_.graph.FrameSlot(frame).HasError())
                return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
            outputFrame_ = frame;
            output_.clearColor = output.clearColor;
            return Result<void>::Success();
        }

        Result<void> ExecuteGui(const Render::GuiCompositionRequest &request, RenderNative::IVulkanGuiEncoder &encoder) override {
            if (request.frame != outputFrame_)
                return Result<void>::Failure(MakeError(XR::XRErrors::IdentityStale));
            return native_.graph.ExecuteGui(request, output_, encoder);
        }

        Result<void> AttachDeviceChild(RenderNative::IVulkanEditorDeviceChild &child) override {
            if (child_)
                return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
            child_ = &child;
            return Result<void>::Success();
        }

        void DetachDeviceChild(RenderNative::IVulkanEditorDeviceChild &child) noexcept override {
            if (child_ == &child)
                child_ = nullptr;
        }

        void ReleaseDrainedChild() {
            if (child_)
                child_->ReleaseNative();
            child_ = nullptr;
        }

    private:
        Render::VulkanGraphTest::NativeGraphFixture &native_;
        RenderNative::IVulkanEditorDeviceChild *child_{};
        Render::FrameToken outputFrame_;
        // Same explicit scripted WSI tuple as VulkanGuiCommandTests, never physical presentation evidence.
        RenderNative::VulkanGuiOutput output_{OpenXR::NativeHandle<VkImage>(800),
                                              OpenXR::NativeHandle<VkImageView>(801),
                                              OpenXR::NativeHandle<VkSemaphore>(802),
                                              {8, 8},
                                              {},
                                              true};
    };

    struct ApplicationVulkanFixture final {
        ShaderDriverScope shader;
        Render::VulkanGraphTest::NativeGraphFixture native{true};
        GraphGraphicsPort port{native};

        ApplicationVulkanFixture() {
            native.resourceState.state.sizedAllocations = true;
            native.encoded.expectedIndexCount = 36;  // Canonical authored box from MakeHierarchyScene.
            VkPhysicalDeviceProperties properties{};
            auto &limits = properties.limits;
            limits.minUniformBufferOffsetAlignment = 16;
            limits.maxUniformBufferRange = 65536;
            limits.maxDescriptorSetUniformBuffers = 128;
            limits.maxDescriptorSetSampledImages = 128;
            limits.maxDescriptorSetSamplers = 128;
            limits.maxPerStageDescriptorUniformBuffers = 128;
            limits.maxPerStageDescriptorSampledImages = 128;
            limits.maxPerStageDescriptorSamplers = 128;
            limits.maxPerStageResources = 128;
            limits.maxBoundDescriptorSets = 4;
            limits.maxSamplerAllocationCount = 4096;
            REQUIRE(native.resources.InitializeShaders(properties, Render::RenderAdapterId{"vulkan:test"}, ::Resolve, 11, true).HasValue());
        }

        /** @brief Structurally admitted App multipass stages with no Layer output, never GPU execution evidence. */
        static std::vector<std::uint8_t> Code(const Render::ShaderStage stage) {
            const std::vector<std::uint32_t> words{0x07230203,
                                                   0x00010600,
                                                   0,
                                                   2,
                                                   0,
                                                   (5U << 16) | 15U,
                                                   stage == Render::ShaderStage::Vertex ? 0U : 4U,
                                                   1,
                                                   stage == Render::ShaderStage::Vertex ? 0x614d5356U : 0x614d5350U,
                                                   0x00006e69};
            const auto bytes = std::as_bytes(std::span{words});
            std::vector<std::uint8_t> result(bytes.size());
            std::ranges::transform(bytes, result.begin(), [](const std::byte value) {
                return std::to_integer<std::uint8_t>(value);
            });
            return result;
        }

        static ShaderCook::CookFixture Cook(const bool multiview) {
            const std::array stages{Render::ShaderArtifactStage{Render::ShaderStage::Vertex, "VSMain", Code(Render::ShaderStage::Vertex)},
                                    Render::ShaderArtifactStage{Render::ShaderStage::Fragment, "PSMain",
                                                                Code(Render::ShaderStage::Fragment)}};
            return ShaderCook::CookFixture{multiview ? ShaderCook::MultiviewEvidence() : ShaderCook::Evidence(), stages, multiview};
        }
    };
}  // namespace Horo::Tests::OpenXRApplication
