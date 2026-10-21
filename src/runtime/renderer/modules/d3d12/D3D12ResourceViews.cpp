#include "D3D12ResourceInternals.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12Detail {

    namespace {
        /** @brief Validates dependent buffer roles without dereferencing absent or wrong-kind instances. */
        bool MeshBuffersMatch(const std::shared_ptr<Resource> &vertices, const std::shared_ptr<Resource> &indices) {
            return vertices && indices && vertices->kind == Resource::Kind::Buffer && indices->kind == Resource::Kind::Buffer &&
                   HasBufferUsage(vertices->buffer.usage, RenderBufferUsage::Vertex) &&
                   HasBufferUsage(indices->buffer.usage, RenderBufferUsage::Index);
        }

        /** @brief Requires both logical ranges and native 32-bit vertex/index view sizes to fit without truncation. */
        bool MeshRangesMatch(const RenderMeshDescriptor &descriptor, const Resource &vertices, const Resource &indices) {
            const std::uint64_t vertexBytes = std::uint64_t(descriptor.vertexStride) * descriptor.vertexCount;
            const std::uint64_t indexBytes =
                std::uint64_t(descriptor.indexCount) * (descriptor.indexFormat == RenderIndexFormat::UInt16 ? 2U : 4U);
            return vertexBytes <= vertices.buffer.byteSize && indexBytes <= indices.buffer.byteSize &&
                   vertexBytes <= std::numeric_limits<UINT>::max() && indexBytes <= std::numeric_limits<UINT>::max();
        }

        /** @brief Identifies native mip-specific attachment/storage descriptors. */
        bool NeedsSingleMip(const RenderTextureDescriptor &texture) {
            return HasTextureUsage(texture.usage, RenderTextureUsage::Storage) ||
                   HasTextureUsage(texture.usage, RenderTextureUsage::RenderAttachment);
        }

        /** @brief Preserves canonical shader dimensionality and the planes exposed by native cube descriptors. */
        bool ShaderShapeSupported(const RenderTextureDescriptor &texture, const RenderTextureViewDescriptor &view) {
            if (view.dimension == RenderTextureViewDimension::Cube || view.dimension == RenderTextureViewDimension::CubeArray) {
                if (texture.sampleCount != 1 || view.aspect == RenderTextureAspect::Stencil)
                    return false;
                return view.dimension != RenderTextureViewDimension::Cube || view.baseLayer == 0;
            }
            if (texture.layerCount > 1)
                return view.dimension != RenderTextureViewDimension::TwoD && view.dimension != RenderTextureViewDimension::OneD;
            return true;
        }

        /** @brief Rejects native view policies which would expose a different range or shader plane. */
        bool ViewPolicySupported(const RenderTextureDescriptor &texture, const RenderTextureViewDescriptor &view) {
            if (!ShaderShapeSupported(texture, view))
                return false;
            if (NeedsSingleMip(texture) && view.mipCount != 1)
                return false;
            return !(view.aspect == RenderTextureAspect::DepthStencil && HasTextureUsage(texture.usage, RenderTextureUsage::Sampled));
        }

        /** @brief Validates typed view support before calling native void-returning descriptor creation APIs. */
        Result<void> CheckViewFormat(ID3D12Device &device, DXGI_FORMAT format, UINT required1, UINT required2) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
            const HRESULT status = device.CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
            if (FAILED(status))
                return Result<void>::Failure(
                    ResourceError("render.d3d12.format_query_failed", "Query the exact typed view format.", status));
            if ((support.Support1 & required1) != required1 || (support.Support2 & required2) != required2)
                return Result<void>::Failure(ResourceError("render.d3d12.view_unsupported",
                                                           "Select a typed view format supporting every requested use on this device."));
            return Result<void>::Success();
        }

        /** @brief Checks shader, attachment and storage interpretations independently for compatible typeless storage. */
        Result<void> ViewFormatSupported(ID3D12Device &device, const RenderTextureDescriptor &texture,
                                         const RenderTextureViewDescriptor &view) {
            if (HasTextureUsage(texture.usage, RenderTextureUsage::Sampled)) {
                const UINT required =
                    view.aspect == RenderTextureAspect::Stencil ? D3D12_FORMAT_SUPPORT1_SHADER_LOAD : D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
                if (auto valid = CheckViewFormat(device, ShaderViewFormat(view), required, 0); valid.HasError())
                    return valid;
            }
            if (HasTextureUsage(texture.usage, RenderTextureUsage::RenderAttachment)) {
                const UINT required = IsDepth(view.format) ? D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL : D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
                if (auto valid = CheckViewFormat(device, NativeFormat(view.format), required, 0); valid.HasError())
                    return valid;
            }
            if (HasTextureUsage(texture.usage, RenderTextureUsage::Storage))
                return CheckViewFormat(device, NativeFormat(view.format), 0, D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
            return Result<void>::Success();
        }

        /** @brief Converts the validated comparison/anisotropy/filter combination. */
        D3D12_FILTER SamplerFilter(const RenderSamplerDescriptor &descriptor) {
            if (descriptor.maximumAnisotropy > 1)
                return descriptor.compareEnabled ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
            return D3D12_ENCODE_BASIC_FILTER(descriptor.minFilter == RenderSamplerFilter::Linear ? D3D12_FILTER_TYPE_LINEAR
                                                                                                 : D3D12_FILTER_TYPE_POINT,
                                             descriptor.magFilter == RenderSamplerFilter::Linear ? D3D12_FILTER_TYPE_LINEAR
                                                                                                 : D3D12_FILTER_TYPE_POINT,
                                             descriptor.mipmapMode == RenderSamplerMipmapMode::Linear ? D3D12_FILTER_TYPE_LINEAR
                                                                                                      : D3D12_FILTER_TYPE_POINT,
                                             descriptor.compareEnabled ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON
                                                                       : D3D12_FILTER_REDUCTION_TYPE_STANDARD);
        }

        /** @brief Creates the immutable native sampler description after canonical validation. */
        D3D12_SAMPLER_DESC NativeSampler(const RenderSamplerDescriptor &descriptor) {
            D3D12_SAMPLER_DESC sampler{};
            sampler.Filter = SamplerFilter(descriptor);
            constexpr std::array address{D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
                                         D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_BORDER};
            sampler.AddressU = address[static_cast<std::size_t>(descriptor.addressU)];
            sampler.AddressV = address[static_cast<std::size_t>(descriptor.addressV)];
            sampler.AddressW = address[static_cast<std::size_t>(descriptor.addressW)];
            sampler.MaxAnisotropy = static_cast<UINT>(descriptor.maximumAnisotropy);
            constexpr std::array comparisons{D3D12_COMPARISON_FUNC_NEVER,         D3D12_COMPARISON_FUNC_LESS,
                                             D3D12_COMPARISON_FUNC_LESS_EQUAL,    D3D12_COMPARISON_FUNC_EQUAL,
                                             D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_GREATER,
                                             D3D12_COMPARISON_FUNC_NOT_EQUAL,     D3D12_COMPARISON_FUNC_ALWAYS};
            sampler.ComparisonFunc = comparisons[static_cast<std::size_t>(descriptor.compare)];
            sampler.MinLOD = descriptor.minimumLod;
            sampler.MaxLOD = descriptor.maximumLod;
            return sampler;
        }

        /** @brief Checks one resolved attachment without conflating presence and framebuffer compatibility. */
        bool AttachmentMatches(const std::shared_ptr<Resource> &view, UINT pool, const RenderTargetDescriptor &descriptor) {
            if (!view || view->kind != Resource::Kind::View || !view->descriptors[pool])
                return false;
            const auto &storage = view->texture;
            return std::max(1U, storage.extent.width >> view->view.baseMip) == descriptor.extent.width &&
                   std::max(1U, storage.extent.height >> view->view.baseMip) == descriptor.extent.height &&
                   storage.sampleCount == descriptor.sampleCount;
        }

        /** @brief Validates optional attachment presence and each resolved role. */
        bool TargetMatches(const RenderTargetDescriptor &descriptor, const std::shared_ptr<Resource> &color,
                           const std::shared_ptr<Resource> &depth) {
            if (!descriptor.IsValid() || descriptor.colorAttachment.IsValid() != bool(color) ||
                descriptor.depthAttachment.IsValid() != bool(depth))
                return false;
            if (color && !AttachmentMatches(color, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, descriptor))
                return false;
            return !depth || AttachmentMatches(depth, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, descriptor);
        }
    }  // namespace

    /** @copydoc Resources::CreateMesh */
    Result<std::uint64_t> Resources::CreateMesh(const RenderMeshDescriptor &descriptor, std::uint64_t vertex, std::uint64_t index,
                                                RenderResourceNativeLifetime lifetime) {
        if (auto checked = Check(); checked.HasError())
            return Result<std::uint64_t>::Failure(checked.ErrorValue());
        auto vertices = Find(vertex);
        auto indices = Find(index);
        if (!descriptor.IsValid() || !MeshBuffersMatch(vertices, indices) || !MeshRangesMatch(descriptor, *vertices, *indices))
            return Failure<std::uint64_t>("render.d3d12.mesh_invalid", "Resolve compatible live vertex/index buffers and checked ranges.");
        auto created = NewResource(Resource::Kind::Mesh, std::move(lifetime));
        if (created.HasError())
            return Result<std::uint64_t>::Failure(created.ErrorValue());
        const auto resource = created.Value();
        resource->mesh = descriptor;
        resource->vertexView = {vertices->native->GetGPUVirtualAddress(),
                                static_cast<UINT>(std::uint64_t(descriptor.vertexStride) * descriptor.vertexCount),
                                descriptor.vertexStride};
        resource->indexView = {indices->native->GetGPUVirtualAddress(),
                               static_cast<UINT>(std::uint64_t(descriptor.indexCount) *
                                                 (descriptor.indexFormat == RenderIndexFormat::UInt16 ? 2U : 4U)),
                               descriptor.indexFormat == RenderIndexFormat::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        resource->dependencies = {vertices, indices};
        resources_.push_back(resource);
        return Result<std::uint64_t>::Success(resource->id);
    }

    /** @copydoc Resources::CreateTextureView */
    Result<std::uint64_t> Resources::CreateTextureView(const RenderTextureViewDescriptor &descriptor, std::uint64_t texture,
                                                       RenderResourceNativeLifetime lifetime) {
        if (auto checked = Check(); checked.HasError())
            return Result<std::uint64_t>::Failure(checked.ErrorValue());
        const auto storage = Find(texture);
        if (!storage || storage->kind != Resource::Kind::Texture)
            return Failure<std::uint64_t>("render.d3d12.view_invalid", "Resolve the exact live texture before creating its view.");
        auto valid = ValidateRenderTextureViewCompatibility(storage->texture, descriptor);
        if (valid.HasError())
            return Result<std::uint64_t>::Failure(valid.ErrorValue());
        if (!ViewPolicySupported(storage->texture, descriptor))
            return Failure<std::uint64_t>("render.d3d12.view_unsupported",
                                          "Use explicit array views, one mip for attachment/storage, and one shader plane.");
        if (auto supported = ViewFormatSupported(*device_.Get(), storage->texture, descriptor); supported.HasError())
            return Result<std::uint64_t>::Failure(supported.ErrorValue());
        auto created = NewResource(Resource::Kind::View, std::move(lifetime));
        if (created.HasError())
            return Result<std::uint64_t>::Failure(created.ErrorValue());
        auto resource = created.Value();
        resource->view = descriptor;
        resource->texture = storage->texture;
        resource->dependencies = {storage};
        resources_.push_back(resource);
        CreationRollback rollback{*this, *resource};
        if (auto views = CreateTextureDescriptors(*resource, *storage); views.HasError()) {
            return Result<std::uint64_t>::Failure(views.ErrorValue());
        }
        rollback.Commit();
        return Result<std::uint64_t>::Success(resource->id);
    }

    /** @copydoc Resources::CreateTextureDescriptors */
    Result<void> Resources::CreateTextureDescriptors(Resource &resource, const Resource &storage) {
        const auto &descriptor = resource.view;
        const auto allocate = [&](UINT pool) -> std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> {
            auto slot = pools_[pool].Allocate();
            resource.descriptors[pool] = slot;
            return slot ? std::optional{pools_[pool].Handle(*slot)} : std::nullopt;
        };
        if (HasTextureUsage(storage.texture.usage, RenderTextureUsage::Sampled)) {
            auto handle = allocate(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            if (!handle) {
                return Failure<void>("render.d3d12.descriptor_capacity", "Retire descriptor users before allocating another view.");
            }
            const auto view = ShaderView(descriptor, storage.texture.sampleCount > 1);
            device_->CreateShaderResourceView(storage.native.Get(), &view, *handle);
        }
        if (HasTextureUsage(storage.texture.usage, RenderTextureUsage::RenderAttachment)) {
            const bool depth = IsDepth(descriptor.format);
            auto handle = allocate(depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            if (!handle) {
                return Failure<void>("render.d3d12.descriptor_capacity",
                                     "Retire attachment descriptor users before allocating another view.");
            }
            if (depth) {
                const auto view = DepthView(descriptor, storage.texture);
                device_->CreateDepthStencilView(storage.native.Get(), &view, *handle);
            } else {
                const auto view = ColorView(descriptor, storage.texture);
                device_->CreateRenderTargetView(storage.native.Get(), &view, *handle);
            }
        }
        if (HasTextureUsage(storage.texture.usage, RenderTextureUsage::Storage)) {
            auto slot = pools_[0].Allocate();
            if (!slot) {
                return Failure<void>("render.d3d12.descriptor_capacity", "Retire storage descriptor users before allocating another view.");
            }
            resource.descriptors[4] = slot;
            const auto view = StorageView(descriptor, storage.texture);
            device_->CreateUnorderedAccessView(storage.native.Get(), nullptr, &view, pools_[0].Handle(*slot));
        }
        return Result<void>::Success();
    }

    /** @copydoc Resources::QuerySamplerMemoryCost */
    Result<RenderMemoryCostPlan> Resources::QuerySamplerMemoryCost(const RenderSamplerDescriptor &descriptor) const {
        if (auto checked = Check(); checked.HasError())
            return Result<RenderMemoryCostPlan>::Failure(checked.ErrorValue());
        if (!descriptor.IsValid() || descriptor.maximumAnisotropy > 16 ||
            descriptor.maximumAnisotropy != std::floor(descriptor.maximumAnisotropy))
            return Failure<RenderMemoryCostPlan>("render.d3d12.sampler_invalid",
                                                 "Use a valid sampler with integral anisotropy from 1 to 16.");
        // Native descriptor storage is opaque. Conservatively reserve one 64-KiB heap per sampler.
        return Result<RenderMemoryCostPlan>::Success({RenderMemoryClass::PersistentDevice,
                                                      RenderMemoryAllocationClass::Dedicated,
                                                      RenderMemoryCostProvenance::Estimated,
                                                      {static_cast<std::uint64_t>(MemoryCompatibility::Sampler)},
                                                      1,
                                                      65536,
                                                      65536});
    }

    /** @copydoc Resources::CreateSampler */
    Result<std::uint64_t> Resources::CreateSampler(const RenderSamplerDescriptor &descriptor, const RenderMemoryPlacement &placement) {
        auto plan = QuerySamplerMemoryCost(descriptor);
        if (plan.HasError())
            return Result<std::uint64_t>::Failure(plan.ErrorValue());
        if (!PlacementMatches(plan.Value(), placement))
            return Failure<std::uint64_t>("render.d3d12.placement_invalid", "Admit sampler descriptor backing before creation.");
        auto created = NewResource(Resource::Kind::Sampler);
        if (created.HasError())
            return Result<std::uint64_t>::Failure(created.ErrorValue());
        auto resource = created.Value();
        const auto slot = pools_[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER].Allocate();
        if (!slot)
            return Failure<std::uint64_t>("render.d3d12.descriptor_capacity", "Retire sampler descriptors before creating another.");
        resource->descriptors[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER] = slot;
        resource->pool = placement.pool;
        resource->backingBytes = placement.backingBytes;
        resource->requiredBytes = placement.requiredBytes;
        resource->sampler = descriptor;
        resources_.push_back(resource);
        const auto sampler = NativeSampler(descriptor);
        device_->CreateSampler(&sampler, pools_[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER].Handle(*slot));
        return Result<std::uint64_t>::Success(resource->id);
    }

    /** @copydoc Resources::CreateRenderTarget */
    Result<std::uint64_t> Resources::CreateRenderTarget(const RenderTargetDescriptor &descriptor, std::uint64_t color, std::uint64_t depth,
                                                        RenderResourceNativeLifetime lifetime) {
        if (auto checked = Check(); checked.HasError())
            return Result<std::uint64_t>::Failure(checked.ErrorValue());
        auto colorView = Find(color);
        auto depthView = Find(depth);
        if ((color != 0 && !colorView) || (depth != 0 && !depthView) || !TargetMatches(descriptor, colorView, depthView))
            return Failure<std::uint64_t>("render.d3d12.target_invalid",
                                          "Use live compatible attachment views matching target extent and samples.");
        auto created = NewResource(Resource::Kind::Target, std::move(lifetime));
        if (created.HasError())
            return Result<std::uint64_t>::Failure(created.ErrorValue());
        auto resource = created.Value();
        resource->target = descriptor;
        if (colorView)
            resource->dependencies.push_back(colorView);
        if (depthView)
            resource->dependencies.push_back(depthView);
        resources_.push_back(resource);
        return Result<std::uint64_t>::Success(resource->id);
    }
}  // namespace Horo::Render::D3D12Detail
#endif
