#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "MetalResourceRuntimeInternal.h"

#include <algorithm>

namespace Horo::Render::Detail {
    /** @copydoc MetalResourceRuntime::QueryMaterialParameterBufferSize */
    Result<std::size_t> MetalResourceRuntime::QueryMaterialParameterBufferSize(std::uint64_t pipeline,
                                                                               const MaterialBindingDescriptor &descriptor,
                                                                               ShaderBindingId binding) const {
        const auto native = Decode<MetalGraphicsPipeline>(pipeline);
        if (!impl_->graphicsPipelines.contains(native) || !SameMetalMaterialLayout(native->layout, descriptor.layout))
            return Result<std::size_t>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        const auto declaration = std::ranges::find(native->layout.resources, binding, &ShaderReflectedBinding::id);
        if (declaration == native->layout.resources.end() || !declaration->active || declaration->kind != ShaderResourceKind::UniformBuffer)
            return Result<std::size_t>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        const auto mapping = std::ranges::find_if(native->layout.targetBindings, [&](const auto &entry) {
            return entry.id == binding && entry.active && entry.generatedHelperIndex == 0;
        });
        if (mapping == native->layout.targetBindings.end() || mapping->nativeBinding >= native->bufferSizes.size() ||
            native->bufferSizes[mapping->nativeBinding] == 0)
            return Result<std::size_t>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        return Result<std::size_t>::Success(native->bufferSizes[mapping->nativeBinding]);
    }

    namespace {
        /** @brief Checks range arithmetic and actual native element boundaries before any material publication. */
        bool ValidMaterialBufferRange(const MaterialResidentResource &resident, const std::size_t nativeBytes,
                                      const std::size_t requiredBytes, const std::size_t alignment, const bool storage) noexcept {
            if (resident.offset % alignment != 0 || resident.offset > nativeBytes || resident.byteCount > nativeBytes - resident.offset ||
                requiredBytes == 0 || resident.byteCount < requiredBytes)
                return false;
            return !storage || resident.byteCount % requiredBytes == 0;
        }
    }  // namespace

