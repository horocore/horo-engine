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

    /** @copydoc MetalResourceRuntime::Impl::ResolveMaterialResource */
    Result<MetalMaterialSlot> MetalResourceRuntime::Impl::ResolveMaterialResource(ShaderResourceKind kind,
                                                                                  const MaterialResidentResource &resident,
                                                                                  std::size_t requiredBufferBytes) const {
        MetalMaterialSlot slot{.kind = resident.kind, .offset = resident.offset};
        switch (kind) {
            case ShaderResourceKind::UniformBuffer: {
                auto *buffer = Decode<MetalBufferInstance>(resident.instance);
                if (resident.kind != MaterialResidentResourceKind::Buffer || !buffers.contains(buffer) || buffer->use.retired ||
                    !HasBufferUsage(buffer->usage, RenderBufferUsage::Uniform) || resident.offset % 256 != 0 ||
                    resident.offset > buffer->buffer.length || resident.byteCount > buffer->buffer.length - resident.offset ||
                    requiredBufferBytes == 0 || resident.byteCount < requiredBufferBytes)
                    return Result<MetalMaterialSlot>::Failure(
                        MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                                  "Material uniform buffer generation, usage, alignment or native block range is incompatible."));
                slot.buffer = buffer->buffer;
                break;
            }
            case ShaderResourceKind::SampledTexture: {
                auto *view = Decode<MetalTextureViewInstance>(resident.instance);
                if (resident.kind != MaterialResidentResourceKind::TextureView || resident.offset != 0 || resident.byteCount != 0 ||
                    !textureViews.contains(view) || !HasTextureUsage(view->usage, RenderTextureUsage::Sampled) ||
                    view->texture.textureType != MTLTextureType2D || view->texture.sampleCount != 1)
                    return Result<MetalMaterialSlot>::Failure(
                        MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                                  "Material texture view must be an exact resident sampled 2D single-sample generation."));
                slot.texture = view->texture;
                break;
            }
            case ShaderResourceKind::Sampler: {
                auto *sampler = Decode<MetalSamplerInstance>(resident.instance);
                if (resident.kind != MaterialResidentResourceKind::Sampler || resident.offset != 0 || resident.byteCount != 0 ||
                    !samplers.contains(sampler))
                    return Result<MetalMaterialSlot>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
                if (sampler->descriptor.compareEnabled)
                    return Result<MetalMaterialSlot>::Failure(
                        MakeError(MaterialBindingErrors::Unsupported,
                                  "Comparison samplers require a separately admitted depth-texture material path."));
                slot.sampler = sampler->sampler;
                break;
            }
            default:
                return Result<MetalMaterialSlot>::Failure(MakeError(MaterialBindingErrors::Unsupported));
        }
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
        table->pipeline = native->pipeline;
        table->depth = native->depth;
        table->state = native->state;
        for (std::size_t index = 0; index < resources.size(); ++index) {
            const auto &logical = descriptor.resources[index];
            const auto &resident = resources[index];
            const auto mapping = std::ranges::find_if(native->layout.targetBindings, [&](const auto &entry) {
                return entry.id == logical.binding && entry.active && entry.generatedHelperIndex == 0;
            });
            const auto declaration = std::ranges::find(native->layout.resources, logical.binding, &ShaderReflectedBinding::id);
            if (mapping == native->layout.targetBindings.end() || declaration == native->layout.resources.end() ||
                mapping->nativeSpace != 0 || mapping->nativeBinding >= 31 || declaration->arrayCount != 1 || logical.arrayElement != 0)
                return Outcome::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            auto resource = impl_->ResolveMaterialResource(declaration->kind, resident, native->bufferSizes[mapping->nativeBinding]);
            if (resource.HasError())
                return Outcome::Failure(resource.ErrorValue());
            auto slot = std::move(resource).Value();
            slot.binding = mapping->nativeBinding;
            slot.stages = declaration->stages;
            table->slots[table->count++] = std::move(slot);
        }
        return Outcome::Success(std::move(table));
    }

}  // namespace Horo::Render::Detail
