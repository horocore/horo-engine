#include "VulkanResourceRuntime.h"

#include "Horo/Runtime/Render/RenderCapabilities.h"
#include "VulkanRenderBackendErrors.h"
#include "VulkanResourceConversions.h"

#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <new>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        [[nodiscard]] Error ResourceError(const ErrorCodeDescriptor &code, std::string message) {
            return MakeError(code, std::move(message));
        }

        [[nodiscard]] bool IsBufferRealizationValid(const RenderMemoryCostPlan &plan, const RenderBufferDescriptor &descriptor,
                                                    const std::span<const std::byte> initialData,
                                                    const RenderMemoryPlacement &placement) noexcept {
            return VulkanResourceConversion::PlacementMatches(plan, placement) &&
                   (initialData.empty() || initialData.size() == descriptor.byteSize) &&
                   (initialData.empty() || descriptor.access == RenderBufferAccess::HostVisible);
        }

        [[nodiscard]] VkBufferCreateInfo BufferCreateInfo(const RenderBufferDescriptor &descriptor) noexcept {
            return {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .size = descriptor.byteSize,
                    .usage = VulkanResourceConversion::BufferUsage(descriptor.usage),
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        }

    }  // namespace

    struct VulkanResourceRuntime::Impl {
        enum class AllocationFailure : std::uint8_t {
            None,
            MemoryTypeUnavailable,
            AllocationOrBindingFailed
        };

        template <typename NativeHandle, typename DestroyNative> struct AllocationResource {
            AllocationResource() = default;

            AllocationResource(const AllocationResource &) = delete;
            AllocationResource &operator=(const AllocationResource &) = delete;

            ~AllocationResource() {
                if (handle != VK_NULL_HANDLE && !borrowed)
                    destroy(device, handle, nullptr);
                if (memory != VK_NULL_HANDLE)
                    freeMemory(device, memory, nullptr);
            }

            RenderResourceNativeLifetime lifetime;
            bool borrowed{false};
            VkDevice device{VK_NULL_HANDLE};
            NativeHandle handle{VK_NULL_HANDLE};
            VkDeviceMemory memory{VK_NULL_HANDLE};
            DestroyNative destroy{nullptr};
            PFN_vkFreeMemory freeMemory{nullptr};
        };

        struct Buffer final : AllocationResource<VkBuffer, PFN_vkDestroyBuffer> {
            RenderBufferDescriptor descriptor;
            VulkanGraphResourceState graphState;
        };

        struct Texture final : AllocationResource<VkImage, PFN_vkDestroyImage> {
            const IVulkanExternalTextureSource *externalSource{};
            RenderTextureDescriptor descriptor;
            VulkanGraphResourceState graphState;
            VkImageView colorView{VK_NULL_HANDLE};
            PFN_vkDestroyImageView destroyView{};

            ~Texture() {
                if (externalSource != nullptr)
                    externalSource->RecordNativeRetirement(graphState.logical, graphState.hasNativeState);
                if (colorView != VK_NULL_HANDLE)
                    destroyView(this->device, colorView, nullptr);
            }
        };

        struct View {
            RenderResourceNativeLifetime lifetime;
            View() = default;

            View(const View &) = delete;
            View &operator=(const View &) = delete;

            ~View() {
                if (handle != VK_NULL_HANDLE)
                    destroy(device, handle, nullptr);
            }

            VkDevice device{VK_NULL_HANDLE};
            VkImageView handle{VK_NULL_HANDLE};
            PFN_vkDestroyImageView destroy{nullptr};
            std::shared_ptr<Texture> texture;
            RenderTextureAspect aspect{};
            RenderTextureViewDescriptor descriptor;
        };

        struct Mesh {
            RenderResourceNativeLifetime lifetime;
            std::uint64_t vertex{0};
            std::uint64_t index{0};
            RenderMeshDescriptor descriptor;
        };

        struct Target {
            RenderResourceNativeLifetime lifetime;
            std::shared_ptr<View> color;
            std::shared_ptr<View> depth;
            FramebufferExtent extent;
        };

        struct Dispatch {
            PFN_vkCreateBuffer createBuffer{nullptr};
            PFN_vkDestroyBuffer destroyBuffer{nullptr};
            PFN_vkGetBufferMemoryRequirements getBufferRequirements{nullptr};
            PFN_vkCreateImage createImage{nullptr};
            PFN_vkDestroyImage destroyImage{nullptr};
            PFN_vkGetImageMemoryRequirements getImageRequirements{nullptr};
            PFN_vkAllocateMemory allocateMemory{nullptr};
            PFN_vkFreeMemory freeMemory{nullptr};
            PFN_vkBindBufferMemory bindBufferMemory{nullptr};
            PFN_vkBindImageMemory bindImageMemory{nullptr};
            PFN_vkMapMemory mapMemory{nullptr};
            PFN_vkUnmapMemory unmapMemory{nullptr};
            PFN_vkCreateImageView createImageView{nullptr};
            PFN_vkDestroyImageView destroyImageView{nullptr};
        };

        template <typename Resource> using Registry = std::unordered_map<std::uint64_t, std::shared_ptr<Resource>>;

        VkDevice device{VK_NULL_HANDLE};
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        Dispatch dispatch;
        Registry<Buffer> buffers;
        Registry<Texture> textures;
        Registry<View> views;
        Registry<Mesh> meshes;
        Registry<Target> targets;
        std::uint64_t maximumIdentity{0};
        std::uint64_t nextIdentity{1};

        explicit Impl(const std::uint64_t identityLimit) noexcept : maximumIdentity(identityLimit) {}

        [[nodiscard]] bool DispatchComplete() const noexcept {
            const std::array availability{
                this->dispatch.createBuffer != nullptr,
                this->dispatch.destroyBuffer != nullptr,
                this->dispatch.getBufferRequirements != nullptr,
                this->dispatch.createImage != nullptr,
                this->dispatch.destroyImage != nullptr,
                this->dispatch.getImageRequirements != nullptr,
                this->dispatch.allocateMemory != nullptr,
                this->dispatch.freeMemory != nullptr,
                this->dispatch.bindBufferMemory != nullptr,
                this->dispatch.bindImageMemory != nullptr,
                this->dispatch.mapMemory != nullptr,
                this->dispatch.unmapMemory != nullptr,
                this->dispatch.createImageView != nullptr,
                this->dispatch.destroyImageView != nullptr,
            };
            return std::ranges::all_of(availability, [](const bool available) {
                return available;
            });
        }

        template <typename T> [[nodiscard]] static T *Find(const Registry<T> &entries, const std::uint64_t identity) noexcept {
            const auto found = entries.find(identity);
            return found == entries.end() ? nullptr : found->second.get();
        }

        [[nodiscard]] std::optional<std::uint64_t> IssueIdentity() noexcept {
            if (nextIdentity == 0 || nextIdentity > maximumIdentity)
                return std::nullopt;
            return nextIdentity++;
        }

        template <typename Resource>
        [[nodiscard]] Result<std::uint64_t> Register(Registry<Resource> &entries, std::shared_ptr<Resource> resource) {
            const auto identity = IssueIdentity();
            if (!identity.has_value())
                return Result<std::uint64_t>::Failure(
                    ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Vulkan resource identities are exhausted."));
            try {
                entries.emplace(*identity, std::move(resource));
            } catch (const std::bad_alloc &) {
                return Result<std::uint64_t>::Failure(
                    ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Vulkan resource registry allocation failed."));
            }
            return Result<std::uint64_t>::Success(*identity);
        }

        template <typename Resource, typename... Arguments>
        [[nodiscard]] static std::shared_ptr<Resource> Allocate(Arguments &&...arguments) noexcept {
            try {
                return std::make_shared<Resource>(std::forward<Arguments>(arguments)...);
            } catch (const std::bad_alloc &) {
                return nullptr;
            }
        }

        template <typename Resource, typename BindMemory>
        [[nodiscard]] AllocationFailure AllocateAndBind(Resource &resource, const VkMemoryRequirements &requirements,
                                                        const VkMemoryPropertyFlags flags, BindMemory bindMemory) const {
            using enum AllocationFailure;
            const auto type = MemoryType(requirements.memoryTypeBits, flags);
            if (!type.has_value())
                return MemoryTypeUnavailable;
            if (const VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                      .allocationSize = requirements.size,
                                                      .memoryTypeIndex = *type};
                dispatch.allocateMemory(device, &allocation, nullptr, &resource.memory) != VK_SUCCESS ||
                bindMemory(device, resource.handle, resource.memory, 0) != VK_SUCCESS)
                return AllocationOrBindingFailed;
            return None;
        }

        [[nodiscard]] static Error AllocationError(const AllocationFailure failure, const std::string_view resourceName) {
            if (failure == AllocationFailure::MemoryTypeUnavailable)
                return ResourceError(VulkanBackendErrors::MemoryTypeUnavailable, std::string(resourceName) + " memory type unavailable.");
            return ResourceError(VulkanBackendErrors::ResourceCreationFailed, std::string(resourceName) + " allocation or binding failed.");
        }

        [[nodiscard]] static bool RequirementsStillMatch(const VkMemoryRequirements &requirements,
                                                         const RenderMemoryCostPlan &plan) noexcept {
            return VulkanResourceConversion::RequirementsMatch(requirements, plan);
        }

        [[nodiscard]] bool Upload(Buffer &buffer, const std::span<const std::byte> initialData) const {
            if (initialData.empty())
                return true;
            if (initialData.size() > buffer.descriptor.byteSize)
                return false;
            void *mapped{nullptr};
            if (dispatch.mapMemory(device, buffer.memory, 0, initialData.size(), 0, &mapped) != VK_SUCCESS)
                return false;
            std::ranges::copy(initialData, static_cast<std::byte *>(mapped));
            dispatch.unmapMemory(device, buffer.memory);
            return true;
        }

        [[nodiscard]] static bool AttachmentsKnown(const std::uint64_t colorIdentity, const View *color, const std::uint64_t depthIdentity,
                                                   const View *depth) noexcept {
            return (colorIdentity == 0 || color != nullptr) && (depthIdentity == 0 || depth != nullptr);
        }

        [[nodiscard]] static bool AttachmentExtentMatches(const RenderTargetDescriptor &descriptor, const View *view) noexcept {
            return view == nullptr || view->texture->descriptor.extent == descriptor.extent;
        }

        [[nodiscard]] static bool AttachmentSamplesMatch(const RenderTargetDescriptor &descriptor, const View *view) noexcept {
            return view == nullptr || view->texture->descriptor.sampleCount == descriptor.sampleCount;
        }

        [[nodiscard]] static bool AttachmentAspectsMatch(const View *color, const View *depth) noexcept {
            return (color == nullptr || color->aspect == RenderTextureAspect::Color) &&
                   (depth == nullptr || depth->aspect != RenderTextureAspect::Color);
        }

        [[nodiscard]] static bool IsTargetValid(const RenderTargetDescriptor &descriptor, const std::uint64_t colorIdentity,
                                                const View *color, const std::uint64_t depthIdentity, const View *depth) noexcept {
            return descriptor.IsValid() && AttachmentsKnown(colorIdentity, color, depthIdentity, depth) &&
                   AttachmentExtentMatches(descriptor, color) && AttachmentExtentMatches(descriptor, depth) &&
                   AttachmentSamplesMatch(descriptor, color) && AttachmentSamplesMatch(descriptor, depth) &&
                   AttachmentAspectsMatch(color, depth);
        }

        Result<VulkanMaterialResource> ResolveMaterialBuffer(const MaterialResidentResource &) const;
        Result<VulkanMaterialResource> ResolveMaterialView(const MaterialResidentResource &) const;

        void RemoveTargetsReferencing(const View *view) {
            std::erase_if(targets, [view](const auto &entry) {
                return entry.second->color.get() == view || entry.second->depth.get() == view;
            });
        }

        void RemoveViewsReferencing(const Texture *texture) {
            std::erase_if(views, [this, texture](const auto &entry) {
                if (entry.second->texture.get() != texture)
                    return false;
                RemoveTargetsReferencing(entry.second.get());
                return true;
            });
        }

        [[nodiscard]] static bool ViewDescriptorMatches(const RenderTextureDescriptor &texture,
                                                        const RenderTextureViewDescriptor &view) noexcept {
            return VulkanResourceConversion::ViewDimensionMatches(texture, view);
        }

        [[nodiscard]] std::optional<std::uint32_t> MemoryType(const std::uint32_t bits, const VkMemoryPropertyFlags flags) const {
            for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
                if ((bits & (1U << index)) != 0 && (memoryProperties.memoryTypes[index].propertyFlags & flags) == flags)
                    return index;
            return std::nullopt;
        }
    };

    /** @copydoc VulkanResourceRuntime::VulkanResourceRuntime */
    VulkanResourceRuntime::VulkanResourceRuntime(const std::uint64_t maximumIdentity) : impl_(std::make_unique<Impl>(maximumIdentity)) {}

    VulkanResourceRuntime::~VulkanResourceRuntime() {
        ShutdownResources();
    }

    Result<void> VulkanResourceRuntime::Initialize(const VkPhysicalDeviceMemoryProperties &memoryProperties, const VkDevice device,
                                                   const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver) {
        if (impl_->device != VK_NULL_HANDLE || device == VK_NULL_HANDLE || !resolver)
            return Result<void>::Failure(
                ResourceError(VulkanBackendErrors::ResourceRequestInvalid, "Invalid Vulkan resource runtime initialization."));
        impl_->device = device;
        impl_->memoryProperties = memoryProperties;
#define HORO_VK_LOAD(member, type, name) impl_->dispatch.member = std::bit_cast<type>(resolver(device, name))
        HORO_VK_LOAD(createBuffer, PFN_vkCreateBuffer, "vkCreateBuffer");
        HORO_VK_LOAD(destroyBuffer, PFN_vkDestroyBuffer, "vkDestroyBuffer");
        HORO_VK_LOAD(getBufferRequirements, PFN_vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
        HORO_VK_LOAD(createImage, PFN_vkCreateImage, "vkCreateImage");
        HORO_VK_LOAD(destroyImage, PFN_vkDestroyImage, "vkDestroyImage");
        HORO_VK_LOAD(getImageRequirements, PFN_vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements");
        HORO_VK_LOAD(allocateMemory, PFN_vkAllocateMemory, "vkAllocateMemory");
        HORO_VK_LOAD(freeMemory, PFN_vkFreeMemory, "vkFreeMemory");
        HORO_VK_LOAD(bindBufferMemory, PFN_vkBindBufferMemory, "vkBindBufferMemory");
        HORO_VK_LOAD(bindImageMemory, PFN_vkBindImageMemory, "vkBindImageMemory");
        HORO_VK_LOAD(mapMemory, PFN_vkMapMemory, "vkMapMemory");
        HORO_VK_LOAD(unmapMemory, PFN_vkUnmapMemory, "vkUnmapMemory");
        HORO_VK_LOAD(createImageView, PFN_vkCreateImageView, "vkCreateImageView");
        HORO_VK_LOAD(destroyImageView, PFN_vkDestroyImageView, "vkDestroyImageView");
#undef HORO_VK_LOAD
        if (!impl_->DispatchComplete()) {
            impl_->device = VK_NULL_HANDLE;
            impl_->memoryProperties = {};
            return Result<void>::Failure(ResourceError(VulkanBackendErrors::EntryPointMissing, "Vulkan resource dispatch is incomplete."));
        }
        return Result<void>::Success();
    }

#include "VulkanResourceShaderDelegation.inl"

    Result<RenderMemoryCostPlan> VulkanResourceRuntime::QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const {
        if (impl_->device == VK_NULL_HANDLE || !descriptor.IsValid())
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(VulkanBackendErrors::ResourceRequestInvalid,
                              "Vulkan buffer memory-cost query failed: " + DescribeRenderBufferRequest(descriptor)));
        const VkBufferCreateInfo info = BufferCreateInfo(descriptor);
        VkBuffer buffer{VK_NULL_HANDLE};
        if (impl_->dispatch.createBuffer(impl_->device, &info, nullptr, &buffer) != VK_SUCCESS)
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Buffer requirement probe failed."));
        VkMemoryRequirements requirements{};
        impl_->dispatch.getBufferRequirements(impl_->device, buffer, &requirements);
        impl_->dispatch.destroyBuffer(impl_->device, buffer, nullptr);
        if (!VulkanResourceConversion::RequirementsValid(requirements, descriptor.byteSize))
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(VulkanBackendErrors::ResourceCreationFailed, "The Vulkan buffer requirements are invalid."));
        const RenderMemoryClass memoryClass =
            descriptor.access == RenderBufferAccess::HostVisible ? RenderMemoryClass::Upload : RenderMemoryClass::PersistentDevice;
        return Result<RenderMemoryCostPlan>::Success(
            {.memoryClass = memoryClass,
             .allocationClass = RenderMemoryAllocationClass::Dedicated,
             .provenance = RenderMemoryCostProvenance::Exact,
             .compatibility = VulkanResourceConversion::Compatibility(requirements.memoryTypeBits, memoryClass),
             .payloadBytes = descriptor.byteSize,
             .requiredBytes = requirements.size,
             .alignment = requirements.alignment});
    }

    Result<RenderMemoryCostPlan> VulkanResourceRuntime::QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const {
        const auto payload = RenderTextureBaseLevelByteSize(descriptor);
        if (impl_->device == VK_NULL_HANDLE)
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(VulkanBackendErrors::ResourceUnsupported, "Vulkan texture memory-cost query failed: device is unavailable; " +
                                                                            DescribeRenderTextureRequest(descriptor)));
        if (!payload.has_value())
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(VulkanBackendErrors::ResourceUnsupported,
                              "Vulkan texture memory-cost query failed: base-level size is invalid; " +
                                  DescribeRenderTextureRequest(descriptor)));
        if (VulkanResourceConversion::TextureFormat(descriptor.format) == VK_FORMAT_UNDEFINED)
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(VulkanBackendErrors::ResourceUnsupported, "Vulkan texture memory-cost query failed: format is unavailable; " +
                                                                            DescribeRenderTextureRequest(descriptor)));
        const VkImageCreateInfo info = VulkanResourceConversion::ImageCreateInfo(descriptor);
        VkImage image{VK_NULL_HANDLE};
        if (impl_->dispatch.createImage(impl_->device, &info, nullptr, &image) != VK_SUCCESS)
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Texture requirement probe failed."));
        VkMemoryRequirements requirements{};
        impl_->dispatch.getImageRequirements(impl_->device, image, &requirements);
        impl_->dispatch.destroyImage(impl_->device, image, nullptr);
        if (!VulkanResourceConversion::RequirementsValid(requirements, *payload))
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(VulkanBackendErrors::ResourceCreationFailed, "The Vulkan texture requirements are invalid."));
        const RenderMemoryCostPlan cost{.memoryClass = RenderMemoryClass::PersistentDevice,
                                        .allocationClass = RenderMemoryAllocationClass::Dedicated,
                                        .provenance = RenderMemoryCostProvenance::Exact,
                                        .compatibility = VulkanResourceConversion::Compatibility(requirements.memoryTypeBits,
                                                                                                 RenderMemoryClass::PersistentDevice),
                                        .payloadBytes = *payload,
                                        .requiredBytes = requirements.size,
                                        .alignment = requirements.alignment};
        return Result<RenderMemoryCostPlan>::Success(cost);
    }

    Result<std::uint64_t> VulkanResourceRuntime::CreateBuffer(const RenderBufferDescriptor &descriptor,
                                                              const std::span<const std::byte> initialData,
                                                              const RenderMemoryPlacement &placement,
                                                              RenderResourceNativeLifetime lifetime) {
        const auto plan = QueryBufferMemoryCost(descriptor);
        if (plan.HasError())
            return Result<std::uint64_t>::Failure(WrapError(VulkanBackendErrors::ResourceRequestInvalid, plan.ErrorValue(),
                                                            "Vulkan buffer creation failed: " + DescribeRenderBufferRequest(descriptor)));
        if (!IsBufferRealizationValid(plan.Value(), descriptor, initialData, placement))
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceRequestInvalid,
                              "Vulkan buffer creation failed: initial data or placement is incompatible with " +
                                  DescribeRenderBufferRequest(descriptor)));
        const VkBufferCreateInfo info = BufferCreateInfo(descriptor);
        auto instance = Impl::Allocate<Impl::Buffer>();
        if (instance == nullptr)
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Buffer identity allocation failed."));
        instance->lifetime = std::move(lifetime);
        instance->device = impl_->device;
        instance->destroy = impl_->dispatch.destroyBuffer;
        instance->freeMemory = impl_->dispatch.freeMemory;
        instance->descriptor = descriptor;
        if (impl_->dispatch.createBuffer(impl_->device, &info, nullptr, &instance->handle) != VK_SUCCESS) {
            instance->handle = VK_NULL_HANDLE;
            return Result<std::uint64_t>::Failure(ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Buffer creation failed."));
        }
        VkMemoryRequirements requirements{};
        impl_->dispatch.getBufferRequirements(impl_->device, instance->handle, &requirements);
        if (!Impl::RequirementsStillMatch(requirements, plan.Value()))
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Buffer requirements changed during realization."));
        const VkMemoryPropertyFlags flags = descriptor.access == RenderBufferAccess::HostVisible
                                                ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (const Impl::AllocationFailure allocation =
                impl_->AllocateAndBind(*instance, requirements, flags, impl_->dispatch.bindBufferMemory);
            allocation != Impl::AllocationFailure::None) {
            return Result<std::uint64_t>::Failure(Impl::AllocationError(allocation, "Buffer"));
        }
        if (!impl_->Upload(*instance, initialData))
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Buffer upload mapping failed."));
        instance->graphState.contentsDefined = !initialData.empty();
        return impl_->Register(impl_->buffers, std::move(instance));
    }

    Result<std::uint64_t> VulkanResourceRuntime::CreateMesh(const RenderMeshDescriptor &descriptor, const std::uint64_t vertexBuffer,
                                                            const std::uint64_t indexBuffer, RenderResourceNativeLifetime lifetime) {
        const auto *vertex = Impl::Find(impl_->buffers, vertexBuffer);
        if (const auto *index = Impl::Find(impl_->buffers, indexBuffer);
            !descriptor.IsValid() || vertex == nullptr || index == nullptr ||
            !HasBufferUsage(vertex->descriptor.usage, RenderBufferUsage::Vertex) ||
            !HasBufferUsage(index->descriptor.usage, RenderBufferUsage::Index))
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceIdentityInvalid, "Mesh buffer identity is invalid."));
        auto mesh = Impl::Allocate<Impl::Mesh>();
        if (mesh == nullptr)
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Mesh identity allocation failed."));
        mesh->lifetime = std::move(lifetime);
        mesh->vertex = vertexBuffer;
        mesh->index = indexBuffer;
        mesh->descriptor = descriptor;
        return impl_->Register(impl_->meshes, std::move(mesh));
    }

    Result<std::uint64_t> VulkanResourceRuntime::CreateTexture(const RenderTextureDescriptor &descriptor,
                                                               const std::span<const std::byte> initialData,
                                                               const RenderMemoryPlacement &placement,
                                                               RenderResourceNativeLifetime lifetime) {
        const auto plan = QueryTextureMemoryCost(descriptor);
        if (plan.HasError())
            return Result<std::uint64_t>::Failure(WrapError(VulkanBackendErrors::ResourceRequestInvalid, plan.ErrorValue(),
                                                            "Vulkan texture creation failed: " + DescribeRenderTextureRequest(descriptor)));
        if (!VulkanResourceConversion::PlacementMatches(plan.Value(), placement))
            return Result<std::uint64_t>::Failure(ResourceError(VulkanBackendErrors::ResourceRequestInvalid,
                                                                "Vulkan texture creation failed: placement is incompatible with " +
                                                                    DescribeRenderTextureRequest(descriptor)));
        if (!initialData.empty())
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceUnsupported, "Texture uploads require the Vulkan transfer stage."));
        const VkImageCreateInfo info = VulkanResourceConversion::ImageCreateInfo(descriptor);
        auto texture = Impl::Allocate<Impl::Texture>();
        if (texture == nullptr)
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Texture identity allocation failed."));
        texture->lifetime = std::move(lifetime);
        texture->device = impl_->device;
        texture->destroy = impl_->dispatch.destroyImage;
        texture->freeMemory = impl_->dispatch.freeMemory;
        texture->descriptor = descriptor;
        texture->destroyView = impl_->dispatch.destroyImageView;
        if (impl_->dispatch.createImage(impl_->device, &info, nullptr, &texture->handle) != VK_SUCCESS) {
            texture->handle = VK_NULL_HANDLE;
            return Result<std::uint64_t>::Failure(ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Texture creation failed."));
        }
        VkMemoryRequirements requirements{};
        impl_->dispatch.getImageRequirements(impl_->device, texture->handle, &requirements);
        if (!Impl::RequirementsStillMatch(requirements, plan.Value())) {
            const Error changedRequirements =
                ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Texture requirements changed during realization.");
            return Result<std::uint64_t>::Failure(changedRequirements);
        }
        if (const Impl::AllocationFailure allocation =
                impl_->AllocateAndBind(*texture, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, impl_->dispatch.bindImageMemory);
            allocation != Impl::AllocationFailure::None) {
            return Result<std::uint64_t>::Failure(Impl::AllocationError(allocation, "Texture"));
        }
        if (descriptor.dimension == RenderTextureDimension::TwoD && descriptor.depth == 1 && descriptor.mipCount == 1 &&
            descriptor.layerCount == 1 && descriptor.sampleCount == 1 && descriptor.format == RenderTextureFormat::Rgba8Unorm &&
            HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment)) {
            const VkImageAspectFlags colorAspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                             .image = texture->handle,
                                             .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                             .format = VulkanResourceConversion::TextureFormat(descriptor.format),
                                             .subresourceRange = {colorAspectMask, 0, 1, 0, 1}};
            if (impl_->dispatch.createImageView(impl_->device, &view, nullptr, &texture->colorView) != VK_SUCCESS)
                return Result<std::uint64_t>::Failure(
                    ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Preparing a native graph color attachment view failed."));
        }
        return impl_->Register(impl_->textures, std::move(texture));
    }

