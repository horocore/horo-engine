#pragma once
/** @file VulkanEditorGraphicsBridge.h
 * @brief Non-installed selected-device composition seam for the matching Vulkan editor pair. */
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "VulkanBackendModule.h"

namespace Horo::Render::Detail {
    /** @brief Cold reducer inputs resolved by the selected viewport's frontend resource owner. */
    struct VulkanStereoCoveragePreparation final {
        std::span<const std::uint8_t> artifact;
        std::array<std::uint64_t, 2> eyes;
        std::array<MultiviewRenderRectangle, 2> rectangles;
        std::uint64_t reset{}, result{}, parameters{};
        RenderQueueId queue;
    };

    /** @brief Canonical retired retention slot, independent of swapchain image and encoder call counts. */
    struct VulkanFrameRetentionSlot {
        std::uint32_t index{};
        std::uint32_t count{};
    };

    /** @brief Borrowed exact device children held by the existing completion owner. */
    struct VulkanGuiSampleBinding {
        VkImageView view{};
        VkSampler sampler{};
    };

    /** @brief Borrowed coherent frontend-owned encoder buffer. */
    struct VulkanGuiBufferBinding {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        VkDeviceSize bytes{};
    };

    /** @brief Sole GUI native encoder: recording authority only, without submission or presentation. */
    class IVulkanGuiEncoder {
    public:
        virtual ~IVulkanGuiEncoder() = default;
        [[nodiscard]] virtual Result<void> Record(VkCommandBuffer command, VulkanFrameRetentionSlot slot,
                                                  std::span<const VulkanGuiSampleBinding> samples,
                                                  std::span<const VulkanGuiBufferBinding> buffers) = 0;
    };

    /** @brief Explicit host-owned device child released after canonical drain and before native device destruction. */
    class IVulkanEditorDeviceChild {
    public:
        virtual ~IVulkanEditorDeviceChild() = default;
        virtual void ReleaseNative() noexcept = 0;
    };

    /** @brief Exact successful API/extension requests retained immutably by the current native owner.
     * @details Spans remain stable until all device children release and the corresponding native instance/device retires.
     * Supported extension enumeration is not enabled-extension evidence.
     */
    struct VulkanEnabledGraphicsFacts {
        VulkanApiVersion api;
        std::span<const std::string> instanceExtensions;
        std::span<const std::string> deviceExtensions;
    };

    /** @brief Borrowed same-instance/device facts, queried only after explicit surface realization. */
    struct VulkanEditorDevice {
        VkInstance instance{};
        VkPhysicalDevice physical{};
        VkDevice device{};
        VkQueue graphicsQueue{};
        std::uint32_t graphicsFamily{};
        std::uint32_t frames{};
        VkFormat outputFormat{};
        PFN_vkGetInstanceProcAddr instanceResolver{};
        PFN_vkGetDeviceProcAddr deviceResolver{};
        RenderQueueId graphicsIdentity;
        VulkanEnabledGraphicsFacts enabled;
    };

    /** @brief Explicit private composition port implemented by the selected runtime's existing owners. */
    class IVulkanEditorGraphicsPort {
    public:
        virtual ~IVulkanEditorGraphicsPort() = default;
        [[nodiscard]] virtual Result<VulkanEditorDevice> QueryDevice() const = 0;
        /** @brief Borrows exact frontend-owned sample children during load-time descriptor preparation. */
        [[nodiscard]] virtual Result<VulkanGuiSampleBinding> BorrowSample(const GuiSampledResourceInstance &sample) const = 0;
        [[nodiscard]] virtual Result<VulkanFrameRetentionSlot> FrameSlot(FrameToken frame) const = 0;
        [[nodiscard]] virtual Result<void> PreparePrimaryOutput(FrameToken frame, PrimaryOutputAttachment attachment) = 0;
        [[nodiscard]] virtual Result<void> ExecuteGui(const GuiCompositionRequest &request, IVulkanGuiEncoder &encoder) = 0;
        [[nodiscard]] virtual Result<void> WriteFrameBuffer(FrameToken frame, std::uint64_t instance, std::span<const std::byte> bytes) = 0;
        /** @brief Polls prior GPU readers/writers of a frontend-resolved buffer without beginning a frame.
         * @param instance Exact live native buffer instance from the selected frontend.
         * @return True for a reusable preparation slot, false while native work remains, or typed owner/lifecycle failure.
         */
        [[nodiscard]] virtual Result<bool> ObservePreparationBuffer(std::uint64_t instance) = 0;