    Result<MetalMaterialSlot> MetalResourceRuntime::Impl::ResolveMaterialBuffer(const ShaderResourceKind kind,
                                                                                const MaterialResidentResource &resident,
                                                                                const std::size_t requiredBufferBytes) const {
        auto *buffer = Decode<MetalBufferInstance>(resident.instance);
        const bool storage = kind == ShaderResourceKind::StorageBuffer;
        const auto usage = storage ? RenderBufferUsage::Storage : RenderBufferUsage::Uniform;
        if (resident.kind != MaterialResidentResourceKind::Buffer || !buffers.contains(buffer) || buffer->use.retired ||
            !HasBufferUsage(buffer->usage, usage))
            return Result<MetalMaterialSlot>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        if (!ValidMaterialBufferRange(resident, buffer->buffer.length, requiredBufferBytes, storage ? 16 : 256, storage))
            return Result<MetalMaterialSlot>::Failure(
                MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                          "Material buffer range is incompatible with its actual native block/element ABI."));
        MetalMaterialSlot slot{.kind = resident.kind,
                               .buffer = buffer->buffer,
                               .sourceBuffer = buffer,
                               .offset = resident.offset,
                               .byteCount = resident.byteCount};
        return Result<MetalMaterialSlot>::Success(std::move(slot));
    }

    Result<MetalMaterialSlot> MetalResourceRuntime::Impl::ResolveMaterialTexture(const MaterialResidentResource &resident) const {
        MetalMaterialSlot slot{.kind = resident.kind};
        auto *view = Decode<MetalTextureViewInstance>(resident.instance);
        if (resident.kind != MaterialResidentResourceKind::TextureView || resident.offset != 0 || resident.byteCount != 0 ||
            !textureViews.contains(view) || !HasTextureUsage(view->usage, RenderTextureUsage::Sampled) ||
            view->texture.textureType != MTLTextureType2D || view->texture.sampleCount != 1)
            return Result<MetalMaterialSlot>::Failure(
                MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                          "Material texture view must be an exact resident sampled 2D single-sample generation."));
        slot.texture = view->texture;
        slot.sourceTexture = view->sourceTexture;
        slot.sourceTextureInstance = view->sourceInstance;
        return Result<MetalMaterialSlot>::Success(std::move(slot));
    }

    Result<MetalMaterialSlot> MetalResourceRuntime::Impl::ResolveMaterialSampler(const MaterialResidentResource &resident) const {
        MetalMaterialSlot slot{.kind = resident.kind};
        auto *sampler = Decode<MetalSamplerInstance>(resident.instance);
        if (resident.kind != MaterialResidentResourceKind::Sampler || resident.offset != 0 || resident.byteCount != 0 ||
            !samplers.contains(sampler))
            return Result<MetalMaterialSlot>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        if (sampler->descriptor.compareEnabled)
            return Result<MetalMaterialSlot>::Failure(
                MakeError(MaterialBindingErrors::Unsupported,
                          "Comparison samplers require a separately admitted depth-texture material path."));
        slot.sampler = sampler->sampler;
        return Result<MetalMaterialSlot>::Success(std::move(slot));
    }

    /** @copydoc MetalResourceRuntime::Impl::ResolveMaterialResource */
    Result<MetalMaterialSlot> MetalResourceRuntime::Impl::ResolveMaterialResource(ShaderResourceKind kind,
                                                                                  const MaterialResidentResource &resident,
                                                                                  std::size_t requiredBufferBytes) const {
        switch (kind) {
            case ShaderResourceKind::UniformBuffer:
            case ShaderResourceKind::StorageBuffer:
                return ResolveMaterialBuffer(kind, resident, requiredBufferBytes);
            case ShaderResourceKind::SampledTexture:
                return ResolveMaterialTexture(resident);
            case ShaderResourceKind::Sampler:
                return ResolveMaterialSampler(resident);
            default:
                return Result<MetalMaterialSlot>::Failure(MakeError(MaterialBindingErrors::Unsupported));
        }
    }

    Result<MetalMaterialSlot> MetalResourceRuntime::Impl::ResolveMaterialSlot(const MetalGraphicsPipeline &pipeline,
                                                                              const MaterialResourceBinding &logical,
                                                                              const MaterialResidentResource &resident) const {
        const auto mapping = std::ranges::find_if(pipeline.layout.targetBindings, [&](const auto &entry) {
            return entry.id == logical.binding && entry.active && entry.generatedHelperIndex == 0;
        });
        const auto declaration = std::ranges::find(pipeline.layout.resources, logical.binding, &ShaderReflectedBinding::id);
        if (mapping == pipeline.layout.targetBindings.end() || declaration == pipeline.layout.resources.end() ||
            mapping->nativeSpace != 0 || mapping->nativeBinding >= 31 || declaration->arrayCount != 1 || logical.arrayElement != 0)
            return Result<MetalMaterialSlot>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        auto resource = ResolveMaterialResource(declaration->kind, resident, pipeline.bufferSizes[mapping->nativeBinding]);
        if (resource.HasError())
            return Result<MetalMaterialSlot>::Failure(resource.ErrorValue());
        auto slot = std::move(resource).Value();
        slot.binding = mapping->nativeBinding;
        slot.stages = declaration->stages;
        return Result<MetalMaterialSlot>::Success(std::move(slot));
    }

    /** @copydoc MetalResourceRuntime::RealizeResolved */
    Result<std::unique_ptr<IResidentMaterialBinding>> MetalResourceRuntime::RealizeResolved(
        const MaterialBindingDescriptor &descriptor, std::uint64_t pipeline, std::span<const MaterialResidentResource> resources) {
        using Outcome = Result<std::unique_ptr<IResidentMaterialBinding>>;
        if (const auto valid = ValidateMaterialBindingDescriptor(descriptor, descriptor.pipeline.owner); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        auto *native = Decode<MetalGraphicsPipeline>(pipeline);
        if (!impl_->graphicsPipelines.contains(native) || !SameMetalMaterialLayout(native->layout, descriptor.layout) ||
            resources.size() != descriptor.resources.size() || resources.size() > 128)
            return Outcome::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        auto table = std::make_unique<MetalMaterialBinding>();
        table->owner = impl_.get();
        table->deviceIncarnation = impl_->deviceIncarnation;
        table->sourcePipeline = native;
        table->pipeline = native->pipeline;
        table->depth = native->depth;
        table->state = native->state;
        for (std::size_t index = 0; index < resources.size(); ++index) {
            const auto &logical = descriptor.resources[index];
            const auto &resident = resources[index];
            auto slot = impl_->ResolveMaterialSlot(*native, logical, resident);
            if (slot.HasError())
                return Outcome::Failure(slot.ErrorValue());
            table->slots[table->count++] = std::move(slot).Value();
        }
        return Outcome::Success(std::move(table));
    }

}  // namespace Horo::Render::Detail
