#pragma once
#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "VulkanRenderBackendErrors.h"
#include "VulkanShaderRuntime.h"

#include <algorithm>
#include <array>
#include <map>
#include <thread>
#include <unordered_map>

namespace Horo::Render::Detail {
    namespace VulkanShaderDetail {
        inline constexpr std::size_t MaximumObjects = 4096;
        RenderMemoryCostPlan Estimate(std::uint64_t kind, std::size_t payload);
        VkShaderStageFlags Stages(ShaderStageVisibility stages);
        VkDescriptorType DescriptorType(ShaderResourceKind kind);
        VkCompareOp Compare(RenderCompareFunction value);
        bool SameLayout(const MaterialBindingLayout &a, const MaterialBindingLayout &b);
    }  // namespace VulkanShaderDetail

    using namespace VulkanShaderDetail;

    struct VulkanShaderRuntime::Impl {
        struct Shader {
            VkShaderModule handle{VK_NULL_HANDLE};
            RenderShaderModuleDescriptor descriptor;
            std::size_t bytes{0};
        };

        struct Pipeline {
            VkDevice device{VK_NULL_HANDLE};
            PFN_vkDestroyPipeline destroy{nullptr};
            PFN_vkDestroyPipelineLayout destroyLayout{nullptr};
            PFN_vkDestroyDescriptorSetLayout destroySetLayout{nullptr};
            VkPipeline handle{VK_NULL_HANDLE};
            VkPipelineLayout layout{VK_NULL_HANDLE};
            std::vector<VkDescriptorSetLayout> sets;
            MaterialBindingLayout material;
            PipelineCacheKey key;

            ~Pipeline() {
                if (handle)
                    destroy(device, handle, nullptr);
                if (layout)
                    destroyLayout(device, layout, nullptr);
                for (auto set : sets)
                    if (set)
                        destroySetLayout(device, set, nullptr);
            }
        };

        struct Sampler {
            VkSampler handle{VK_NULL_HANDLE};
            RenderSamplerDescriptor descriptor;
        };

        struct StageState {
            std::array<std::vector<VkSpecializationMapEntry>, 2> entries;
            std::array<std::vector<std::uint32_t>, 2> data;
            std::array<VkSpecializationInfo, 2> specialization{};
            std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        };

        [[nodiscard]] Result<void> CreateLayouts(Pipeline &pipeline);
        void ConfigureStages(const RenderGraphicsPipelineDescriptor &descriptor, std::uint64_t vertex, std::uint64_t fragment,
                             StageState &output) const;
        VkDevice device{VK_NULL_HANDLE};
        VkPhysicalDeviceProperties properties{};
        RenderPipelineDeviceIdentity identity;
        IssueIdentity issue;
        ResolveResource resolve;
        std::thread::id owner;
        VkPipelineCache cache{VK_NULL_HANDLE};
        std::unordered_map<std::uint64_t, Shader> shaders;
        std::unordered_map<std::uint64_t, std::unique_ptr<Pipeline>> pipelines;
        std::unordered_map<std::uint64_t, Sampler> samplers;
        PFN_vkCreateShaderModule createShaderModule{nullptr};
        PFN_vkDestroyShaderModule destroyShaderModule{nullptr};
        PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout{nullptr};
        PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout{nullptr};
        PFN_vkCreatePipelineLayout createPipelineLayout{nullptr};
        PFN_vkDestroyPipelineLayout destroyPipelineLayout{nullptr};
        PFN_vkCreatePipelineCache createPipelineCache{nullptr};
        PFN_vkDestroyPipelineCache destroyPipelineCache{nullptr};
        PFN_vkCreateGraphicsPipelines createGraphicsPipelines{nullptr};
        PFN_vkDestroyPipeline destroyPipeline{nullptr};
        PFN_vkCreateSampler createSampler{nullptr};
        PFN_vkDestroySampler destroySampler{nullptr};
        PFN_vkCreateDescriptorPool createDescriptorPool{nullptr};
        PFN_vkDestroyDescriptorPool destroyDescriptorPool{nullptr};
        PFN_vkAllocateDescriptorSets allocateDescriptorSets{nullptr};
        PFN_vkUpdateDescriptorSets updateDescriptorSets{nullptr};

        Result<VkDescriptorBufferInfo> UniformBinding(const MaterialBindingLayout &, const MaterialResourceBinding &,
                                                      const MaterialResidentResource &) const;
        Result<VkDescriptorImageInfo> ImageBinding(ShaderResourceKind, const MaterialResidentResource &) const;
        Result<VkWriteDescriptorSet> ResolveSlot(const MaterialBindingLayout &, const MaterialResourceBinding &,
                                                 const MaterialResidentResource &, VkDescriptorBufferInfo &, VkDescriptorImageInfo &) const;
        Result<void> AllocateMaterialSets(const Pipeline &, VulkanPreparedMaterialBinding &,
                                          const std::map<VkDescriptorType, std::uint32_t> &) const;

