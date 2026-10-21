#include "host/OpenXRVulkanSwapchain.h"

#include "Horo/Runtime/Render/RenderFrontend.h"

#include <algorithm>
#include <format>

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Exact Horo/native color/depth formats supported by this qualified private adapter. */
        [[nodiscard]] std::optional<std::int64_t> NativeFormat(Render::RenderTextureFormat format) noexcept {
            using enum Render::RenderTextureFormat;
            switch (format) {
                case Rgba8Unorm:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case Rgba8UnormSrgb:
                    return VK_FORMAT_R8G8B8A8_SRGB;
                case Bgra8Unorm:
                    return VK_FORMAT_B8G8R8A8_UNORM;
                case Bgra8UnormSrgb:
                    return VK_FORMAT_B8G8R8A8_SRGB;
                case Rgba16Float:
                    return VK_FORMAT_R16G16B16A16_SFLOAT;
                case Depth16Unorm:
                    return VK_FORMAT_D16_UNORM;
                case Depth32Float:
                    return VK_FORMAT_D32_SFLOAT;
                default:
                    return std::nullopt;
            }
        }
    }  // namespace

    /** @brief Prepared immutable native image with generation-fenced acquisition identity, never allocation ownership. */
    struct OpenXRVulkanSwapchainImages::Source final : Render::Detail::IVulkanExternalTextureSource {
        OpenXRSwapchain *owner{};
        IOpenXRGraphicsBinding *graphics{};
        const XrGraphicsBindingVulkanKHR *binding{};
        VkDevice device{VK_NULL_HANDLE};
        std::uint64_t incarnation{};
        VkImage native{VK_NULL_HANDLE};
        std::uint32_t family{};
        Render::RenderQueueId queue;
        NativeSwapchainRequest request;
        XRSwapchainImageId image;
        mutable bool releaseStateProved{};

        Result<void> Validate() const override {
            if (binding->device != device || binding->queueFamilyIndex != family || binding->queueIndex != 0)
                return Result<void>::Failure(MakeError(XRErrors::CapabilityStale));
            if (auto current = graphics->Validate(image.target.session); current.HasError())
                return current;
            return owner->ValidateAcquisition(image);
        }

        std::uint64_t DeviceIncarnation() const noexcept override {
            return incarnation;
        }

        const Render::RenderTextureDescriptor &Descriptor() const noexcept override {
            return request.texture;
        }

        VkDevice NativeDevice() const noexcept override {
            return device;
        }

        VkImage NativeImage() const noexcept override {
            return native;
        }

        std::uint32_t NativeQueueFamily() const noexcept override {
            return family;
        }

        Render::RenderGraphLogicalState AcquiredState() const noexcept override {
            const bool depth = request.role == NativeSwapchainRole::Depth;
            return {Render::RenderGraphSynchronizationAccess::ReadWrite,
                    depth ? Render::RenderGraphSynchronizationOperation::DepthStencilAttachment
                          : Render::RenderGraphSynchronizationOperation::ColorAttachment,
                    Render::RenderGraphPipelineScope::Graphics,
                    depth ? Render::RenderGraphTextureLayout::DepthStencilAttachment : Render::RenderGraphTextureLayout::ColorAttachment,
                    queue};
        }

        void RecordNativeRetirement(const Render::RenderGraphLogicalState &state, const bool hasNativeState) const noexcept override {
            const auto required = AcquiredState();
            releaseStateProved = hasNativeState && state.layout == required.layout && state.queue == required.queue &&
                                 state.operation == required.operation && state.scope == required.scope;
        }

        Render::RenderGraphLogicalState RequiredFinalState() const noexcept override {
            auto state = AcquiredState();
            state.scope = Render::RenderGraphPipelineScope::External;
            return state;
        }
    };

    /** @copydoc OpenXRVulkanSwapchainImages::OpenXRVulkanSwapchainImages */
    OpenXRVulkanSwapchainImages::OpenXRVulkanSwapchainImages(Render::RenderFrontend &frontend, IOpenXRGraphicsBinding &graphics,
                                                             const XrGraphicsBindingVulkanKHR &binding, const Render::RenderQueueId queue)
        : frontend_(&frontend), graphics_(&graphics), binding_(&binding), queue_(queue) {}

    /** @copydoc OpenXRVulkanSwapchainImages::~OpenXRVulkanSwapchainImages */
    OpenXRVulkanSwapchainImages::~OpenXRVulkanSwapchainImages() {
        HORO_INVARIANT_MSG(count_ == 0 && !imported_.IsValid(), "XR Vulkan image borrows require explicit complete retirement");
    }

    /** @copydoc OpenXRVulkanSwapchainImages::Bind */
    Result<void> OpenXRVulkanSwapchainImages::Bind(OpenXRSwapchain &owner) {
        if (owner_ != nullptr || count_ != 0)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        owner_ = &owner;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRVulkanSwapchainImages::Validate */
    Result<void> OpenXRVulkanSwapchainImages::Validate(const NativeSwapchainRequest &request) const {
        if (owner_ == nullptr || binding_->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR || binding_->device == VK_NULL_HANDLE ||
            !queue_.IsValid() || binding_->queueIndex != 0 || !request.texture.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (request.foveation != NativeSwapchainFoveation::None ||
            (request.role != NativeSwapchainRole::Color && request.role != NativeSwapchainRole::Depth) ||
            !NativeFormat(request.texture.format).has_value() || request.texture.sampleCount != 1 ||
            request.texture.dimension != Render::RenderTextureDimension::TwoD || request.texture.mipCount != 1 ||
            !Render::HasTextureUsage(request.texture.usage, Render::RenderTextureUsage::RenderAttachment))
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        return graphics_->Validate(request.target.session);
    }

    /** @copydoc OpenXRVulkanSwapchainImages::SelectFormat */
    Result<std::int64_t> OpenXRVulkanSwapchainImages::SelectFormat(const NativeSwapchainRequest &request,
                                                                   const std::span<const std::int64_t> formats) const {
        if (auto valid = Validate(request); valid.HasError())
            return Result<std::int64_t>::Failure(valid.ErrorValue());
        const auto native = *NativeFormat(request.texture.format);
        return std::ranges::find(formats, native) != formats.end()
                   ? Result<std::int64_t>::Success(native)
                   : Result<std::int64_t>::Failure(MakeError(XRErrors::OperationUnsupported));
    }

    /** @copydoc OpenXRVulkanSwapchainImages::Enumerate */
    Result<void> OpenXRVulkanSwapchainImages::Enumerate(const XrSwapchain swapchain, const std::uint32_t count,
                                                        PFN_xrEnumerateSwapchainImages enumerate) {
        if (!enumerate || count == 0 || count > sources_.size() || count_ != 0)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        std::array<XrSwapchainImageVulkanKHR, 64> images{};
        for (auto &image : images)
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        std::uint32_t written{};
        const XrResult result = enumerate(swapchain, count, &written, reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()));
        if (XR_FAILED(result))
            return Result<void>::Failure(
                MakeError(XRErrors::OperationUnavailable,
                          std::format("OpenXR Vulkan image enumeration failed; native result={}", static_cast<std::int32_t>(result))));
        if (written != count)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        for (std::uint32_t index = 0; index < count; ++index) {
            if (images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR || images[index].next != nullptr ||
                images[index].image == VK_NULL_HANDLE)
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
            for (std::uint32_t prior = 0; prior < index; ++prior)
                if (images[prior].image == images[index].image)
                    return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        }
        // Load-time prepared sources are reused only after the prior acquisition registration retires.
        for (std::uint32_t index = 0; index < count; ++index) {
            auto source = std::make_shared<Source>();
            source->owner = owner_;
            source->graphics = graphics_;
            source->binding = binding_;
            source->device = binding_->device;
            source->incarnation = frontend_->Capabilities().support.deviceIncarnation;
            source->native = images[index].image;
            source->family = binding_->queueFamilyIndex;
            source->queue = queue_;
            sources_[index] = std::move(source);
            ++count_;
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRVulkanSwapchainImages::Import */
    Result<Render::RenderTextureHandle> OpenXRVulkanSwapchainImages::Import(const NativeSwapchainRequest &request,
                                                                            const XRSwapchainImageId &image, const std::uint32_t index) {
        using Outcome = Result<Render::RenderTextureHandle>;
        if (auto valid = Validate(request); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (imported_.IsValid() && retirementConfirmed_) {
            imported_ = {};
            retirementConfirmed_ = false;
        }
        if (index >= count_ || imported_.IsValid())
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        auto &source = sources_[index];
        source->request = request;
        source->image = image;
        source->releaseStateProved = false;
        auto imported = Render::Detail::RenderExternalTextureAccess::Import(*frontend_, source);
        if (imported.HasValue()) {
            imported_ = imported.Value();
            importedIndex_ = index;
        }
        return imported;
    }

    /** @copydoc OpenXRVulkanSwapchainImages::Retire */
    Result<void> OpenXRVulkanSwapchainImages::Retire(const Render::RenderTextureHandle texture) {
        if (texture != imported_ || !texture.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        return frontend_->ReleaseTexture(texture);
    }

    /** @copydoc OpenXRVulkanSwapchainImages::RetirementComplete */
    Result<bool> OpenXRVulkanSwapchainImages::RetirementComplete(const Render::RenderTextureHandle texture) {
        if (texture != imported_ || !texture.IsValid())
            return Result<bool>::Failure(MakeError(XRErrors::IdentityStale));
        auto complete = Render::Detail::RenderExternalTextureAccess::RetirementComplete(*frontend_, texture);
        if (complete.HasValue() && complete.Value()) {
            if (!sources_[importedIndex_]->releaseStateProved)
                return Result<bool>::Failure(
                    MakeError(XRErrors::OperationIncompatible,
                              "Actual retired Vulkan image state does not prove the OpenXR-required layout and bound queue."));
            retirementConfirmed_ = true;
        }
        return complete;
    }

    /** @copydoc OpenXRVulkanSwapchainImages::Reset */
    Result<void> OpenXRVulkanSwapchainImages::Reset() {
        if (imported_.IsValid() && !retirementConfirmed_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        imported_ = {};
        retirementConfirmed_ = false;
        for (auto &source : sources_)
            source.reset();
        count_ = 0;
        return Result<void>::Success();
    }
}  // namespace Horo::XR::OpenXRInternal
