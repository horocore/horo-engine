#pragma once
#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "VulkanRenderBackendErrors.h"
#include "VulkanShaderRuntime.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace Horo::Render::Detail {
    namespace VulkanShaderDetail {
        inline constexpr std::size_t MaximumObjects = 4096;
        /** @brief Preserves a native creation operation and VkResult in its typed resource failure. */
        Error NativeShaderFailure(std::string_view operation, VkResult result);
        RenderMemoryCostPlan Estimate(std::uint64_t kind, std::size_t payload);
        VkShaderStageFlags Stages(ShaderStageVisibility stages);
        VkDescriptorType DescriptorType(ShaderResourceKind kind);
        VkCompareOp Compare(RenderCompareFunction value);
        bool SameLayout(const MaterialBindingLayout &a, const MaterialBindingLayout &b);
    }  // namespace VulkanShaderDetail

    using VulkanShaderDetail::Compare;
    using VulkanShaderDetail::DescriptorType;
    using VulkanShaderDetail::Estimate;
    using VulkanShaderDetail::MaximumObjects;
    using VulkanShaderDetail::NativeShaderFailure;
    using VulkanShaderDetail::SameLayout;
    using VulkanShaderDetail::Stages;

    struct VulkanShaderRuntime::Impl {
        struct Shader {
            VkShaderModule handle{VK_NULL_HANDLE};
            RenderShaderModuleDescriptor descriptor;
            std::size_t bytes{0};
            bool writesLayer{false};
        };

        struct Pipeline {
            Pipeline() = default;
            Pipeline(const Pipeline &) = delete;
            Pipeline &operator=(const Pipeline &) = delete;
            VkDevice device{VK_NULL_HANDLE};
            PFN_vkDestroyPipeline destroy{nullptr};
            PFN_vkDestroyPipelineLayout destroyLayout{nullptr};
            PFN_vkDestroyDescriptorSetLayout destroySetLayout{nullptr};
            VkPipeline handle{VK_NULL_HANDLE};
            VkPipelineLayout layout{VK_NULL_HANDLE};
            std::vector<VkDescriptorSetLayout> sets;
            MaterialBindingLayout material;
            PipelineCacheKey key;
            RenderGraphicsPipelineState state;

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
            VkDevice device{VK_NULL_HANDLE};
            PFN_vkDestroySampler destroy{nullptr};
            Sampler() = default;
            Sampler(const Sampler &) = delete;
            Sampler &operator=(const Sampler &) = delete;

            ~Sampler() {
                if (handle)
                    destroy(device, handle, nullptr);
            }

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
        VulkanShaderOwnerIdentity ownerIdentity;
        bool vertexLayerEnabled{false};
        IssueIdentity issue;
        ResolveResource resolve;
        std::thread::id owner;
        VkPipelineCache cache{VK_NULL_HANDLE};
        std::unordered_map<std::uint64_t, Shader> shaders;
        std::unordered_map<std::uint64_t, std::shared_ptr<Pipeline>> pipelines;
        std::unordered_map<std::uint64_t, std::shared_ptr<Sampler>> samplers;

        /** @brief Value-owned complete native dispatch for this shader/material owner. */
        struct Dispatch {
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
        };

        Dispatch native;

        Result<VkDescriptorBufferInfo> UniformBinding(const MaterialBindingLayout &, const MaterialResourceBinding &,
                                                      const MaterialResidentResource &) const;
        Result<VkDescriptorImageInfo> ImageBinding(ShaderResourceKind, const MaterialResidentResource &) const;
        Result<VkWriteDescriptorSet> ResolveSlot(const MaterialBindingLayout &, const MaterialResourceBinding &,
                                                 const MaterialResidentResource &, VkDescriptorBufferInfo &, VkDescriptorImageInfo &) const;
        Result<void> FinalizeMaterialSets(const Pipeline &, VulkanPreparedMaterialBinding &, const MaterialBindingDescriptor &,
                                          std::span<VkWriteDescriptorSet>, const std::map<VkDescriptorType, std::uint32_t> &) const;
        Result<void> RetainMaterialResource(VulkanPreparedMaterialBinding &, const MaterialBindingLayout &, const MaterialResourceBinding &,
                                            const MaterialResidentResource &) const;
        Result<void> AllocateMaterialSets(const Pipeline &, VulkanPreparedMaterialBinding &,
                                          const std::map<VkDescriptorType, std::uint32_t> &) const;

        [[nodiscard]] Error AdmissionError() const {
            return device ? MakeError(VulkanBackendErrors::ResourceRequestInvalid,
                                      "Shader/material preparation requires the render owner thread.")
                          : MakeError(VulkanBackendErrors::NotInitialized,
                                      "Initialize the selected Vulkan device before shader/material preparation.");
        }

        Result<std::size_t> PipelinePayload(const RenderGraphicsPipelineDescriptor &, std::uint64_t, std::uint64_t) const;
        template <typename ResolverCallable> bool LoadDispatch(VkDevice selectedDevice, const ResolverCallable &resolver);
        void PublishShader(std::uint64_t id, Shader &shader);

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
            return map.generatedHelperIndex == 0 && map.nativeSpace < 4 && map.nativeBinding < 128;
        }

        /** @brief Vulkan's separate image/sampler descriptors cannot alias one set/binding coordinate. */
        static bool NativeBindingsUnique(std::span<const ShaderTargetBindingMapEntry> mappings) noexcept {
            for (std::size_t index = 0; index < mappings.size(); ++index) {
                const auto &entry = mappings[index];
                if (!entry.active)
                    continue;
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (mappings[prior].active && mappings[prior].nativeSpace == entry.nativeSpace &&
                        mappings[prior].nativeBinding == entry.nativeBinding)
                        return false;
            }
            return true;
        }

        /** @brief Matches the established MeshVertex attribute shape. */
        static bool VertexInputSupported(const ShaderReflectedInterfaceVariable &input) {
            return input.location <= 2 && input.type == ShaderValueType::Float32 && input.rows == 1 && input.arrayCount == 1 &&
                   input.columns == (input.location == 2 ? 2 : 3);
        }

        /** @brief Counts actual descriptors, including arrays, without issuing native calls. */
        struct DescriptorCounts {
            std::uint64_t buffers{};
            std::uint64_t images{};
            std::uint64_t samplers{};

            void Add(const ShaderReflectedBinding &binding) noexcept {
                switch (binding.kind) {
                    case ShaderResourceKind::UniformBuffer:
                        buffers += binding.arrayCount;
                        break;
                    case ShaderResourceKind::SampledTexture:
                        images += binding.arrayCount;
                        break;
                    case ShaderResourceKind::Sampler:
                        samplers += binding.arrayCount;
                        break;
                    default:
                        break;  // ValidateLayout rejects all other kinds before this count.
                }
            }

            bool Fits(std::uint32_t maxBuffers, std::uint32_t maxImages, std::uint32_t maxSamplers,
                      std::uint32_t maxResources) const noexcept {
                return buffers <= maxBuffers && images <= maxImages && samplers <= maxSamplers &&
                       buffers + images + samplers <= maxResources;
            }
        };

        /** @brief Rejects legitimate cooked layouts that exceed the exact selected device's descriptor limits. */
        Result<void> ValidateDescriptorLimits(const NormalizedShaderReflection &reflection) const {
            DescriptorCounts total;
            std::array<DescriptorCounts, 2> stages;
            constexpr std::array stageFlags{VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
            for (const auto &binding : reflection.bindings) {
                if (!binding.active)
                    continue;
                total.Add(binding);
                for (std::size_t stage = 0; stage < stages.size(); ++stage)
                    if ((Stages(binding.stages) & stageFlags[stage]) != 0)
                        stages[stage].Add(binding);
            }
            const auto &limits = properties.limits;
            if (!total.Fits(limits.maxDescriptorSetUniformBuffers, limits.maxDescriptorSetSampledImages, limits.maxDescriptorSetSamplers,
                            std::numeric_limits<std::uint32_t>::max()))
                return Result<void>::Failure(
                    MakeError(VulkanBackendErrors::ResourceUnsupported,
                              "Cooked pipeline descriptors exceed the selected Vulkan device's aggregate descriptor limits."));
            for (const auto &stage : stages)
                if (!stage.Fits(limits.maxPerStageDescriptorUniformBuffers, limits.maxPerStageDescriptorSampledImages,
                                limits.maxPerStageDescriptorSamplers, limits.maxPerStageResources))
                    return Result<void>::Failure(
                        MakeError(VulkanBackendErrors::ResourceUnsupported,
                                  "Cooked shader stage exceeds the selected Vulkan device's per-stage descriptor limits."));
            if (std::ranges::any_of(reflection.targetBindings, [&](const auto &entry) {
                return entry.active && entry.nativeSpace >= limits.maxBoundDescriptorSets;
            }))
                return Result<void>::Failure(
                    MakeError(VulkanBackendErrors::ResourceUnsupported,
                              "Cooked descriptor set number exceeds the selected Vulkan device's bound-set limit."));
            return Result<void>::Success();
        }

        /** @brief Validates a complete cooked layout within guaranteed Vulkan 1.3 baseline bounds. */
        Result<void> ValidateLayout(const RenderGraphicsPipelineDescriptor &descriptor) const {
            if (descriptor.state.viewRouting == RenderViewRouting::VertexLayer && !vertexLayerEnabled)
                return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceUnsupported,
                                                       "Vertex layer routing was not enabled on the selected Vulkan device."));
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
            if (!NativeBindingsUnique(descriptor.reflection.targetBindings))
                return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceUnsupported,
                                                       "Vulkan material realization requires separate image/sampler "
                                                       "coordinates; recook a direct target binding map."));
            return ValidateDescriptorLimits(descriptor.reflection);
        }

        /** @brief Applies native block alignment to the shared final-artifact matrix/array packing contract. */
        Result<std::size_t> ParameterBytes(const MaterialBindingLayout &layout, ShaderBindingId id) const {
            auto size = ComputeMaterialParameterBufferByteSize(layout, id);
            if (size.HasError())
                return size;
            const auto bytes = (size.Value() + 15) & ~std::size_t{15};
            if (bytes > properties.limits.maxUniformBufferRange)
                return Result<std::size_t>::Failure(
                    MakeError(VulkanBackendErrors::ResourceRequestInvalid, "Reflected uniform block exceeds the selected device range."));
            return Result<std::size_t>::Success(bytes);
        }
    };

}  // namespace Horo::Render::Detail