#include "VulkanExternalImageRegistration.inl"

    Result<std::uint64_t> VulkanResourceRuntime::CreateTextureView(const RenderTextureViewDescriptor &descriptor,
                                                                   const std::uint64_t textureIdentity,
                                                                   RenderResourceNativeLifetime lifetime) {
        auto *texture = Impl::Find(impl_->textures, textureIdentity);
        if (!descriptor.IsValid() || texture == nullptr)
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceIdentityInvalid, "Texture view source identity is invalid."));
        if (!Impl::ViewDescriptorMatches(texture->descriptor, descriptor))
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceIdentityInvalid, "Texture view source or range is invalid."));
        const VkImageViewCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .image = texture->handle,
                                         .viewType = VulkanResourceConversion::ViewType(descriptor.dimension),
                                         .format = VulkanResourceConversion::TextureFormat(descriptor.format),
                                         .subresourceRange = {VulkanResourceConversion::Aspect(descriptor.aspect), descriptor.baseMip,
                                                              descriptor.mipCount, descriptor.baseLayer, descriptor.layerCount}};
        auto view = Impl::Allocate<Impl::View>();
        if (view == nullptr)
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Texture view identity allocation failed."));
        view->lifetime = std::move(lifetime);
        view->device = impl_->device;
        view->destroy = impl_->dispatch.destroyImageView;
        view->texture = impl_->textures.at(textureIdentity);
        view->aspect = descriptor.aspect;
        view->descriptor = descriptor;
        if (impl_->dispatch.createImageView(impl_->device, &info, nullptr, &view->handle) != VK_SUCCESS) {
            view->handle = VK_NULL_HANDLE;
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Texture view creation failed."));
        }
        return impl_->Register(impl_->views, std::move(view));
    }

    Result<std::uint64_t> VulkanResourceRuntime::CreateRenderTarget(const RenderTargetDescriptor &descriptor,
                                                                    const std::uint64_t colorIdentity, const std::uint64_t depthIdentity,
                                                                    RenderResourceNativeLifetime lifetime) {
        auto *color = Impl::Find(impl_->views, colorIdentity);
        auto *depth = Impl::Find(impl_->views, depthIdentity);
        if (!Impl::IsTargetValid(descriptor, colorIdentity, color, depthIdentity, depth))
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceIdentityInvalid, "Render target attachment identity is invalid."));
        auto target = Impl::Allocate<Impl::Target>();
        if (target == nullptr)
            return Result<std::uint64_t>::Failure(
                ResourceError(VulkanBackendErrors::ResourceCreationFailed, "Render target identity allocation failed."));
        target->lifetime = std::move(lifetime);
        if (color)
            target->color = impl_->views.at(colorIdentity);
        if (depth)
            target->depth = impl_->views.at(depthIdentity);
        target->extent = descriptor.extent;
        return impl_->Register(impl_->targets, std::move(target));
    }

    void VulkanResourceRuntime::DestroyBuffer(const std::uint64_t id) noexcept {
        if (Impl::Find(impl_->buffers, id) != nullptr)
            std::erase_if(impl_->meshes, [id](const auto &entry) {
                return entry.second->vertex == id || entry.second->index == id;
            });
        impl_->buffers.erase(id);
    }

    void VulkanResourceRuntime::DestroyMesh(const std::uint64_t id) noexcept {
        impl_->meshes.erase(id);
    }

    void VulkanResourceRuntime::DestroyTexture(const std::uint64_t id) noexcept {
        if (const Impl::Texture *texture = Impl::Find(impl_->textures, id); texture != nullptr)
            impl_->RemoveViewsReferencing(texture);
        impl_->textures.erase(id);
    }

    void VulkanResourceRuntime::DestroyTextureView(const std::uint64_t id) noexcept {
        if (const Impl::View *view = Impl::Find(impl_->views, id); view != nullptr)
            impl_->RemoveTargetsReferencing(view);
        impl_->views.erase(id);
    }

    void VulkanResourceRuntime::DestroyRenderTarget(const std::uint64_t id) noexcept {
        impl_->targets.erase(id);
    }

    void VulkanResourceRuntime::ShutdownResources() noexcept {
        shaders_.Shutdown();
        impl_->targets.clear();
        impl_->views.clear();
        impl_->meshes.clear();
        impl_->textures.clear();
        impl_->buffers.clear();
        impl_->device = VK_NULL_HANDLE;
    }

