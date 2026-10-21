#pragma once
/** @file VulkanShaderDispatchFixture.h
 * @brief Deterministic native-call counters and failure injection for shader preparation tests.
 */
#include "runtime/renderer/modules/vulkan/VulkanShaderRuntime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Detail;

    template <class T> T Handle(std::uintptr_t value) {
        if constexpr (std::is_pointer_v<T>)
            return reinterpret_cast<T>(value);
        else
            return static_cast<T>(value);
    }

    struct Driver {
        unsigned shaders{}, destroyedShaders{}, layouts{}, destroyedLayouts{}, pipelines{}, destroyedPipelines{}, pools{}, destroyedPools{},
            updates{};
        VkResult shaderResult{VK_SUCCESS}, pipelineResult{VK_SUCCESS}, layoutResult{VK_SUCCESS}, poolResult{VK_SUCCESS},
            allocationResult{VK_SUCCESS};
        std::string_view missing;
        std::uint32_t specialization{};
        std::size_t uniformRange{16};
        bool depthTestEnabled{}, depthWriteEnabled{};
        VkCompareOp depthCompare{};
    };

    Driver *driver{};

    VKAPI_ATTR VkResult VKAPI_CALL CreateShader(VkDevice, const VkShaderModuleCreateInfo *info, const VkAllocationCallbacks *,
                                                VkShaderModule *out) {
        CHECK(info->codeSize >= 20);
        ++driver->shaders;
        *out = Handle<VkShaderModule>(driver->shaders + 10);
        return driver->shaderResult;
    }

    VKAPI_ATTR void VKAPI_CALL DestroyShader(VkDevice, VkShaderModule, const VkAllocationCallbacks *) {
        ++driver->destroyedShaders;
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo *info, const VkAllocationCallbacks *,
                                                   VkDescriptorSetLayout *out) {
        CHECK(info->bindingCount <= 128);
        ++driver->layouts;
        *out = Handle<VkDescriptorSetLayout>(20);
        return driver->layoutResult;
    }

    VKAPI_ATTR void VKAPI_CALL DestroySetLayout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks *) {
        ++driver->destroyedLayouts;
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateLayout(VkDevice, const VkPipelineLayoutCreateInfo *info, const VkAllocationCallbacks *,
                                                VkPipelineLayout *out) {
        CHECK(info->setLayoutCount == 1);
        *out = Handle<VkPipelineLayout>(30);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL DestroyLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks *) {}

    VKAPI_ATTR VkResult VKAPI_CALL CreateCache(VkDevice, const VkPipelineCacheCreateInfo *info, const VkAllocationCallbacks *,
                                               VkPipelineCache *out) {
        CHECK(info->initialDataSize == 0);
        *out = Handle<VkPipelineCache>(40);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL DestroyCache(VkDevice, VkPipelineCache, const VkAllocationCallbacks *) {}

    VKAPI_ATTR VkResult VKAPI_CALL CreatePipeline(VkDevice, VkPipelineCache cache, std::uint32_t count,
                                                  const VkGraphicsPipelineCreateInfo *info, const VkAllocationCallbacks *,
                                                  VkPipeline *out) {
        REQUIRE(cache != VK_NULL_HANDLE);
        CHECK(count == 1);
        CHECK(info->renderPass == VK_NULL_HANDLE);
        REQUIRE(info->pNext != nullptr);
        CHECK(info->stageCount == 2);
        CHECK(info->pVertexInputState->vertexAttributeDescriptionCount == 3);
        const auto *spec = info->pStages[1].pSpecializationInfo;
        REQUIRE(spec != nullptr);
        REQUIRE(spec->mapEntryCount == 1);
        CHECK(spec->pMapEntries[0].constantID == 7);
        driver->specialization = *static_cast<const std::uint32_t *>(spec->pData);
        driver->depthTestEnabled = info->pDepthStencilState->depthTestEnable;
        driver->depthWriteEnabled = info->pDepthStencilState->depthWriteEnable;
        driver->depthCompare = info->pDepthStencilState->depthCompareOp;
        ++driver->pipelines;
        *out = Handle<VkPipeline>(50);
        return driver->pipelineResult;
    }

    VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks *) {
        ++driver->destroyedPipelines;
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(VkDevice, const VkSamplerCreateInfo *, const VkAllocationCallbacks *, VkSampler *out) {
        *out = Handle<VkSampler>(60);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL DestroySampler(VkDevice, VkSampler, const VkAllocationCallbacks *) {}

    VKAPI_ATTR VkResult VKAPI_CALL CreatePool(VkDevice, const VkDescriptorPoolCreateInfo *, const VkAllocationCallbacks *,
                                              VkDescriptorPool *out) {
        ++driver->pools;
        *out = Handle<VkDescriptorPool>(70);
        return driver->poolResult;
    }

    VKAPI_ATTR void VKAPI_CALL DestroyPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks *) {
        ++driver->destroyedPools;
    }

    VKAPI_ATTR VkResult VKAPI_CALL AllocateSets(VkDevice, const VkDescriptorSetAllocateInfo *info, VkDescriptorSet *out) {
        for (std::uint32_t i = 0; i < info->descriptorSetCount; ++i)
            out[i] = Handle<VkDescriptorSet>(80 + i);
        return driver->allocationResult;
    }

    VKAPI_ATTR void VKAPI_CALL UpdateSets(VkDevice, std::uint32_t count, const VkWriteDescriptorSet *writes, std::uint32_t,
                                          const VkCopyDescriptorSet *) {
        REQUIRE(count > 0);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (writes[index].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                REQUIRE(writes[index].pBufferInfo != nullptr);
                CHECK(writes[index].pBufferInfo->range == driver->uniformRange);
            } else {
                REQUIRE(writes[index].pImageInfo != nullptr);
                if (writes[index].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                    CHECK(writes[index].pImageInfo->imageView != VK_NULL_HANDLE);
                else
                    CHECK(writes[index].pImageInfo->sampler != VK_NULL_HANDLE);
            }
        }
        ++driver->updates;
    }

    PFN_vkVoidFunction Resolve(VkDevice, const char *name) {
        if (driver->missing == name)
            return nullptr;
        const std::array entries{std::pair{std::string_view{"vkCreateShaderModule"}, std::bit_cast<PFN_vkVoidFunction>(&CreateShader)},
                                 std::pair{std::string_view{"vkDestroyShaderModule"}, std::bit_cast<PFN_vkVoidFunction>(&DestroyShader)},
                                 std::pair{std::string_view{"vkCreateDescriptorSetLayout"},
                                           std::bit_cast<PFN_vkVoidFunction>(&CreateSetLayout)},
                                 std::pair{std::string_view{"vkDestroyDescriptorSetLayout"},
                                           std::bit_cast<PFN_vkVoidFunction>(&DestroySetLayout)},
                                 std::pair{std::string_view{"vkCreatePipelineLayout"}, std::bit_cast<PFN_vkVoidFunction>(&CreateLayout)},
                                 std::pair{std::string_view{"vkDestroyPipelineLayout"}, std::bit_cast<PFN_vkVoidFunction>(&DestroyLayout)},
                                 std::pair{std::string_view{"vkCreatePipelineCache"}, std::bit_cast<PFN_vkVoidFunction>(&CreateCache)},
                                 std::pair{std::string_view{"vkDestroyPipelineCache"}, std::bit_cast<PFN_vkVoidFunction>(&DestroyCache)},
                                 std::pair{std::string_view{"vkCreateGraphicsPipelines"},
                                           std::bit_cast<PFN_vkVoidFunction>(&CreatePipeline)},
                                 std::pair{std::string_view{"vkDestroyPipeline"}, std::bit_cast<PFN_vkVoidFunction>(&DestroyPipeline)},
                                 std::pair{std::string_view{"vkCreateSampler"}, std::bit_cast<PFN_vkVoidFunction>(&CreateSampler)},
                                 std::pair{std::string_view{"vkDestroySampler"}, std::bit_cast<PFN_vkVoidFunction>(&DestroySampler)},
                                 std::pair{std::string_view{"vkCreateDescriptorPool"}, std::bit_cast<PFN_vkVoidFunction>(&CreatePool)},
                                 std::pair{std::string_view{"vkDestroyDescriptorPool"}, std::bit_cast<PFN_vkVoidFunction>(&DestroyPool)},
                                 std::pair{std::string_view{"vkAllocateDescriptorSets"}, std::bit_cast<PFN_vkVoidFunction>(&AllocateSets)},
                                 std::pair{std::string_view{"vkUpdateDescriptorSets"}, std::bit_cast<PFN_vkVoidFunction>(&UpdateSets)}};
        const auto found = std::ranges::find(entries, std::string_view(name), [](const auto &entry) {
            return entry.first;
        });
        return found == entries.end() ? nullptr : found->second;
    }

}  // namespace
