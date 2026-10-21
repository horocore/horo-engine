#include "VulkanShaderRuntimeInternal.h"

#include <map>

namespace Horo::Render::Detail {
    /** @brief Validates exact registry backing and native uniform ABI before publishing a descriptor. */
    Result<VkDescriptorBufferInfo> VulkanShaderRuntime::Impl::UniformBinding(const MaterialBindingLayout &layout,
                                                                             const MaterialResourceBinding &logical,
                                                                             const MaterialResidentResource &resident) const {
        const auto resource = resolve(resident, ShaderResourceKind::UniformBuffer);
        if (resource.HasError())
            return Result<VkDescriptorBufferInfo>::Failure(resource.ErrorValue());
        const auto &buffer = resource.Value();
        const std::array validRange{buffer.buffer != VK_NULL_HANDLE,
                                    resident.offset % std::max<VkDeviceSize>(1, properties.limits.minUniformBufferOffsetAlignment) == 0,
                                    resident.byteCount != 0,
                                    resident.byteCount <= properties.limits.maxUniformBufferRange,
                                    resident.offset <= buffer.bytes,
                                    resident.byteCount <= buffer.bytes - std::min(resident.offset, buffer.bytes)};
        if (!std::ranges::all_of(validRange, [](bool valid) {
            return valid;
        }))
            return Result<VkDescriptorBufferInfo>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        {
            const auto required = ParameterBytes(layout, logical.binding);
            if (required.HasError())
                return Result<VkDescriptorBufferInfo>::Failure(required.ErrorValue());
            if (resident.byteCount < required.Value())
                return Result<VkDescriptorBufferInfo>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        }
        return Result<VkDescriptorBufferInfo>::Success({.buffer = buffer.buffer, .offset = resident.offset, .range = resident.byteCount});
    }

