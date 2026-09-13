#include "MetalResourceRuntime.h"

#include "MetalRenderBackendErrors.h"
#include "MetalResourceInstances.h"

#import <Metal/Metal.h>
#include <limits>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        [[nodiscard]] Error ResourceError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] MTLPixelFormat PixelFormat(const RenderTextureFormat format) noexcept {
            using enum RenderTextureFormat;
            switch (format) {
                case Rgba8Unorm:
                    return MTLPixelFormatRGBA8Unorm;
                case Depth24Stencil8:
                    return MTLPixelFormatDepth24Unorm_Stencil8;
                case Depth32Float:
                    return MTLPixelFormatDepth32Float;
                case R8Unorm:
                case Rg8Unorm:
                case Rgba8UnormSrgb:
                case Bgra8Unorm:
                case Bgra8UnormSrgb:
                case R16Float:
                case Rg16Float:
                case Rgba16Float:
                case R32Float:
                case Rg32Float:
                case Rgba32Float:
                case Depth16Unorm:
                case Depth32FloatStencil8:
                    return MTLPixelFormatInvalid;
            }
            return MTLPixelFormatInvalid;
        }

        [[nodiscard]] MTLTextureUsage TextureUsage(const RenderTextureUsage usage) noexcept {
            MTLTextureUsage nativeUsage = MTLTextureUsageUnknown;
            if (HasTextureUsage(usage, RenderTextureUsage::Sampled))
                nativeUsage |= MTLTextureUsageShaderRead;
            if (HasTextureUsage(usage, RenderTextureUsage::RenderAttachment))
                nativeUsage |= MTLTextureUsageRenderTarget;
            return nativeUsage;
        }

        [[nodiscard]] MTLStorageMode TextureStorageMode(const RenderTextureDescriptor &descriptor) noexcept {
            return HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment) ? MTLStorageModePrivate : MTLStorageModeShared;
        }

        [[nodiscard]] MTLTextureDescriptor *NativeTextureDescriptor(const RenderTextureDescriptor &descriptor) noexcept {
            MTLTextureDescriptor *native = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:PixelFormat(descriptor.format)
                                                                                              width:descriptor.extent.width
                                                                                             height:descriptor.extent.height
                                                                                          mipmapped:NO];
            native.usage = TextureUsage(descriptor.usage);
            native.storageMode = TextureStorageMode(descriptor);
            return native;
        }

        [[nodiscard]] bool MatchesPlacement(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) noexcept {
            return placement.IsValid() && placement.memoryClass == plan.memoryClass && placement.allocationClass == plan.allocationClass &&
                   placement.provenance == plan.provenance && placement.compatibility == plan.compatibility &&
                   placement.payloadBytes == plan.payloadBytes && placement.requiredBytes == plan.requiredBytes &&
                   placement.offsetBytes == 0;
        }

        template <typename Instance> [[nodiscard]] Instance *Decode(const std::uint64_t identity) noexcept {
            if (identity == 0 || identity > std::numeric_limits<std::uintptr_t>::max())
                return nullptr;
            return reinterpret_cast<Instance *>(static_cast<std::uintptr_t>(identity));
        }

        template <typename Instance> [[nodiscard]] std::uint64_t Identity(Instance *instance) noexcept {
            return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(instance));
        }

        template <typename Instance> void DestroyTracked(std::unordered_set<Instance *> &instances, const std::uint64_t identity) noexcept {
            Instance *instance = Decode<Instance>(identity);
            if (instance != nullptr && instances.erase(instance) > 0)
                delete instance;
        }

        template <typename Instance> void DestroyAll(std::unordered_set<Instance *> &instances) noexcept {
            for (Instance *instance : instances)
                delete instance;
            instances.clear();
        }

        [[nodiscard]] bool AspectMatches(const RenderTextureFormat format, const RenderTextureAspect aspect) noexcept {
            if (format == RenderTextureFormat::Rgba8Unorm)
                return aspect == RenderTextureAspect::Color;
            if (format == RenderTextureFormat::Depth24Stencil8)
                return aspect == RenderTextureAspect::Depth || aspect == RenderTextureAspect::DepthStencil;
            return format == RenderTextureFormat::Depth32Float && aspect == RenderTextureAspect::Depth;
        }

        enum class AttachmentKind : std::uint8_t {
            Color,
            Depth,
        };

        [[nodiscard]] Result<MetalTextureViewInstance *> ResolveAttachment(const std::unordered_set<MetalTextureViewInstance *> &instances,
                                                                           const std::uint64_t identity, const AttachmentKind kind) {
            if (identity == 0)
                return Result<MetalTextureViewInstance *>::Success(nullptr);
            MetalTextureViewInstance *instance = Decode<MetalTextureViewInstance>(identity);
            const bool aspectMatches =
                instance != nullptr && (kind == AttachmentKind::Color ? instance->aspect == RenderTextureAspect::Color
                                                                      : instance->aspect != RenderTextureAspect::Color);
            if (!aspectMatches || !instances.contains(instance)) {
                return Result<MetalTextureViewInstance *>::Failure(
                    ResourceError(MetalBackendErrors::ResourceIdentityInvalid, "Metal render target attachment is invalid."));
            }
            return Result<MetalTextureViewInstance *>::Success(instance);
        }

        [[nodiscard]] bool ExtentMatches(const MetalTextureViewInstance *attachment, const FramebufferExtent extent) noexcept {
            return attachment == nullptr || (attachment->texture.width == extent.width && attachment->texture.height == extent.height);
        }
    }  // namespace

    struct MetalResourceRuntime::Impl {
        __strong id<MTLDevice> device{nil};
        std::unordered_set<MetalBufferInstance *> buffers;
        std::unordered_set<MetalMeshInstance *> meshes;
        std::unordered_set<MetalTextureInstance *> textures;
        std::unordered_set<MetalTextureViewInstance *> textureViews;
        std::unordered_set<MetalRenderTargetInstance *> renderTargets;
    };

    MetalResourceRuntime::MetalResourceRuntime() : impl_(std::make_unique<Impl>()) {}

    MetalResourceRuntime::~MetalResourceRuntime() {
        Shutdown();
    }

    void MetalResourceRuntime::Initialize(void *device) noexcept {
        impl_->device = (__bridge id<MTLDevice>)device;
    }

    Result<RenderMemoryCostPlan> MetalResourceRuntime::QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const {
        if (impl_->device == nil || !descriptor.IsValid())
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(MetalBackendErrors::InvalidConfig, "Metal buffer memory requirement request is invalid."));
        const MTLSizeAndAlign native = [impl_->device heapBufferSizeAndAlignWithLength:descriptor.byteSize
                                                                               options:MTLResourceStorageModeShared];
        if (native.size < descriptor.byteSize || native.align == 0)
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal returned invalid buffer memory requirements."));
        return Result<RenderMemoryCostPlan>::Success({.memoryClass = RenderMemoryClass::PersistentDevice,
                                                      .allocationClass = RenderMemoryAllocationClass::Dedicated,
                                                      .provenance = RenderMemoryCostProvenance::Exact,
                                                      .compatibility = RenderMemoryCompatibilityId{1},
                                                      .payloadBytes = descriptor.byteSize,
                                                      .requiredBytes = native.size,
                                                      .alignment = native.align});
    }

    Result<RenderMemoryCostPlan> MetalResourceRuntime::QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const {
        const auto payload = RenderTextureBaseLevelByteSize(descriptor);
        if (impl_->device == nil || !payload.has_value() || PixelFormat(descriptor.format) == MTLPixelFormatInvalid)
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(MetalBackendErrors::InvalidConfig, "Metal texture memory requirement request is invalid."));
        MTLTextureDescriptor *nativeDescriptor = NativeTextureDescriptor(descriptor);
        const MTLSizeAndAlign native = [impl_->device heapTextureSizeAndAlignWithDescriptor:nativeDescriptor];
        if (native.size < *payload || native.align == 0)
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal returned invalid texture memory requirements."));
        const std::uint64_t compatibility = TextureStorageMode(descriptor) == MTLStorageModePrivate ? 3 : 2;
        return Result<RenderMemoryCostPlan>::Success({.memoryClass = RenderMemoryClass::PersistentDevice,
                                                      .allocationClass = RenderMemoryAllocationClass::Dedicated,
                                                      .provenance = RenderMemoryCostProvenance::Exact,
                                                      .compatibility = RenderMemoryCompatibilityId{compatibility},
                                                      .payloadBytes = *payload,
                                                      .requiredBytes = native.size,
                                                      .alignment = native.align});
    }

    Result<std::uint64_t> MetalResourceRuntime::CreateBuffer(const RenderBufferDescriptor &descriptor,
                                                             const std::span<const std::byte> initialData,
                                                             const RenderMemoryPlacement &placement) {
        const auto cost = QueryBufferMemoryCost(descriptor);
        if (impl_->device == nil || !descriptor.IsValid() || (!initialData.empty() && initialData.size() != descriptor.byteSize) ||
            cost.HasError() || !MatchesPlacement(cost.Value(), placement))
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::InvalidConfig, "Metal buffer creation request is invalid."));
        id<MTLBuffer> buffer = initialData.empty()
                                   ? [impl_->device newBufferWithLength:descriptor.byteSize options:MTLResourceStorageModeShared]
                                   : [impl_->device newBufferWithBytes:initialData.data()
                                                                length:initialData.size()
                                                               options:MTLResourceStorageModeShared];
        if (buffer == nil)
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal failed to allocate a resident buffer."));
        auto *instance = new (std::nothrow) MetalBufferInstance{.buffer = buffer};
        if (instance == nullptr)
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal buffer identity allocation failed."));
        impl_->buffers.insert(instance);
        return Result<std::uint64_t>::Success(Identity(instance));
    }

    Result<std::uint64_t> MetalResourceRuntime::CreateMesh(const RenderMeshDescriptor &descriptor, const std::uint64_t vertexBuffer,
                                                           const std::uint64_t indexBuffer) {
        MetalBufferInstance *vertex = Decode<MetalBufferInstance>(vertexBuffer);
        MetalBufferInstance *index = Decode<MetalBufferInstance>(indexBuffer);
        if (!descriptor.IsValid() || vertex == nullptr || index == nullptr || !impl_->buffers.contains(vertex) ||
            !impl_->buffers.contains(index)) {
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceIdentityInvalid, "Metal mesh references unknown buffer instances."));
        }
        auto *instance = new (std::nothrow) MetalMeshInstance{.vertexBuffer = vertex->buffer, .indexBuffer = index->buffer};
        if (instance == nullptr)
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal mesh identity allocation failed."));
        impl_->meshes.insert(instance);
        return Result<std::uint64_t>::Success(Identity(instance));
    }

    Result<std::uint64_t> MetalResourceRuntime::CreateTexture(const RenderTextureDescriptor &descriptor,
                                                              const std::span<const std::byte> initialData,
                                                              const RenderMemoryPlacement &placement) {
        const auto payload = RenderTextureBaseLevelByteSize(descriptor);
        const auto cost = QueryTextureMemoryCost(descriptor);
        const bool initialDataValid = payload.has_value() && (initialData.empty() || initialData.size() == *payload) &&
                                      (initialData.empty() || TextureStorageMode(descriptor) == MTLStorageModeShared);
        if (impl_->device == nil || !descriptor.IsValid() || !initialDataValid || cost.HasError() ||
            !MatchesPlacement(cost.Value(), placement))
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::InvalidConfig, "Metal texture creation request is invalid."));
        MTLTextureDescriptor *native = NativeTextureDescriptor(descriptor);
        id<MTLTexture> texture = [impl_->device newTextureWithDescriptor:native];
        if (texture == nil)
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal failed to allocate a resident texture."));
        if (!initialData.empty()) {
            const std::size_t texelBytes = *RenderTextureTexelBytes(descriptor.format);
            [texture replaceRegion:MTLRegionMake2D(0, 0, descriptor.extent.width, descriptor.extent.height)
                       mipmapLevel:0
                         withBytes:initialData.data()
                       bytesPerRow:static_cast<NSUInteger>(descriptor.extent.width) * texelBytes];
        }
        auto *instance = new (std::nothrow) MetalTextureInstance{.texture = texture, .format = descriptor.format};
        if (instance == nullptr)
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal texture identity allocation failed."));
        impl_->textures.insert(instance);
        return Result<std::uint64_t>::Success(Identity(instance));
    }

    Result<std::uint64_t> MetalResourceRuntime::CreateTextureView(const RenderTextureViewDescriptor &descriptor,
                                                                  const std::uint64_t texture) {
        MetalTextureInstance *source = Decode<MetalTextureInstance>(texture);
        if (!descriptor.IsValid() || source == nullptr || !impl_->textures.contains(source) || source->format != descriptor.format ||
            !AspectMatches(descriptor.format, descriptor.aspect)) {
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceIdentityInvalid, "Metal texture view source or aspect is invalid."));
        }
        id<MTLTexture> view = [source->texture newTextureViewWithPixelFormat:PixelFormat(descriptor.format)];
        if (view == nil)
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal failed to create a resident texture view."));
        auto *instance =
            new (std::nothrow) MetalTextureViewInstance{.texture = view, .format = descriptor.format, .aspect = descriptor.aspect};
        if (instance == nullptr)
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal texture-view identity allocation failed."));
        impl_->textureViews.insert(instance);
        return Result<std::uint64_t>::Success(Identity(instance));
    }

    Result<std::uint64_t> MetalResourceRuntime::CreateRenderTarget(const RenderTargetDescriptor &descriptor,
                                                                   const std::uint64_t colorAttachment,
                                                                   const std::uint64_t depthAttachment) {
        if (!descriptor.IsValid())
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::InvalidConfig, "Metal render target descriptor is invalid."));
        const auto color = ResolveAttachment(impl_->textureViews, colorAttachment, AttachmentKind::Color);
        if (color.HasError())
            return Result<std::uint64_t>::Failure(color.ErrorValue());
        const auto depth = ResolveAttachment(impl_->textureViews, depthAttachment, AttachmentKind::Depth);
        if (depth.HasError())
            return Result<std::uint64_t>::Failure(depth.ErrorValue());
        if (!ExtentMatches(color.Value(), descriptor.extent) || !ExtentMatches(depth.Value(), descriptor.extent))
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::InvalidExtent, "Metal render target attachment extent does not match its descriptor."));
        auto *instance = new (std::nothrow) MetalRenderTargetInstance{
            .colorTexture = color.Value() == nullptr ? nil : color.Value()->texture,
            .depthTexture = depth.Value() == nullptr ? nil : depth.Value()->texture,
        };
        if (instance == nullptr)
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal render-target identity allocation failed."));
        impl_->renderTargets.insert(instance);
        return Result<std::uint64_t>::Success(Identity(instance));
    }

    void MetalResourceRuntime::DestroyBuffer(const std::uint64_t backendInstance) noexcept {
        DestroyTracked(impl_->buffers, backendInstance);
    }

    void MetalResourceRuntime::DestroyMesh(const std::uint64_t backendInstance) noexcept {
        DestroyTracked(impl_->meshes, backendInstance);
    }

    void MetalResourceRuntime::DestroyTexture(const std::uint64_t backendInstance) noexcept {
        DestroyTracked(impl_->textures, backendInstance);
    }

    void MetalResourceRuntime::DestroyTextureView(const std::uint64_t backendInstance) noexcept {
        DestroyTracked(impl_->textureViews, backendInstance);
    }

    void MetalResourceRuntime::DestroyRenderTarget(const std::uint64_t backendInstance) noexcept {
        DestroyTracked(impl_->renderTargets, backendInstance);
    }

    void MetalResourceRuntime::Shutdown() noexcept {
        DestroyAll(impl_->renderTargets);
        DestroyAll(impl_->textureViews);
        DestroyAll(impl_->meshes);
        DestroyAll(impl_->textures);
        DestroyAll(impl_->buffers);
        impl_->device = nil;
    }
}  // namespace Horo::Render::Detail
