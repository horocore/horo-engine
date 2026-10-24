#include "host/OpenXRVulkanSwapchain.h"

#include "Horo/Runtime/Render/RenderFrontend.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <format>
#include <type_traits>

namespace Horo::XR::OpenXRInternal {
    namespace {
        // Ordinary same-address-space data-pointer ABI qualification is required; these
        // SDK layout contracts do not prove pointer representation or grant aliasing.
        static_assert(std::is_standard_layout_v<XrSwapchainImageVulkanKHR>);
        static_assert(std::is_standard_layout_v<XrSwapchainImageBaseHeader>);
        static_assert(std::is_trivially_copyable_v<XrSwapchainImageVulkanKHR *>);
        static_assert(std::is_trivially_copyable_v<XrSwapchainImageBaseHeader *>);
        static_assert(sizeof(XrSwapchainImageVulkanKHR *) == sizeof(XrSwapchainImageBaseHeader *));
        static_assert(alignof(XrSwapchainImageVulkanKHR) >= alignof(XrSwapchainImageBaseHeader));
        static_assert(offsetof(XrSwapchainImageVulkanKHR, type) == 0);
        static_assert(offsetof(XrSwapchainImageBaseHeader, type) == 0);
        static_assert(offsetof(XrSwapchainImageVulkanKHR, next) == offsetof(XrSwapchainImageBaseHeader, next));
        static_assert(std::is_same_v<decltype(XrSwapchainImageVulkanKHR::type), decltype(XrSwapchainImageBaseHeader::type)>);
        static_assert(std::is_same_v<decltype(XrSwapchainImageVulkanKHR::next), decltype(XrSwapchainImageBaseHeader::next)>);

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
        Render::Detail::VulkanExternalViewDispatch viewDispatch;
        Render::Detail::VulkanExternalNativeViews views;

        ~Source() override {
            if (request.texture.layerCount > 1)
                for (auto view : views.layers)
                    if (view != VK_NULL_HANDLE)
                        viewDispatch.destroy(device, view, nullptr);
            if (views.attachment != VK_NULL_HANDLE)
                viewDispatch.destroy(device, views.attachment, nullptr);
        }

        Render::Detail::VulkanExternalNativeViews PreparedNativeViews() const noexcept override {
            return views;
        }

        bool AdmitsFirstEyeGuiMirror(const Render::RenderTextureViewDescriptor &view) const noexcept override {
            const auto format = request.texture.format;
            return request.role == NativeSwapchainRole::Color && views.layerCount > 0 && views.layers[0] != VK_NULL_HANDLE &&
                   (format == Render::RenderTextureFormat::Rgba8Unorm || format == Render::RenderTextureFormat::Bgra8Unorm) &&
                   request.texture.layerCount >= 2 && Render::HasTextureUsage(request.texture.usage, Render::RenderTextureUsage::Sampled) &&
                   view.format == format && view.aspect == Render::RenderTextureAspect::Color &&
                   view.dimension == Render::RenderTextureViewDimension::TwoD && view.baseMip == 0 && view.mipCount == 1 &&
                   view.baseLayer == 0 && view.layerCount == 1;
        }

        /** @brief Creates the exact immutable array/eye views cold, before acquisition publication. */
        Result<void> PrepareViews() {
            const bool depth = request.role == NativeSwapchainRole::Depth;
            const VkImageAspectFlags aspects = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageViewCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                       .image = native,
                                       .viewType = request.texture.layerCount == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                       .format = static_cast<VkFormat>(*NativeFormat(request.texture.format)),
                                       .subresourceRange = {aspects, 0, 1, 0, request.texture.layerCount}};
            auto result = viewDispatch.create(device, &info, nullptr, &views.attachment);
            if (result != VK_SUCCESS || views.attachment == VK_NULL_HANDLE) {
                views.attachment = VK_NULL_HANDLE;
                return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable, "Preparing the native XR attachment view failed."));
            }
            if (request.texture.layerCount == 1) {
                views.layers[0] = views.attachment;
                views.layerCount = 1;
                return Result<void>::Success();
            }
            info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            info.subresourceRange.layerCount = 1;
            for (std::uint32_t layer = 0; layer < request.texture.layerCount; ++layer) {
                info.subresourceRange.baseArrayLayer = layer;
                result = viewDispatch.create(device, &info, nullptr, &views.layers[layer]);
                if (result != VK_SUCCESS || views.layers[layer] == VK_NULL_HANDLE) {
                    views.layers[layer] = VK_NULL_HANDLE;
                    return Result<void>::Failure(
                        MakeError(XRErrors::OperationUnavailable, "Preparing a native XR layer attachment view failed."));
                }
                ++views.layerCount;
            }
            return Result<void>::Success();
        }

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
                                                             const XrGraphicsBindingVulkanKHR &binding, const Render::RenderQueueId queue,
                                                             const Render::Detail::VulkanExternalViewDispatch views)
        : frontend_(&frontend), graphics_(&graphics), binding_(&binding), queue_(queue), views_(views) {}

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
            !queue_.IsValid() || binding_->queueIndex != 0 || !request.texture.IsValid() || !views_.create || !views_.destroy)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (request.foveation != NativeSwapchainFoveation::None ||
            (request.role != NativeSwapchainRole::Color && request.role != NativeSwapchainRole::Depth) ||
            !NativeFormat(request.texture.format).has_value() || request.texture.sampleCount != 1 ||
            request.texture.layerCount > Render::Detail::VulkanExternalNativeViews::MaximumLayers ||
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
        const auto native = NativeFormat(request.texture.format);
        if (!native.has_value())
            return Result<std::int64_t>::Failure(MakeError(XRErrors::OperationUnsupported));
        return std::ranges::find(formats, *native) != formats.end()
                   ? Result<std::int64_t>::Success(*native)
                   : Result<std::int64_t>::Failure(MakeError(XRErrors::OperationUnsupported));
    }

    /** @copydoc OpenXRVulkanSwapchainImages::Enumerate */
    Result<void> OpenXRVulkanSwapchainImages::Enumerate(const NativeSwapchainRequest &request, const std::uint32_t count,
                                                        const IOpenXRSwapchainImageEnumeration &enumeration) {
        if (auto valid = Validate(request); valid.HasError())
            return valid;
        if (!enumeration.IsAvailable() || count == 0 || count > sources_.size() || count_ != 0)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        std::array<XrSwapchainImageVulkanKHR, 64> images{};
        for (auto &image : images)
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        std::uint32_t written{};
        const XrResult result = enumeration.Enumerate(count, &written, std::bit_cast<XrSwapchainImageBaseHeader *>(images.data()));
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
            source->request = request;
            source->viewDispatch = views_;
            if (auto prepared = source->PrepareViews(); prepared.HasError())
                return prepared;  // Prefix views stay owned until the canonical Reset before native swapchain destruction.
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
        const auto &source = sources_[index];
        const auto &prepared = source->request;
        const auto &left = prepared.texture;
        const auto &right = request.texture;
        if (prepared.target != request.target || prepared.role != request.role || prepared.foveation != request.foveation ||
            left.dimension != right.dimension || left.extent != right.extent || left.format != right.format ||
            left.mipCount != right.mipCount || left.layerCount != right.layerCount || left.sampleCount != right.sampleCount ||
            left.usage != right.usage || left.depth != right.depth || source->views.attachment == VK_NULL_HANDLE)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
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
