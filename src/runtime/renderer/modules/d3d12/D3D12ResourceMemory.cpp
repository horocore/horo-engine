#include "D3D12ResourceInternals.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12Detail {

    /** @copydoc QueryAllocationInfo */
    Result<D3D12_RESOURCE_ALLOCATION_INFO> QueryAllocationInfo(ID3D12Device &device, const D3D12_RESOURCE_DESC &shape) {
        const auto info = device.GetResourceAllocationInfo(0, 1, &shape);
        if (info.SizeInBytes == std::numeric_limits<UINT64>::max() || info.SizeInBytes == 0 || info.Alignment == 0 ||
            (info.Alignment & (info.Alignment - 1U)) != 0) {
            const HRESULT removed = device.GetDeviceRemovedReason();
            if (FAILED(removed))
                return Result<D3D12_RESOURCE_ALLOCATION_INFO>::Failure(
                    ResourceError("render.d3d12.device_lost", "The device was removed during allocation requirements query.", removed));
            return Result<D3D12_RESOURCE_ALLOCATION_INFO>::Failure(
                ResourceError("render.d3d12.requirements_failed", "The device returned invalid resource size/alignment requirements."));
        }
        return Result<D3D12_RESOURCE_ALLOCATION_INFO>::Success(info);
    }

    namespace {
        /** @brief Separates native tier-1 heap classes and MSAA placement alignment. */
        MemoryCompatibility Compatibility(const D3D12_RESOURCE_DESC &shape, RenderMemoryClass memoryClass) {
            if (memoryClass == RenderMemoryClass::Upload)
                return MemoryCompatibility::UploadBuffer;
            if (shape.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                return MemoryCompatibility::DeviceBuffer;
            if ((shape.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) == 0)
                return MemoryCompatibility::Texture;
            return shape.SampleDesc.Count > 1 ? MemoryCompatibility::MultisampledAttachment : MemoryCompatibility::Attachment;
        }

        /** @brief Restricts native heaps to the compatible resource class admitted by the cost plan. */
        D3D12_HEAP_FLAGS HeapFlags(const D3D12_RESOURCE_DESC &shape) {
            if (shape.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                return D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            return (shape.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0
                       ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
                       : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        }

        /** @brief Verifies the caller's unchanged admitted requirements without granting additional capacity. */
        bool PlacementRequirementsMatch(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) {
            return placement.IsValid() && placement.memoryClass == plan.memoryClass && placement.compatibility == plan.compatibility &&
                   placement.allocationClass == plan.allocationClass && placement.provenance == plan.provenance &&
                   placement.payloadBytes == plan.payloadBytes && placement.requiredBytes == plan.requiredBytes &&
                   placement.offsetBytes % plan.alignment == 0 && placement.backingBytes % plan.alignment == 0;
        }

        /** @brief Rejects buffer policies which cannot be represented by upload heaps or native views. */
        bool BufferPolicySupported(const RenderBufferDescriptor &descriptor) {
            if (descriptor.access != RenderBufferAccess::HostVisible)
                return true;
            return !HasBufferUsage(descriptor.usage, RenderBufferUsage::Storage) &&
                   !HasBufferUsage(descriptor.usage, RenderBufferUsage::CopyDestination);
        }

        /** @brief Checks constant/raw view limits before allocating native storage. */
        bool BufferViewSizeSupported(const RenderBufferDescriptor &descriptor) {
            if (HasBufferUsage(descriptor.usage, RenderBufferUsage::Uniform))
                if (descriptor.byteSize > 65536 || descriptor.byteSize % 256 != 0)
                    return false;
            if (HasBufferUsage(descriptor.usage, RenderBufferUsage::Storage))
                if (descriptor.byteSize % 4 != 0 || descriptor.byteSize / 4 > std::numeric_limits<UINT>::max())
                    return false;
            return true;
        }

        /** @brief Builds the mandatory native format feature mask from canonical usage. */
        UINT RequiredFormatSupport(const RenderTextureDescriptor &descriptor) {
            UINT mask = descriptor.dimension == RenderTextureDimension::OneD     ? D3D12_FORMAT_SUPPORT1_TEXTURE1D
                        : descriptor.dimension == RenderTextureDimension::ThreeD ? D3D12_FORMAT_SUPPORT1_TEXTURE3D
                                                                                 : D3D12_FORMAT_SUPPORT1_TEXTURE2D;
            if (HasTextureUsage(descriptor.usage, RenderTextureUsage::Sampled) && !IsDepth(descriptor.format))
                mask |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
            if (HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment))
                mask |= IsDepth(descriptor.format) ? D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL : D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
            return mask;
        }

        /** @brief Checks format/usage and sample support on the exact device, retaining native query failures. */
        Result<void> ValidateTextureSupport(ID3D12Device &device, const RenderTextureDescriptor &descriptor) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{NativeFormat(descriptor.format)};
            HRESULT status = device.CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
            if (FAILED(status))
                return Result<void>::Failure(
                    ResourceError("render.d3d12.format_query_failed", "Query the selected device format.", status));
            const UINT mask = RequiredFormatSupport(descriptor);
            const bool missingStorage = HasTextureUsage(descriptor.usage, RenderTextureUsage::Storage) &&
                                        (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0;
            if ((support.Support1 & mask) != mask || missingStorage)
                return Result<void>::Failure(
                    ResourceError("render.d3d12.format_unsupported", "Select a format supporting every requested usage on this device."));
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS samples{NativeFormat(descriptor.format), descriptor.sampleCount,
                                                                  D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
            status = device.CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &samples, sizeof(samples));
            if (FAILED(status))
                return Result<void>::Failure(ResourceError("render.d3d12.samples_query_failed", "Query requested sample support.", status));
            if (samples.NumQualityLevels == 0)
                return Result<void>::Failure(ResourceError("render.d3d12.samples_unsupported", "Use a supported sample count."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc Resources::QueryBufferMemoryCost */
    Result<RenderMemoryCostPlan> Resources::QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const {
        if (auto checked = Check(); checked.HasError())
            return Result<RenderMemoryCostPlan>::Failure(checked.ErrorValue());
        if (!descriptor.IsValid())
            return Failure<RenderMemoryCostPlan>("render.d3d12.buffer_invalid", "Use a valid buffer size, access and usage.");
        if (!BufferPolicySupported(descriptor))
            return Failure<RenderMemoryCostPlan>("render.d3d12.buffer_unsupported",
                                                 "Host-visible upload heaps cannot be GPU-written; use device-local storage.");
        if (!BufferViewSizeSupported(descriptor))
            return Failure<RenderMemoryCostPlan>("render.d3d12.buffer_unsupported",
                                                 "Use aligned constant/raw storage buffer sizes representable by native descriptors.");
        auto shape = BufferShape(descriptor.byteSize);
        if (HasBufferUsage(descriptor.usage, RenderBufferUsage::Storage))
            shape.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return Cost(shape, descriptor.byteSize,
                    descriptor.access == RenderBufferAccess::HostVisible ? RenderMemoryClass::Upload : RenderMemoryClass::PersistentDevice);
    }

    /** @copydoc Resources::QueryTextureMemoryCost */
    Result<RenderMemoryCostPlan> Resources::QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const {
        if (auto checked = Check(); checked.HasError())
            return Result<RenderMemoryCostPlan>::Failure(checked.ErrorValue());
        const auto bytes = RenderTextureBaseLevelByteSize(descriptor);
        if (!bytes || descriptor.depth > 2048 || descriptor.layerCount > 2048 || descriptor.extent.width > 16384 ||
            descriptor.extent.height > 16384)
            return Failure<RenderMemoryCostPlan>("render.d3d12.texture_invalid",
                                                 "Use representable D3D12 texture dimensions and base-level bytes.");
        if (IsDepth(descriptor.format) && HasTextureUsage(descriptor.usage, RenderTextureUsage::Storage))
            return Failure<RenderMemoryCostPlan>("render.d3d12.texture_unsupported", "Depth storage cannot expose typed unordered access.");
        auto supported = ValidateTextureSupport(*device_.Get(), descriptor);
        if (supported.HasError())
            return Result<RenderMemoryCostPlan>::Failure(supported.ErrorValue());
        return Cost(TextureShape(descriptor), *bytes, RenderMemoryClass::PersistentDevice);
    }

    /** @copydoc Resources::CreateBuffer */
    Result<std::uint64_t> Resources::CreateBuffer(const RenderBufferDescriptor &descriptor, std::span<const std::byte> bytes,
                                                  const RenderMemoryPlacement &placement, RenderResourceNativeLifetime lifetime) {
        auto plan = QueryBufferMemoryCost(descriptor);
        if (plan.HasError())
            return Result<std::uint64_t>::Failure(plan.ErrorValue());
        if (auto valid = ValidateRenderBufferInitialData(descriptor, {bytes}); valid.HasError())
            return Result<std::uint64_t>::Failure(valid.ErrorValue());
        auto shape = BufferShape(descriptor.byteSize);
        if (HasBufferUsage(descriptor.usage, RenderBufferUsage::Storage))
            shape.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        auto created = Allocate(shape, plan.Value(), placement, Resource::Kind::Buffer, std::move(lifetime));
        if (created.HasError())
            return Result<std::uint64_t>::Failure(created.ErrorValue());
        const auto resource = created.Value();
        CreationRollback rollback{*this, *resource};
        resource->buffer = descriptor;
        if (auto views = CreateBufferViews(*resource); views.HasError()) {
            return Result<std::uint64_t>::Failure(views.ErrorValue());
        }
        if (!bytes.empty()) {
            auto upload = Write(resource, bytes, 0, {});
            if (upload.HasError()) {
                return Result<std::uint64_t>::Failure(upload.ErrorValue());
            }
        }
        rollback.Commit();
        return Result<std::uint64_t>::Success(resource->id);
    }

    /** @copydoc Resources::CreateBufferViews */
    Result<void> Resources::CreateBufferViews(Resource &resource) {
        const auto &descriptor = resource.buffer;
        if (HasBufferUsage(descriptor.usage, RenderBufferUsage::Uniform)) {
            auto slot = pools_[0].Allocate();
            if (!slot) {
                return Failure<void>("render.d3d12.descriptor_capacity", "Retire descriptors before allocating a constant-buffer view.");
            }
            resource.descriptors[0] = slot;
            const D3D12_CONSTANT_BUFFER_VIEW_DESC view{resource.native->GetGPUVirtualAddress(), static_cast<UINT>(descriptor.byteSize)};
            device_->CreateConstantBufferView(&view, pools_[0].Handle(*slot));
        }
        if (HasBufferUsage(descriptor.usage, RenderBufferUsage::Storage)) {
            auto slot = pools_[0].Allocate();
            if (!slot) {
                return Failure<void>("render.d3d12.descriptor_capacity", "Retire descriptors before allocating a storage view.");
            }
            resource.descriptors[4] = slot;
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            view.Buffer.NumElements = static_cast<UINT>(descriptor.byteSize / 4);
            view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            device_->CreateUnorderedAccessView(resource.native.Get(), nullptr, &view, pools_[0].Handle(*slot));
        }
        return Result<void>::Success();
    }

    /** @copydoc Resources::CreateTexture */
    Result<std::uint64_t> Resources::CreateTexture(const RenderTextureDescriptor &descriptor, std::span<const std::byte> bytes,
                                                   const RenderMemoryPlacement &placement, RenderResourceNativeLifetime lifetime) {
        auto plan = QueryTextureMemoryCost(descriptor);
        if (plan.HasError())
            return Result<std::uint64_t>::Failure(plan.ErrorValue());
        if (!bytes.empty() &&
            (bytes.size() != *RenderTextureBaseLevelByteSize(descriptor) || descriptor.sampleCount != 1 || IsDepth(descriptor.format)))
            return Failure<std::uint64_t>("render.d3d12.upload_unsupported",
                                          "Texture initialization requires one complete uncompressed color base level without MSAA.");
        auto created = Allocate(TextureShape(descriptor), plan.Value(), placement, Resource::Kind::Texture, std::move(lifetime));
        if (created.HasError())
            return Result<std::uint64_t>::Failure(created.ErrorValue());
        const auto resource = created.Value();
        CreationRollback rollback{*this, *resource};
        resource->texture = descriptor;
        if (!bytes.empty()) {
            auto upload = Write(resource, bytes, 0, {});
            if (upload.HasError()) {
                return Result<std::uint64_t>::Failure(upload.ErrorValue());
            }
        }
        rollback.Commit();
        return Result<std::uint64_t>::Success(resource->id);
    }

    /** @copydoc Resources::Cost */
    Result<RenderMemoryCostPlan> Resources::Cost(const D3D12_RESOURCE_DESC &shape, std::size_t payload,
                                                 RenderMemoryClass memoryClass) const {
        const auto queried = QueryAllocationInfo(*device_.Get(), shape);
        if (queried.HasError())
            return Result<RenderMemoryCostPlan>::Failure(queried.ErrorValue());
        const auto &info = queried.Value();
        if (info.SizeInBytes < payload)
            return Failure<RenderMemoryCostPlan>("render.d3d12.requirements_failed",
                                                 "The selected device rejected resource allocation requirements.");
        return Result<RenderMemoryCostPlan>::Success({memoryClass,
                                                      RenderMemoryAllocationClass::Suballocated,
                                                      RenderMemoryCostProvenance::Exact,
                                                      {static_cast<std::uint64_t>(Compatibility(shape, memoryClass))},
                                                      payload,
                                                      static_cast<std::size_t>(info.SizeInBytes),
                                                      static_cast<std::size_t>(info.Alignment)});
    }

    /** @copydoc Resources::PlacementMatches */
    bool Resources::PlacementMatches(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) const {
        if (!PlacementRequirementsMatch(plan, placement))
            return false;
        if (plan.allocationClass == RenderMemoryAllocationClass::Dedicated)
            if (placement.offsetBytes != 0 || placement.backingBytes != plan.requiredBytes)
                return false;
        return std::ranges::none_of(resources_, [&](const auto &entry) {
            if (entry->pool != placement.pool || entry->block != placement.block)
                return false;
            if (plan.allocationClass == RenderMemoryAllocationClass::Dedicated || !entry->heap)
                return true;
            const auto begin = placement.offsetBytes;
            const auto end = begin + placement.requiredBytes;
            return begin < entry->offsetBytes + entry->requiredBytes && entry->offsetBytes < end;
        });
    }

    /** @copydoc Resources::AcquireHeap */
    Result<std::shared_ptr<HeapAllocation>> Resources::AcquireHeap(const D3D12_RESOURCE_DESC &shape, const RenderMemoryCostPlan &plan,
                                                                   const RenderMemoryPlacement &placement) {
        const auto found = std::ranges::find_if(heapAllocations_, [&](const auto &heap) {
            return heap->pool == placement.pool && heap->block == placement.block;
        });
        const auto flags = HeapFlags(shape);
        if (found != heapAllocations_.end()) {
            const auto &heap = *found;
            if (heap->scope != placement.scope || heap->memoryClass != placement.memoryClass ||
                heap->compatibility != placement.compatibility || heap->capacity != placement.backingBytes ||
                heap->alignment != plan.alignment || heap->flags != flags)
                return Failure<std::shared_ptr<HeapAllocation>>("render.d3d12.placement_invalid",
                                                                "Reuse only the exact admitted compatible heap and scope.");
            return Result<std::shared_ptr<HeapAllocation>>::Success(heap);
        }
        auto heap = std::make_shared<HeapAllocation>();
        heap->pool = placement.pool;
        heap->block = placement.block;
        heap->scope = placement.scope;
        heap->memoryClass = placement.memoryClass;
        heap->compatibility = placement.compatibility;
        heap->capacity = placement.backingBytes;
        heap->alignment = plan.alignment;
        heap->flags = flags;
        D3D12_HEAP_DESC description{};
        description.SizeInBytes = heap->capacity;
        description.Alignment = heap->alignment;
        description.Properties.Type = plan.memoryClass == RenderMemoryClass::Upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
        description.Properties.CreationNodeMask = 1;
        description.Properties.VisibleNodeMask = 1;
        description.Flags = flags;
        const HRESULT status = device_->CreateHeap(&description, IID_PPV_ARGS(&heap->native));
        if (FAILED(status))
            return Result<std::shared_ptr<HeapAllocation>>::Failure(
                ResourceError("render.d3d12.allocation_failed", "Allocate the exact admitted native heap capacity.", status));
        heap->slot = heapAllocations_.size();
        heapAllocations_.push_back(heap);
        return Result<std::shared_ptr<HeapAllocation>>::Success(std::move(heap));
    }

    /** @copydoc Resources::Allocate */
    Result<std::shared_ptr<Resource>> Resources::Allocate(const D3D12_RESOURCE_DESC &shape, const RenderMemoryCostPlan &plan,
                                                          const RenderMemoryPlacement &placement, Resource::Kind kind,
                                                          RenderResourceNativeLifetime lifetime) {
        if (!PlacementMatches(plan, placement))
            return Failure<std::shared_ptr<Resource>>("render.d3d12.placement_invalid",
                                                      "Use an unchanged admitted placement with no live native overlap.");
        auto result = NewResource(kind, std::move(lifetime));
        if (result.HasError())
            return result;
        const auto resource = result.Value();
        auto acquired = AcquireHeap(shape, plan, placement);
        if (acquired.HasError())
            return Result<std::shared_ptr<Resource>>::Failure(acquired.ErrorValue());
        resource->heap = acquired.Value();
        HeapRollback rollback{*this, *resource->heap};
        const HRESULT status =
            device_->CreatePlacedResource(resource->heap->native.Get(), placement.offsetBytes, &shape,
                                          plan.memoryClass == RenderMemoryClass::Upload ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                                        : D3D12_RESOURCE_STATE_COMMON,
                                          nullptr, IID_PPV_ARGS(&resource->native));
        if (FAILED(status))
            return Result<std::shared_ptr<Resource>>::Failure(
                ResourceError("render.d3d12.allocation_failed", "Create the resource inside its admitted heap region.", status));
        resource->pool = placement.pool;
        resource->block = placement.block;
        resource->offsetBytes = placement.offsetBytes;
        resource->requiredBytes = placement.requiredBytes;
        ++resource->heap->allocationCount;
        resources_.push_back(resource);
        return result;
    }
}  // namespace Horo::Render::D3D12Detail
#endif