    /** @brief Resolves only admitted separate samplers or sampled texture views. */
    Result<VkDescriptorImageInfo> VulkanShaderRuntime::Impl::ImageBinding(ShaderResourceKind kind,
                                                                          const MaterialResidentResource &resident) const {
        if (kind == ShaderResourceKind::Sampler) {
            const auto sampler = samplers.find(resident.instance);
            if (resident.kind != MaterialResidentResourceKind::Sampler || resident.offset || resident.byteCount ||
                sampler == samplers.end() || sampler->second->descriptor.compareEnabled)
                return Result<VkDescriptorImageInfo>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
            return Result<VkDescriptorImageInfo>::Success({.sampler = sampler->second->handle});
        }
        const auto resource = resolve(resident, ShaderResourceKind::SampledTexture);
        if (resource.HasError())
            return Result<VkDescriptorImageInfo>::Failure(resource.ErrorValue());
        if (!resource.Value().view)
            return Result<VkDescriptorImageInfo>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        return Result<VkDescriptorImageInfo>::Success(
            {.imageView = resource.Value().view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }

    /** @brief Constructs one owned-pointer descriptor write from the canonical mapping and resident generation. */
    Result<VkWriteDescriptorSet> VulkanShaderRuntime::Impl::ResolveSlot(const MaterialBindingLayout &layout,
                                                                        const MaterialResourceBinding &logical,
                                                                        const MaterialResidentResource &resident,
                                                                        VkDescriptorBufferInfo &buffer,
                                                                        VkDescriptorImageInfo &image) const {
        const auto entry = std::ranges::find(layout.resources, logical.binding, &ShaderReflectedBinding::id);
        const auto mapping = std::ranges::find(layout.targetBindings, logical.binding, &ShaderTargetBindingMapEntry::id);
        if (entry == layout.resources.end() || mapping == layout.targetBindings.end() || !mapping->active)
            return Result<VkWriteDescriptorSet>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstBinding = mapping->nativeBinding,
                                   .dstArrayElement = logical.arrayElement,
                                   .descriptorCount = 1,
                                   .descriptorType = DescriptorType(entry->kind)};
        if (entry->kind == ShaderResourceKind::UniformBuffer) {
            const auto resolved = UniformBinding(layout, logical, resident);
            if (resolved.HasError())
                return Result<VkWriteDescriptorSet>::Failure(resolved.ErrorValue());
            buffer = resolved.Value();
            write.pBufferInfo = &buffer;
        } else {
            const auto resolved = ImageBinding(entry->kind, resident);
            if (resolved.HasError())
                return Result<VkWriteDescriptorSet>::Failure(resolved.ErrorValue());
            image = resolved.Value();
            write.pImageInfo = &image;
        }
        return Result<VkWriteDescriptorSet>::Success(write);
    }

    /** @brief Allocates a descriptor pool and complete set sequence beneath the unpublished material lease. */
    Result<void> VulkanShaderRuntime::Impl::AllocateMaterialSets(const Pipeline &pipeline, VulkanPreparedMaterialBinding &table,
                                                                 const std::map<VkDescriptorType, std::uint32_t> &counts) const {
        std::vector<VkDescriptorPoolSize> sizes;
        for (const auto &[type, count] : counts)
            sizes.push_back({type, count});
        // Empty descriptor-set layouts still need one pool; zero descriptor counts are legal.
        const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = static_cast<std::uint32_t>(pipeline.sets.size()),
                                              .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
                                              .pPoolSizes = sizes.data()};
        if (const auto nativeResult = createDescriptorPool(device, &pool, nullptr, &table.pool); nativeResult != VK_SUCCESS) {
            table.pool = VK_NULL_HANDLE;
            return Result<void>::Failure(NativeShaderFailure("vkCreateDescriptorPool", nativeResult));
        }
        table.sets.resize(pipeline.sets.size(), VK_NULL_HANDLE);
        const VkDescriptorSetAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                     .descriptorPool = table.pool,
                                                     .descriptorSetCount = static_cast<std::uint32_t>(table.sets.size()),
                                                     .pSetLayouts = pipeline.sets.data()};
        if (const auto nativeResult = allocateDescriptorSets(device, &allocation, table.sets.data()); nativeResult != VK_SUCCESS)
            return Result<void>::Failure(NativeShaderFailure("vkAllocateDescriptorSets", nativeResult));
        return Result<void>::Success();
    }

    /** @copydoc VulkanShaderRuntime::QueryMaterialParameterBufferSize */
    Result<std::size_t> VulkanShaderRuntime::QueryMaterialParameterBufferSize(std::uint64_t id, const MaterialBindingDescriptor &descriptor,
                                                                              ShaderBindingId binding) const {
        if (!impl_->Ready())
            return Result<std::size_t>::Failure(impl_->AdmissionError());
        const auto pipeline = impl_->pipelines.find(id);
        if (pipeline == impl_->pipelines.end() || !SameLayout(pipeline->second->material, descriptor.layout))
            return Result<std::size_t>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        const auto entry = std::ranges::find(pipeline->second->material.resources, binding, &ShaderReflectedBinding::id);
        if (entry == pipeline->second->material.resources.end() || !entry->active || entry->kind != ShaderResourceKind::UniformBuffer)
            return Result<std::size_t>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        return impl_->ParameterBytes(pipeline->second->material, binding);
    }

    /** @brief Allocates and updates descriptor sets only after every slot and backing is validated. */
    Result<void> VulkanShaderRuntime::Impl::FinalizeMaterialSets(const Pipeline &pipeline, VulkanPreparedMaterialBinding &table,
                                                                 const MaterialBindingDescriptor &descriptor,
                                                                 const std::span<VkWriteDescriptorSet> writes,
                                                                 const std::map<VkDescriptorType, std::uint32_t> &counts) const {
        if (!pipeline.sets.empty()) {
            if (const auto allocated = AllocateMaterialSets(pipeline, table, counts); allocated.HasError())
                return Result<void>::Failure(allocated.ErrorValue());
            for (std::size_t i = 0; i < writes.size(); ++i) {
                const auto mapping =
                    std::ranges::find(descriptor.layout.targetBindings, descriptor.resources[i].binding, &ShaderTargetBindingMapEntry::id);
                writes[i].dstSet = table.sets[mapping->nativeSpace];
            }
            updateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        return Result<void>::Success();
    }

    /** @brief Retains exact immutable backing alongside descriptor storage before publication. */
    Result<void> VulkanShaderRuntime::Impl::RetainMaterialResource(VulkanPreparedMaterialBinding &table,
                                                                   const MaterialBindingLayout &layout,
                                                                   const MaterialResourceBinding &logical,
                                                                   const MaterialResidentResource &resident) const {
        const auto kind = std::ranges::find(layout.resources, logical.binding, &ShaderReflectedBinding::id)->kind;
        if (kind == ShaderResourceKind::Sampler)
            table.samplerBackings.push_back(samplers.at(resident.instance));
        else {
            auto backing = resolve(resident, kind);
            if (backing.HasError())
                return Result<void>::Failure(backing.ErrorValue());
            table.resources.push_back(backing.Value());
        }
        return Result<void>::Success();
    }

    /** @copydoc VulkanShaderRuntime::RealizeResolved */
    Result<std::unique_ptr<IResidentMaterialBinding>> VulkanShaderRuntime::RealizeResolved(
        const MaterialBindingDescriptor &descriptor, std::uint64_t id, std::span<const MaterialResidentResource> resources) {
        using Outcome = Result<std::unique_ptr<IResidentMaterialBinding>>;
        if (!impl_->Ready())
            return Outcome::Failure(impl_->AdmissionError());
        if (const auto valid = ValidateMaterialBindingDescriptor(descriptor, descriptor.pipeline.owner); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        const auto *pipeline = impl_->FindMaterialPipeline(id, descriptor, resources.size());
        if (!pipeline)
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        try {
            auto table = std::make_unique<VulkanPreparedMaterialBinding>();
            table->device = impl_->device;
            table->destroyPool = impl_->destroyDescriptorPool;
            table->owner = impl_.get();
            table->pipeline = pipeline->handle;
            table->layout = pipeline->layout;
            table->deviceIncarnation = impl_->deviceIncarnation;
            table->state = pipeline->state;
            table->pipelineBacking = impl_->pipelines.at(id);
            table->resources.reserve(resources.size());
            table->samplerBackings.reserve(resources.size());
            std::vector<VkDescriptorBufferInfo> buffers(resources.size());
            std::vector<VkDescriptorImageInfo> images(resources.size());
            std::vector<VkWriteDescriptorSet> writes(resources.size());
            std::map<VkDescriptorType, std::uint32_t> counts;
            for (std::size_t i = 0; i < resources.size(); ++i) {
                const auto slot = impl_->ResolveSlot(descriptor.layout, descriptor.resources[i], resources[i], buffers[i], images[i]);
                if (slot.HasError())
                    return Outcome::Failure(slot.ErrorValue());
                if (const auto retained = impl_->RetainMaterialResource(*table, descriptor.layout, descriptor.resources[i], resources[i]);
                    retained.HasError())
                    return Outcome::Failure(retained.ErrorValue());
                writes[i] = slot.Value();
                ++counts[writes[i].descriptorType];
            }
            if (const auto finalized = impl_->FinalizeMaterialSets(*pipeline, *table, descriptor, writes, counts); finalized.HasError())
                return Outcome::Failure(finalized.ErrorValue());
            return Outcome::Success(std::move(table));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
        }
    }
}  // namespace Horo::Render::Detail