        /** @brief Reports the selected native image-copy route for bounded stereo coverage observations. */
        [[nodiscard]] virtual bool SupportsStereoCoverageReadback() const noexcept {
            return false;
        }

        /** @brief Copies exact native completed pixels without allocating or waiting.
         * @return True for copied bytes, false when the route or completion is unavailable, or a typed error.
         */
        [[nodiscard]] virtual Result<bool> ReadCompletedStereoCoverage(std::uint64_t, RenderTimelinePoint, std::span<std::byte>) {
            return Result<bool>::Success(false);
        }

        /** @brief Reports the closed compute route only on the exact selected graphics queue. */
        [[nodiscard]] virtual bool SupportsStereoCoverageReduction(RenderQueueId) const noexcept {
            return false;
        }

        /** @brief Materializes immutable descriptors and the closed reduction pipeline outside normal frame recording. */
        [[nodiscard]] virtual Result<std::shared_ptr<IResidentStereoCoverageReducer>> PrepareStereoCoverageReduction(
            const VulkanStereoCoveragePreparation &) {
            return Result<std::shared_ptr<IResidentStereoCoverageReducer>>::Failure(
                MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
        }

        /** @brief Returns completed exact full-eye counters without waiting or reading a different submission. */
        [[nodiscard]] virtual Result<bool> ReadCompletedStereoCoverageReduction(std::uint64_t, RenderTimelinePoint,
                                                                                RenderGraphStereoCoverageReduction::Tallies &) {
            return Result<bool>::Success(false);
        }

        /** @brief Writes a whole host-visible uniform only after its exact prior native use retires.
         * @param instance Exact live uniform buffer instance from the selected frontend.
         * @param bytes Prepared full-size native uniform contents.
         * @return Success or typed busy, owner, lifecycle or resource failure. Never waits or starts a native frame.
         */
        [[nodiscard]] virtual Result<void> WritePreparationUniform(std::uint64_t instance, std::span<const std::byte> bytes) = 0;
        /** @brief Checks exact queue acceptance of previously recorded editor graph work.
         * @param signal That graph submission's frontend-issued point, independent of the later GUI/WSI tail.
         * @param deviceIncarnation The originating selected frontend's device incarnation.
         * @return True for native queue acceptance, false if still unissued, or typed identity/owner failure.
         * @details Call on the owner immediately after Present/cancel. No presentation success, GPU completion or wait is implied.
         */
        [[nodiscard]] virtual Result<bool> WasSubmissionAccepted(RenderTimelinePoint signal, std::uint64_t deviceIncarnation) const = 0;
        [[nodiscard]] virtual Result<void> AttachDeviceChild(IVulkanEditorDeviceChild &child) = 0;
        virtual void DetachDeviceChild(IVulkanEditorDeviceChild &child) noexcept = 0;

        /** @brief Copies tightly packed RGBA8 bytes from an exact completed color readback receipt.
         * @param instance Exact live host staging instance resolved by the selected frontend.
         * @param completion Timeline point accepted for that color copy.
         * @param destination Output span with the exact captured byte count.
         * @return True when copied, false for a missing route or incomplete receipt, or a typed failure.
         */
        [[nodiscard]] virtual Result<bool> ReadCompletedColor([[maybe_unused]] std::uint64_t instance,
                                                              [[maybe_unused]] RenderTimelinePoint completion,
                                                              [[maybe_unused]] std::span<std::byte> destination) {
            return Result<bool>::Success(false);
        }

        /** @brief Reports the selected device's native image-to-buffer copy route. */
        [[nodiscard]] virtual bool SupportsColorReadback() const noexcept {
            return false;
        }
    };

    /** @brief Host-owned borrowed connection; runtime attachment never selects or creates another renderer. */
    class VulkanEditorGraphicsBridge final {
    public:
        [[nodiscard]] IVulkanEditorGraphicsPort *Port() const noexcept {
            return port_;
        }

        /** @brief Binds at most one selected runtime; the host keeps this object alive until runtime destruction. */
        [[nodiscard]] bool Attach(IVulkanEditorGraphicsPort &port) noexcept {
            if (port_ != nullptr)
                return false;
            port_ = &port;
            return true;
        }

        void Detach(IVulkanEditorGraphicsPort &port) noexcept {
            if (port_ == &port)
                port_ = nullptr;
        }

    private:
        IVulkanEditorGraphicsPort *port_{};
    };
}  // namespace Horo::Render::Detail
