#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "Horo/Runtime/Render/Mesh.h"
#include "VulkanRenderBackendErrors.h"
#include "VulkanResourceConversions.h"
#include "VulkanShaderRuntimeInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace Horo::Render::Detail {
    namespace VulkanShaderDetail {

        /** @brief Supplies conservative driver-object accounting rather than claiming measured allocation. */
        RenderMemoryCostPlan Estimate(std::uint64_t kind, std::size_t payload) {
            return {.memoryClass = RenderMemoryClass::PersistentDevice,
                    .allocationClass = RenderMemoryAllocationClass::Dedicated,
                    .provenance = RenderMemoryCostProvenance::Estimated,
                    .compatibility = RenderMemoryCompatibilityId{kind},
                    .payloadBytes = payload,
                    .requiredBytes = (payload * 16 + 65536 + 255) & ~std::size_t{255},
                    .alignment = 256};
        }

        /** @brief Converts only admitted graphic stage visibility. */
        VkShaderStageFlags Stages(ShaderStageVisibility stages) {
            const auto bits = static_cast<unsigned>(stages);
            return ((bits & static_cast<unsigned>(ShaderStageVisibility::Vertex)) ? VK_SHADER_STAGE_VERTEX_BIT : 0U) |
                   ((bits & static_cast<unsigned>(ShaderStageVisibility::Fragment)) ? VK_SHADER_STAGE_FRAGMENT_BIT : 0U);
        }

        /** @brief Maps the material resource envelope without relying on native enum ordering. */
        VkDescriptorType DescriptorType(ShaderResourceKind kind) {
            switch (kind) {
                case ShaderResourceKind::UniformBuffer:
                    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                case ShaderResourceKind::SampledTexture:
                    return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                case ShaderResourceKind::Sampler:
                    return VK_DESCRIPTOR_TYPE_SAMPLER;
                default:
                    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
            }
        }

        /** @brief Maps admitted compare values explicitly. */
        VkCompareOp Compare(RenderCompareFunction value) {
            constexpr std::array values{VK_COMPARE_OP_NEVER,
                                        VK_COMPARE_OP_LESS,
                                        VK_COMPARE_OP_LESS_OR_EQUAL,
                                        VK_COMPARE_OP_EQUAL,
                                        VK_COMPARE_OP_GREATER_OR_EQUAL,
                                        VK_COMPARE_OP_GREATER,
                                        VK_COMPARE_OP_NOT_EQUAL,
                                        VK_COMPARE_OP_ALWAYS};
            return values[static_cast<std::size_t>(value)];
        }

        /** @brief Compares canonical logical layouts by every ABI-bearing field. */
        bool SameLayout(const MaterialBindingLayout &a, const MaterialBindingLayout &b) {
            const auto resource = [](const ShaderReflectedBinding &x) {
                return std::tie(x.id, x.kind, x.access, x.arrayCount, x.stages, x.active);
            };
            const auto mapping = [](const ShaderTargetBindingMapEntry &x) {
                return std::tie(x.id, x.generatedHelperIndex, x.nativeSpace, x.nativeBinding, x.pairedSampler, x.active);
            };
            const auto parameter = [](const ShaderReflectedParameter &x) {
                return std::tie(x.id, x.binding, x.type, x.rows, x.columns, x.arrayCount, x.byteOffset, x.arrayStride, x.matrixStride,
                                x.columnMajor, x.active);
            };
            return std::tie(a.backend, a.shaderInterface) == std::tie(b.backend, b.shaderInterface) &&
                   std::ranges::equal(a.resources, b.resources, {}, resource, resource) &&
                   std::ranges::equal(a.targetBindings, b.targetBindings, {}, mapping, mapping) &&
                   std::ranges::equal(a.parameters, b.parameters, {}, parameter, parameter);
        }

        /** @brief Matches a bounded SPIR-V OpEntryPoint instruction to the exact cooked stage/name. */
        bool EntryMatches(std::span<const std::uint32_t> entry, const RenderShaderModuleDescriptor &descriptor) {
            if (entry.size() < 4)
                return false;
            const auto *name = reinterpret_cast<const char *>(entry.data() + 3);
            const auto *end = static_cast<const char *>(std::memchr(name, 0, (entry.size() - 3) * 4));
            if (!end)
                return false;
            const auto model = descriptor.stage == ShaderStage::Vertex ? 0U : 4U;
            return entry[1] == model && std::string_view(name, static_cast<std::size_t>(end - name)) == descriptor.entryPoint;
        }

        /** @brief Rejects malformed instruction streams and mismatched exact entry points before driver calls. */
        bool HasEntry(const std::vector<std::uint32_t> &words, const RenderShaderModuleDescriptor &descriptor) {
            if (words.size() < 5)
                return false;
            if (std::tie(words[0], words[1], words[4]) != std::tuple{0x07230203U, 0x00010600U, 0U} || words[3] == 0)
                return false;
            bool found = false;
            for (std::size_t offset = 5; offset < words.size();) {
                const auto count = words[offset] >> 16;
                const auto opcode = words[offset] & 0xffffU;
                if (count == 0 || count > words.size() - offset)
                    return false;
                if (opcode == 15 && EntryMatches(std::span{words}.subspan(offset, count), descriptor))
                    found = true;
                offset += count;
            }
            return found;
        }
    }  // namespace VulkanShaderDetail

    VulkanShaderRuntime::VulkanShaderRuntime() : impl_(std::make_unique<Impl>()) {}

    VulkanShaderRuntime::~VulkanShaderRuntime() {
        Shutdown();
    }

    VulkanPreparedMaterialBinding::~VulkanPreparedMaterialBinding() {
        if (pool)
            destroyPool(device, pool, nullptr);
    }

    /** @brief Resolves the full shader/material device dispatch before acquiring native objects. */
    bool VulkanShaderRuntime::Impl::LoadDispatch(VkDevice device, const Resolver &resolver) {
        createShaderModule = std::bit_cast<PFN_vkCreateShaderModule>(resolver(device, "vkCreateShaderModule"));
        destroyShaderModule = std::bit_cast<PFN_vkDestroyShaderModule>(resolver(device, "vkDestroyShaderModule"));
        createDescriptorSetLayout = std::bit_cast<PFN_vkCreateDescriptorSetLayout>(resolver(device, "vkCreateDescriptorSetLayout"));
        destroyDescriptorSetLayout = std::bit_cast<PFN_vkDestroyDescriptorSetLayout>(resolver(device, "vkDestroyDescriptorSetLayout"));
        createPipelineLayout = std::bit_cast<PFN_vkCreatePipelineLayout>(resolver(device, "vkCreatePipelineLayout"));
        destroyPipelineLayout = std::bit_cast<PFN_vkDestroyPipelineLayout>(resolver(device, "vkDestroyPipelineLayout"));
        createPipelineCache = std::bit_cast<PFN_vkCreatePipelineCache>(resolver(device, "vkCreatePipelineCache"));
        destroyPipelineCache = std::bit_cast<PFN_vkDestroyPipelineCache>(resolver(device, "vkDestroyPipelineCache"));
        createGraphicsPipelines = std::bit_cast<PFN_vkCreateGraphicsPipelines>(resolver(device, "vkCreateGraphicsPipelines"));
        destroyPipeline = std::bit_cast<PFN_vkDestroyPipeline>(resolver(device, "vkDestroyPipeline"));
        createSampler = std::bit_cast<PFN_vkCreateSampler>(resolver(device, "vkCreateSampler"));
        destroySampler = std::bit_cast<PFN_vkDestroySampler>(resolver(device, "vkDestroySampler"));
        createDescriptorPool = std::bit_cast<PFN_vkCreateDescriptorPool>(resolver(device, "vkCreateDescriptorPool"));
        destroyDescriptorPool = std::bit_cast<PFN_vkDestroyDescriptorPool>(resolver(device, "vkDestroyDescriptorPool"));
        allocateDescriptorSets = std::bit_cast<PFN_vkAllocateDescriptorSets>(resolver(device, "vkAllocateDescriptorSets"));
        updateDescriptorSets = std::bit_cast<PFN_vkUpdateDescriptorSets>(resolver(device, "vkUpdateDescriptorSets"));
        const std::array dispatchAvailable{createShaderModule != nullptr,
                                           destroyShaderModule != nullptr,
                                           createDescriptorSetLayout != nullptr,
                                           destroyDescriptorSetLayout != nullptr,
                                           createPipelineLayout != nullptr,
                                           destroyPipelineLayout != nullptr,
                                           createPipelineCache != nullptr,
                                           destroyPipelineCache != nullptr,
                                           createGraphicsPipelines != nullptr,
                                           destroyPipeline != nullptr,
                                           createSampler != nullptr,
                                           destroySampler != nullptr,
                                           createDescriptorPool != nullptr,
                                           destroyDescriptorPool != nullptr,
                                           allocateDescriptorSets != nullptr,
                                           updateDescriptorSets != nullptr};
        if (!std::ranges::all_of(dispatchAvailable, [](bool present) {
            return present;
        }))
            return false;
        return true;
    }

    /** @copydoc VulkanShaderRuntime::Initialize */
    Result<void> VulkanShaderRuntime::Initialize(VkDevice device, const VkPhysicalDeviceProperties &properties, RenderAdapterId adapter,
                                                 const Resolver &resolver, IssueIdentity issue, ResolveResource resolve) {
        if (impl_->device || !device || !resolver || !issue || !resolve || !adapter.IsValid())
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidConfig));
        impl_->owner = std::this_thread::get_id();
        if (!impl_->LoadDispatch(device, resolver))
            return Result<void>::Failure(
                MakeError(VulkanBackendErrors::EntryPointMissing, "Vulkan shader/material dispatch is incomplete."));
        impl_->properties = properties;
        impl_->issue = std::move(issue);
        impl_->resolve = std::move(resolve);
        Sha256Builder hash;
        static_cast<void>(hash.Update(std::as_bytes(std::span{properties.pipelineCacheUUID})));
        const std::array facts{properties.vendorID, properties.deviceID, properties.driverVersion, properties.apiVersion};
        static_cast<void>(hash.Update(std::as_bytes(std::span{facts})));
        impl_->identity = {.backend = RenderBackendId{"vulkan"},
                           .backendModuleVersion = 1,
                           .adapter = std::move(adapter),
                           .deviceCompatibilityDigest = hash.Finalize(),
                           .driverIdentity = std::to_string(properties.vendorID) + ":" + std::to_string(properties.deviceID),
                           .driverVersion = std::to_string(properties.driverVersion)};
        const VkPipelineCacheCreateInfo info{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        if (impl_->createPipelineCache(device, &info, nullptr, &impl_->cache) != VK_SUCCESS) {
            impl_->cache = VK_NULL_HANDLE;
            return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed, "vkCreatePipelineCache failed."));
        }
        impl_->device = device;
        return Result<void>::Success();
    }

    /** @copydoc VulkanShaderRuntime::Shutdown */
    void VulkanShaderRuntime::Shutdown() noexcept {
        impl_->pipelines.clear();
        for (auto &[id, shader] : impl_->shaders)
            impl_->destroyShaderModule(impl_->device, shader.handle, nullptr);
        impl_->shaders.clear();
        for (auto &[id, sampler] : impl_->samplers)
            impl_->destroySampler(impl_->device, sampler.handle, nullptr);
        impl_->samplers.clear();
        if (impl_->cache)
            impl_->destroyPipelineCache(impl_->device, impl_->cache, nullptr);
        impl_->cache = VK_NULL_HANDLE;
        impl_->device = VK_NULL_HANDLE;
    }

    /** @copydoc VulkanShaderRuntime::QueryPipelineDeviceIdentity */
    Result<RenderPipelineDeviceIdentity> VulkanShaderRuntime::QueryPipelineDeviceIdentity() const {
        if (!impl_->Ready())
            return Result<RenderPipelineDeviceIdentity>::Failure(
                MakeError(VulkanBackendErrors::NotInitialized, "Shader preparation requires the initialized render owner thread."));
        return Result<RenderPipelineDeviceIdentity>::Success(impl_->identity);
    }

    /** @copydoc VulkanShaderRuntime::QueryShaderModuleMemoryCost */
    Result<RenderMemoryCostPlan> VulkanShaderRuntime::QueryShaderModuleMemoryCost(const RenderShaderModuleDescriptor &descriptor,
                                                                                  std::size_t size) const {
        if (!impl_->Ready())
            return Result<RenderMemoryCostPlan>::Failure(impl_->AdmissionError());
        if (const auto valid = ValidateRenderShaderModuleDescriptor(descriptor); valid.HasError())
            return Result<RenderMemoryCostPlan>::Failure(valid.ErrorValue());
        const std::array supported{descriptor.target.backend == ShaderTargetBackend::Vulkan,
                                   descriptor.target.payloadFormat == ShaderPayloadFormat::SpirV16,
                                   descriptor.target.descriptorVersion == 1,
                                   descriptor.target.interfaceSchemaVersion == 1,
                                   descriptor.stage == ShaderStage::Vertex || descriptor.stage == ShaderStage::Fragment,
                                   size >= 20,
                                   size % 4 == 0,
                                   size <= 64U * 1024U * 1024U,
                                   impl_->shaders.size() < MaximumObjects};
        if (!std::ranges::all_of(supported, [](bool value) {
            return value;
        }))
            return Result<RenderMemoryCostPlan>::Failure(
                MakeError(VulkanBackendErrors::ResourceUnsupported,
                          "Vulkan requires bounded SPIR-V 1.6 vertex/fragment artifacts with version-one metadata."));
        return Result<RenderMemoryCostPlan>::Success(Estimate(40, size));
    }

    /** @copydoc VulkanShaderRuntime::CreateShaderModule */
    Result<std::uint64_t> VulkanShaderRuntime::CreateShaderModule(const RenderShaderModuleDescriptor &descriptor,
                                                                  std::span<const std::uint8_t> code,
                                                                  const RenderMemoryPlacement &placement) {
        const auto cost = QueryShaderModuleMemoryCost(descriptor, code.size());
        if (cost.HasError())
            return Result<std::uint64_t>::Failure(cost.ErrorValue());
        if (!VulkanResourceConversion::PlacementMatches(cost.Value(), placement))
            return Result<std::uint64_t>::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid));
        try {
            std::vector<std::uint32_t> words(code.size() / 4);
            std::ranges::transform(code, std::as_writable_bytes(std::span{words}).begin(), [](std::uint8_t value) {
                return static_cast<std::byte>(value);
            });
            if (!HasEntry(words, descriptor))
                return Result<std::uint64_t>::Failure(
                    MakeError(VulkanBackendErrors::ResourceRequestInvalid,
                              "SPIR-V header, instruction bounds or exact stage entry point is invalid; recook the artifact."));
            Impl::Shader shader{.descriptor = descriptor, .bytes = code.size()};
            const auto id = impl_->issue();
            if (!id)
                return Result<std::uint64_t>::Failure(
                    MakeError(VulkanBackendErrors::ResourceCreationFailed, "Resource identities exhausted."));
            const VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                .codeSize = code.size(),
                                                .pCode = words.data()};
            if (impl_->createShaderModule(impl_->device, &info, nullptr, &shader.handle) != VK_SUCCESS)
                return Result<std::uint64_t>::Failure(
                    MakeError(VulkanBackendErrors::ResourceCreationFailed, "vkCreateShaderModule rejected cooked SPIR-V."));
            try {
                impl_->shaders.emplace(*id, std::move(shader));
            } catch (...) {
                impl_->destroyShaderModule(impl_->device, shader.handle, nullptr);
                throw;
            }
            return Result<std::uint64_t>::Success(*id);
        } catch (const std::bad_alloc &) {
            return Result<std::uint64_t>::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
        }
    }

    /** @brief Matches native stage identity and the exact initialized device before quoting pipeline backing. */
    Result<std::size_t> VulkanShaderRuntime::Impl::PipelinePayload(const RenderGraphicsPipelineDescriptor &descriptor, std::uint64_t vertex,
                                                                   std::uint64_t fragment) const {
        const auto v = shaders.find(vertex);
        const auto f = shaders.find(fragment);
        if (v == shaders.end() || f == shaders.end())
            return Result<std::size_t>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        const auto targetFacts = [](const ShaderTargetRequirement &t) {
            return std::tie(t.backend, t.payloadFormat, t.descriptorVersion, t.interfaceSchemaVersion, t.maximumBindings,
                            t.maximumInlineConstantBytes, t.supportsCompute, t.supportsStorageResources);
        };
        const auto matches = [&](const Impl::Shader &shader, ShaderStage stage) {
            return std::tie(shader.descriptor.stage, shader.descriptor.artifactKey, shader.descriptor.shaderInterface) ==
                       std::tie(stage, descriptor.compatibility.shaderArtifactKey, descriptor.reflection.interfaceCompatibility) &&
                   targetFacts(shader.descriptor.target) == targetFacts(descriptor.target) &&
                   std::ranges::any_of(descriptor.manifest.entryPoints, [&](const auto &entry) {
                return std::tie(entry.stage, entry.name) == std::tie(stage, shader.descriptor.entryPoint);
            });
        };
        const auto &actual = identity;
        const auto &expected = descriptor.compatibility;
        if (!matches(v->second, ShaderStage::Vertex) || !matches(f->second, ShaderStage::Fragment) ||
            std::tie(actual.backend, actual.backendModuleVersion, actual.adapter, actual.deviceCompatibilityDigest, actual.driverIdentity,
                     actual.driverVersion) != std::tie(expected.backend, expected.backendModuleVersion, expected.adapter,
                                                       expected.deviceCompatibilityDigest, expected.driverIdentity, expected.driverVersion))
            return Result<std::size_t>::Failure(
                MakeError(VulkanBackendErrors::ResourceIdentityInvalid,
                          "Pipeline stages or cache compatibility differ from the exact selected Vulkan device/artifact."));
        return Result<std::size_t>::Success(v->second.bytes + f->second.bytes);
    }

    /** @copydoc VulkanShaderRuntime::QueryGraphicsPipelineMemoryCost */
    Result<RenderMemoryCostPlan> VulkanShaderRuntime::QueryGraphicsPipelineMemoryCost(const RenderGraphicsPipelineDescriptor &descriptor,
                                                                                      std::uint64_t vertex, std::uint64_t fragment) const {
        if (!impl_->Ready())
            return Result<RenderMemoryCostPlan>::Failure(impl_->AdmissionError());
        if (const auto valid = ValidateRenderGraphicsPipelineDescriptor(descriptor); valid.HasError())
            return Result<RenderMemoryCostPlan>::Failure(valid.ErrorValue());
        const auto payload = impl_->PipelinePayload(descriptor, vertex, fragment);
        if (payload.HasError())
            return Result<RenderMemoryCostPlan>::Failure(payload.ErrorValue());
        if (descriptor.target.backend != ShaderTargetBackend::Vulkan || impl_->pipelines.size() >= MaximumObjects ||
            descriptor.state.sampleCount != 1)
            return Result<RenderMemoryCostPlan>::Failure(MakeError(VulkanBackendErrors::ResourceUnsupported));
        if (const auto valid = impl_->ValidateLayout(descriptor); valid.HasError())
            return Result<RenderMemoryCostPlan>::Failure(valid.ErrorValue());
        return Result<RenderMemoryCostPlan>::Success(Estimate(41, payload.Value()));
    }

    /** @copydoc VulkanShaderRuntime::QuerySamplerMemoryCost */
    Result<RenderMemoryCostPlan> VulkanShaderRuntime::QuerySamplerMemoryCost(const RenderSamplerDescriptor &descriptor) const {
        if (!impl_->Ready())
            return Result<RenderMemoryCostPlan>::Failure(impl_->AdmissionError());
        if (!descriptor.IsValid() || descriptor.maximumAnisotropy != 1 || impl_->samplers.size() >= MaximumObjects)
            return Result<RenderMemoryCostPlan>::Failure(
                MakeError(VulkanBackendErrors::ResourceUnsupported, "Vulkan baseline samplers require anisotropy one."));
        return Result<RenderMemoryCostPlan>::Success(Estimate(42, 256));
    }

    /** @copydoc VulkanShaderRuntime::CreateSampler */
    Result<std::uint64_t> VulkanShaderRuntime::CreateSampler(const RenderSamplerDescriptor &descriptor,
                                                             const RenderMemoryPlacement &placement) {
        const auto cost = QuerySamplerMemoryCost(descriptor);
        if (cost.HasError())
            return Result<std::uint64_t>::Failure(cost.ErrorValue());
        if (!VulkanResourceConversion::PlacementMatches(cost.Value(), placement))
            return Result<std::uint64_t>::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid));
        constexpr std::array addresses{VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                       VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
        const VkSamplerCreateInfo info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                       .magFilter =
                                           descriptor.magFilter == RenderSamplerFilter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                       .minFilter =
                                           descriptor.minFilter == RenderSamplerFilter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                       .mipmapMode = descriptor.mipmapMode == RenderSamplerMipmapMode::Linear
                                                         ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                         : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                       .addressModeU = addresses[static_cast<std::size_t>(descriptor.addressU)],
                                       .addressModeV = addresses[static_cast<std::size_t>(descriptor.addressV)],
                                       .addressModeW = addresses[static_cast<std::size_t>(descriptor.addressW)],
                                       .compareEnable = descriptor.compareEnabled,
                                       .compareOp = Compare(descriptor.compare),
                                       .minLod = descriptor.minimumLod,
                                       .maxLod = descriptor.maximumLod,
                                       .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
        const auto id = impl_->issue();
        if (!id)
            return Result<std::uint64_t>::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
        Impl::Sampler sampler{.descriptor = descriptor};
        if (impl_->createSampler(impl_->device, &info, nullptr, &sampler.handle) != VK_SUCCESS)
            return Result<std::uint64_t>::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed, "vkCreateSampler failed."));
        try {
            impl_->samplers.emplace(*id, sampler);
        } catch (const std::bad_alloc &) {
            impl_->destroySampler(impl_->device, sampler.handle, nullptr);
            return Result<std::uint64_t>::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
        }
        return Result<std::uint64_t>::Success(*id);
    }

    /** @copydoc VulkanShaderRuntime::DestroyShaderModule */
    void VulkanShaderRuntime::DestroyShaderModule(std::uint64_t id) noexcept {
        const auto found = impl_->shaders.find(id);
        if (found != impl_->shaders.end()) {
            impl_->destroyShaderModule(impl_->device, found->second.handle, nullptr);
            impl_->shaders.erase(found);
        }
    }

    /** @copydoc VulkanShaderRuntime::DestroyGraphicsPipeline */
    void VulkanShaderRuntime::DestroyGraphicsPipeline(std::uint64_t id) noexcept {
        impl_->pipelines.erase(id);
    }

    /** @copydoc VulkanShaderRuntime::DestroySampler */
    void VulkanShaderRuntime::DestroySampler(std::uint64_t id) noexcept {
        const auto found = impl_->samplers.find(id);
        if (found != impl_->samplers.end()) {
            impl_->destroySampler(impl_->device, found->second.handle, nullptr);
            impl_->samplers.erase(found);
        }
    }

}  // namespace Horo::Render::Detail