#include "VulkanMaterialCapture.inl"

    /** @copydoc VulkanResourceRuntime::CaptureGraphResources */
    Result<void> VulkanResourceRuntime::CaptureGraphResources(const RenderGraphExecutionRequest &request,
                                                              const std::span<VulkanGraphResource> destination,
                                                              VulkanCommandRetention &retention) const {
        if (impl_->device == VK_NULL_HANDLE || destination.size() < request.resources.size() || retention.references != 0 ||
            retention.lease != nullptr || retention.resources.capacity() < request.resources.size())
            return Result<void>::Failure(
                ResourceError(VulkanBackendErrors::InvalidCommandState, "Graph capture requires prepared free device-owned storage."));
        retention.resources.clear();
        for (std::size_t index = 0; index < request.resources.size(); ++index) {
            const auto &resource = request.graph.Resources()[index];
            auto &captured = destination[index];
            captured = VulkanGraphResource{.kind = resource.kind};
            if (resource.resourceClass == RenderGraphResourceClass::Transient)
                return Result<void>::Failure(ResourceError(VulkanBackendErrors::ResourceUnsupported,
                                                           "Vulkan placed/transient reuse is not admitted by this resource owner."));
            const auto identity = request.resources[index].instance;
            if (resource.kind == RenderGraphResourceKind::Buffer) {
                const auto found = impl_->buffers.find(identity);
                if (found == impl_->buffers.end())
                    return Result<void>::Failure(
                        ResourceError(VulkanBackendErrors::ResourceIdentityInvalid, "A graph buffer identity is foreign or retired."));
                captured.buffer = found->second->handle;
                captured.bufferDescriptor = found->second->descriptor;
                captured.state = &found->second->graphState;
                retention.resources.push_back(found->second);
            } else {
                const auto found = impl_->textures.find(identity);
                if (found == impl_->textures.end())
                    return Result<void>::Failure(
                        ResourceError(VulkanBackendErrors::ResourceIdentityInvalid, "A graph texture identity is foreign or retired."));
                captured.image = found->second->handle;
                captured.colorView = found->second->colorView;
                captured.textureDescriptor = found->second->descriptor;
                captured.state = &found->second->graphState;
                retention.resources.push_back(found->second);
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