        [[nodiscard]] Error AdmissionError() const {
            return device ? MakeError(VulkanBackendErrors::ResourceRequestInvalid,
                                      "Shader/material preparation requires the render owner thread.")
                          : MakeError(VulkanBackendErrors::NotInitialized,
                                      "Initialize the selected Vulkan device before shader/material preparation.");
        }

        Result<std::size_t> PipelinePayload(const RenderGraphicsPipelineDescriptor &, std::uint64_t, std::uint64_t) const;
        bool LoadDispatch(VkDevice device, const Resolver &resolver);

        const Pipeline *FindMaterialPipeline(std::uint64_t id, const MaterialBindingDescriptor &descriptor, std::size_t count) const {
            const auto found = pipelines.find(id);
            if (found == pipelines.end())
                return nullptr;
            return SameLayout(found->second->material, descriptor.layout) && count == descriptor.resources.size() ? found->second.get()
                                                                                                                  : nullptr;
        }

        [[nodiscard]] bool Ready() const noexcept {
            return device != VK_NULL_HANDLE && owner == std::this_thread::get_id();
        }

        /** @brief Admits a direct read-only graphics binding without compute visibility. */
        static bool BindingSupported(const ShaderReflectedBinding &binding) {
            return DescriptorType(binding.kind) != VK_DESCRIPTOR_TYPE_MAX_ENUM && binding.access == ShaderResourceAccess::ReadOnly &&
                   Stages(binding.stages) != 0 &&
                   !(static_cast<unsigned>(binding.stages) & static_cast<unsigned>(ShaderStageVisibility::Compute));
        }

        /** @brief Keeps final-artifact mappings in the bounded separate descriptor namespace. */
        static bool MappingSupported(const ShaderTargetBindingMapEntry &map) {
            return map.generatedHelperIndex == 0 && map.nativeSpace < 4 && map.nativeBinding < 128 && !map.pairedSampler.IsValid();
        }

        /** @brief Matches the established MeshVertex attribute shape. */
        static bool VertexInputSupported(const ShaderReflectedInterfaceVariable &input) {
            return input.location <= 2 && input.type == ShaderValueType::Float32 && input.rows == 1 && input.arrayCount == 1 &&
                   input.columns == (input.location == 2 ? 2 : 3);
        }

        /** @brief Validates a complete cooked layout within guaranteed Vulkan 1.3 baseline bounds. */
        Result<void> ValidateLayout(const RenderGraphicsPipelineDescriptor &descriptor) const {
            if (!descriptor.manifest.inlineConstants.empty())
                return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceUnsupported,
                                                       "Inline constants require an explicit material value submission contract."));
            if (std::ranges::any_of(descriptor.reflection.bindings, [](const auto &binding) {
                return binding.active && !BindingSupported(binding);
            }))
                return Result<void>::Failure(
                    MakeError(VulkanBackendErrors::ResourceUnsupported,
                              "Vulkan graphics materials require read-only uniform buffers, sampled images and samplers."));
            if (std::ranges::any_of(descriptor.reflection.targetBindings, [](const auto &map) {
                return map.active && !MappingSupported(map);
            }))
                return Result<void>::Failure(
                    MakeError(VulkanBackendErrors::ResourceUnsupported,
                              "Vulkan material ABI requires direct bindings in descriptor sets zero through three."));
            if (std::ranges::any_of(descriptor.reflection.stageInterface, [](const auto &input) {
                return input.stage == ShaderStage::Vertex && input.direction == ShaderInterfaceDirection::Input &&
                       !VertexInputSupported(input);
            }))
                return Result<void>::Failure(
                    MakeError(VulkanBackendErrors::ResourceUnsupported,
                              "Vulkan graphics requires the established MeshVertex location and component contract."));
            return Result<void>::Success();
        }

        /** @brief Computes bounded scalar/vector packing for frontend-admitted uniform buffers. */
        Result<std::size_t> ParameterBytes(const MaterialBindingLayout &layout, ShaderBindingId id) const {
            std::size_t bytes = 0;
            for (const auto &parameter : layout.parameters) {
                if (!parameter.active || parameter.binding != id)
                    continue;
                // Matrices/arrays require complete reflected native tail sizing, not guessed strides.
                if (parameter.arrayCount != 1 || (parameter.rows != 1 && parameter.columns != 1))
                    return Result<std::size_t>::Failure(
                        MakeError(MaterialBindingErrors::Unsupported,
                                  "Vulkan packed material parameters currently require scalar/vector reflection."));
                bytes = std::max(bytes, static_cast<std::size_t>(parameter.byteOffset) + 4 * std::max(parameter.rows, parameter.columns));
            }
            if (bytes == 0 || bytes > properties.limits.maxUniformBufferRange)
                return Result<std::size_t>::Failure(
                    MakeError(VulkanBackendErrors::ResourceRequestInvalid, "Material binding has no finite reflected uniform block."));
            return Result<std::size_t>::Success((bytes + 15) & ~std::size_t{15});
        }
    };

}  // namespace Horo::Render::Detail
