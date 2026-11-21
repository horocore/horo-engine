#include "VulkanStereoCoverageRuntime.h"

#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "VulkanRenderBackendErrors.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        constexpr char Entry[] = "CSReduceCoverage";
        constexpr std::uint32_t GroupAxis = 8;
        constexpr auto CounterBytes = RenderGraphStereoCoverageReduction::CounterBytes;
        constexpr std::size_t MaximumArtifactBytes = 1024U * 1024U;
        using Rectangles = std::array<MultiviewRenderRectangle, 2>;
        using EyeInstances = std::array<std::uint64_t, 2>;

        /** @brief Packs the exact reflected uint4 blocks without exposing host layout padding. */
        auto PackRectangles(const Rectangles &rectangles) {
            const auto &left = rectangles.front();
            const auto &right = rectangles.back();
            return std::array{left.x, left.y, left.width, left.height, right.x, right.y, right.width, right.height};
        }

        /** @brief Validates fixed group dispatch and per-eye area before issuing any Vulkan command. */
        FramebufferExtent Groups(const Rectangles &rectangles) {
            FramebufferExtent extent;
            for (const auto &rect : rectangles) {
                const std::uint64_t area = std::uint64_t{rect.width} * rect.height;
                if (area == 0 || area > std::numeric_limits<std::uint32_t>::max())
                    return {};
                extent.width = std::max(extent.width, 1 + (rect.width - 1) / GroupAxis);
                extent.height = std::max(extent.height, 1 + (rect.height - 1) / GroupAxis);
            }
            return extent;
        }

        /** @brief Resolves exactly one packaged compute stage. The application validates its bound reflection at package intake. */
        Result<std::vector<std::uint32_t>> StageWords(std::span<const std::uint8_t> artifact) {
            using Outcome = Result<std::vector<std::uint32_t>>;
            const auto decoded =
                DecodeShaderArtifactStages(artifact, ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, MaximumArtifactBytes);
            if (decoded.HasError())
                return Outcome::Failure(decoded.ErrorValue());
            const auto &stage = decoded.Value().stages.front();
            if (decoded.Value().stageCount != 1 || stage.stage != ShaderStage::Compute || stage.entryPoint != Entry ||
                stage.bytes.empty() || stage.bytes.size() % sizeof(std::uint32_t) != 0)
                return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid));
            std::vector<std::uint32_t> words(stage.bytes.size() / sizeof(std::uint32_t));
            std::memcpy(words.data(), stage.bytes.data(), stage.bytes.size());
            if (words.front() != 0x07230203U)
                return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid));
            return Outcome::Success(std::move(words));
        }

        /** @brief Checks one sampled view against its exact layer in an owned 2D R32Float coverage array. */
        bool IsCoverageEye(const VulkanDrawViewBinding &view, const std::uint32_t eye) noexcept {
            const auto &shape = view.descriptor;
            return view.view && view.image && shape.format == RenderTextureFormat::R32Float && shape.aspect == RenderTextureAspect::Color &&
                   shape.dimension == RenderTextureViewDimension::TwoD && shape.baseMip == 0 && shape.mipCount == 1 &&
                   shape.baseLayer == eye && shape.layerCount == 1;
        }

        /** @brief Finds the resolved native view instance already retained by the producer's ordinary lease. */
        std::uint64_t ViewInstance(const RenderGraphExecutionRequest &request, RenderTextureViewHandle handle) noexcept {
            const auto found = std::ranges::find(request.views, handle, &RenderGraphTextureViewInstance::view);
            return found != request.views.end() ? found->instance : 0;
        }

        /** @brief Checks the result's exact shape independently of the portable workload's declared use. */
        bool IsCounter(const VulkanGraphResource &resource) noexcept {
            const auto &buffer = resource.bufferDescriptor;
            return resource.buffer && buffer.byteSize == CounterBytes && buffer.access == RenderBufferAccess::HostVisible &&
                   buffer.usage == (RenderBufferUsage::Storage | RenderBufferUsage::CopyDestination);
        }

        /** @brief Checks the exact renderer-owned two-layer sampleable mask shape. */
        bool IsCoverageMask(const VulkanGraphResource &resource) noexcept {
            const auto &texture = resource.textureDescriptor;
            return resource.image && texture.dimension == RenderTextureDimension::TwoD && texture.format == RenderTextureFormat::R32Float &&
                   texture.layerCount == 2 && texture.mipCount == 1 && texture.sampleCount == 1 &&
                   HasTextureUsage(texture.usage, RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment);
        }
    }  // namespace

    class VulkanStereoCoverageRuntime::Kernel final : public IResidentStereoCoverageReducer {
    public:
        Kernel(VkDevice ownedDevice, Dispatch selected, const VulkanStereoCoveragePreparation &request, const VkImage image,
               FramebufferExtent groups)
            : device(ownedDevice), native(selected), queue(request.queue), eyes(request.eyes), rectangles(request.rectangles), mask(image),
              resultInstance(request.result), dispatchGroups(groups) {}

        ~Kernel() override {
            Reset();
        }

        /** @brief Releases native children exactly once while the actual graphics device remains live. */
        void Reset() noexcept {
            if (!device)
                return;
            if (pipeline)
                native.destroyPipeline(device, pipeline, nullptr);
            if (layout)
                native.destroyLayout(device, layout, nullptr);
            if (pool)
                native.destroyPool(device, pool, nullptr);
            if (descriptorLayout)
                native.destroyDescriptorLayout(device, descriptorLayout, nullptr);
            parameters = {};
            counter = {};
            reset = {};
            device = VK_NULL_HANDLE;
        }

        /** @brief Realizes the four closed binding slots and a single immutable set. */
        bool CreateDescriptors(const std::array<VkImageView, 2> &views, const VkBuffer result) {
            constexpr std::array types{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
            std::array<VkDescriptorSetLayoutBinding, types.size()> bindings;
            for (std::uint32_t index = 0; index < types.size(); ++index)
                bindings[index] = {index, types[index], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            const VkDescriptorSetLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                             .bindingCount = static_cast<std::uint32_t>(bindings.size()),
                                                             .pBindings = bindings.data()};
            if (native.createDescriptorLayout(device, &layoutInfo, nullptr, &descriptorLayout) != VK_SUCCESS)
                return false;
            constexpr std::array poolSizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2},
                                           VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                           VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
            const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                      .maxSets = 1,
                                                      .poolSizeCount = static_cast<std::uint32_t>(poolSizes.size()),
                                                      .pPoolSizes = poolSizes.data()};
            if (native.createPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
                return false;
            const VkDescriptorSetAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                         .descriptorPool = pool,
                                                         .descriptorSetCount = 1,
                                                         .pSetLayouts = &descriptorLayout};
            if (native.allocateSet(device, &allocation, &descriptors) != VK_SUCCESS)
                return false;
            const std::array images{VkDescriptorImageInfo{VK_NULL_HANDLE, views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                    VkDescriptorImageInfo{VK_NULL_HANDLE, views[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            const std::array buffers{VkDescriptorBufferInfo{result, 0, CounterBytes},
                                     VkDescriptorBufferInfo{parameters.buffer, 0, sizeof(std::uint32_t) * 8}};
            std::array<VkWriteDescriptorSet, types.size()> writes;
            for (std::uint32_t index = 0; index < types.size(); ++index)
                writes[index] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                 .dstSet = descriptors,
                                 .dstBinding = index,
                                 .descriptorCount = 1,
                                 .descriptorType = types[index],
                                 .pImageInfo = index < images.size() ? &images[index] : nullptr,
                                 .pBufferInfo = index >= images.size() ? &buffers[index - images.size()] : nullptr};
            native.updateSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
            return true;
        }

        /** @brief Creates the pipeline from the single closed prevalidated compute entry. */
        bool CreatePipeline(std::span<const std::uint32_t> words) {
            const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                        .setLayoutCount = 1,
                                                        .pSetLayouts = &descriptorLayout};
            if (native.createLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
                return false;
            const VkShaderModuleCreateInfo moduleInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                      .codeSize = words.size_bytes(),
                                                      .pCode = words.data()};
            VkShaderModule module{};
            if (native.createModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
                return false;
            const VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                                   .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                             .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                             .module = module,
                                                             .pName = Entry},
                                                   .layout = layout};
            const auto result = native.createPipeline(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
            native.destroyModule(device, module, nullptr);
            return result == VK_SUCCESS;
        }

        VkDevice device{};
        Dispatch native;
        RenderQueueId queue;
        EyeInstances eyes;
        Rectangles rectangles;
        VkImage mask{};
        std::uint64_t resultInstance{};
        FramebufferExtent dispatchGroups;
        VulkanCoverageInput reset, parameters, counter;
        VkDescriptorSetLayout descriptorLayout{};
        VkDescriptorPool pool{};
        VkDescriptorSet descriptors{};
        VkPipelineLayout layout{};
        VkPipeline pipeline{};
    };

    /** @copydoc VulkanStereoCoverageRuntime::Initialize */
    void VulkanStereoCoverageRuntime::Initialize(VkDevice device, const VkPhysicalDeviceLimits &limits,
                                                 const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver) {
        if (limits.maxComputeWorkGroupSize[0] < GroupAxis || limits.maxComputeWorkGroupSize[1] < GroupAxis ||
            limits.maxComputeWorkGroupInvocations < GroupAxis * GroupAxis || limits.maxComputeWorkGroupCount[2] < 2 ||
            limits.maxComputeSharedMemorySize < GroupAxis * GroupAxis * sizeof(StereoCoverageTally))
            return;
#define HORO_COVERAGE_LOAD(member, name) native_.member = std::bit_cast<PFN_##name>(resolver(device, #name))
        HORO_COVERAGE_LOAD(createModule, vkCreateShaderModule);
        HORO_COVERAGE_LOAD(destroyModule, vkDestroyShaderModule);
        HORO_COVERAGE_LOAD(createDescriptorLayout, vkCreateDescriptorSetLayout);
        HORO_COVERAGE_LOAD(destroyDescriptorLayout, vkDestroyDescriptorSetLayout);
        HORO_COVERAGE_LOAD(createLayout, vkCreatePipelineLayout);
        HORO_COVERAGE_LOAD(destroyLayout, vkDestroyPipelineLayout);
        HORO_COVERAGE_LOAD(createPipeline, vkCreateComputePipelines);
        HORO_COVERAGE_LOAD(destroyPipeline, vkDestroyPipeline);
        HORO_COVERAGE_LOAD(createPool, vkCreateDescriptorPool);
        HORO_COVERAGE_LOAD(destroyPool, vkDestroyDescriptorPool);
        HORO_COVERAGE_LOAD(allocateSet, vkAllocateDescriptorSets);
        HORO_COVERAGE_LOAD(updateSets, vkUpdateDescriptorSets);
        HORO_COVERAGE_LOAD(bindPipeline, vkCmdBindPipeline);
        HORO_COVERAGE_LOAD(bindSets, vkCmdBindDescriptorSets);
        HORO_COVERAGE_LOAD(dispatch, vkCmdDispatch);
        HORO_COVERAGE_LOAD(barrier, vkCmdPipelineBarrier2);
#undef HORO_COVERAGE_LOAD
        const std::array nativeEntries{native_.createModule != nullptr,
                                       native_.destroyModule != nullptr,
                                       native_.createDescriptorLayout != nullptr,
                                       native_.destroyDescriptorLayout != nullptr,
                                       native_.createLayout != nullptr,
                                       native_.destroyLayout != nullptr,
                                       native_.createPipeline != nullptr,
                                       native_.destroyPipeline != nullptr,
                                       native_.createPool != nullptr,
                                       native_.destroyPool != nullptr,
                                       native_.allocateSet != nullptr,
                                       native_.updateSets != nullptr,
                                       native_.bindPipeline != nullptr,
                                       native_.bindSets != nullptr,
                                       native_.dispatch != nullptr,
                                       native_.barrier != nullptr};
        if (std::ranges::all_of(nativeEntries, std::identity{})) {
            device_ = device;
            maxGroups_ = {limits.maxComputeWorkGroupCount[0], limits.maxComputeWorkGroupCount[1]};
        }
    }

    /** @copydoc VulkanStereoCoverageRuntime::Supported */
    bool VulkanStereoCoverageRuntime::Supported() const noexcept {
        return device_ != VK_NULL_HANDLE;
    }

    /** @copydoc VulkanStereoCoverageRuntime::Realize */
    Result<std::shared_ptr<IResidentStereoCoverageReducer>> VulkanStereoCoverageRuntime::Realize(
        const VulkanStereoCoveragePreparation &request, VulkanResourceRuntime &registry) {
        using Outcome = Result<std::shared_ptr<IResidentStereoCoverageReducer>>;
        const auto groups = Groups(request.rectangles);
        if (!Supported() || !groups.width || !groups.height || groups.width > maxGroups_.width || groups.height > maxGroups_.height ||
            !request.queue.IsValid() || request.reset == request.result || request.result == request.parameters || !request.result)
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid));
        const auto free = std::ranges::find_if(kernels_, [](const auto &entry) {
            return entry.expired();
        });
        if (free == kernels_.end())
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceCapacityExceeded));
        const auto left = registry.ResolveOwnedCoverageView(request.eyes[0]);
        const auto right = registry.ResolveOwnedCoverageView(request.eyes[1]);
        if (left.HasError() || right.HasError() || !IsCoverageEye(left.Value(), 0) || !IsCoverageEye(right.Value(), 1) ||
            left.Value().image != right.Value().image)
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        constexpr std::array<std::byte, CounterBytes> zeros{};
        const auto packed = PackRectangles(request.rectangles);
        auto reset = registry.SealCoverageInput(request.reset, RenderBufferUsage::CopySource, zeros);
        if (reset.HasError())
            return Outcome::Failure(reset.ErrorValue());
        auto parameters = registry.SealCoverageInput(request.parameters, RenderBufferUsage::Uniform, std::as_bytes(std::span{packed}));
        if (parameters.HasError())
            return Outcome::Failure(parameters.ErrorValue());
        auto counter = registry.ExactStereoCoverageCounter(request.result);
        if (counter.HasError())
            return Outcome::Failure(counter.ErrorValue());
        try {
            const auto words = StageWords(request.artifact);
            if (words.HasError())
                return Outcome::Failure(words.ErrorValue());
            auto kernel = std::make_shared<Kernel>(device_, native_, request, left.Value().image, groups);
            kernel->reset = std::move(reset).Value();
            kernel->parameters = std::move(parameters).Value();
            kernel->counter = std::move(counter).Value();
            if (!kernel->CreateDescriptors({left.Value().view, right.Value().view}, kernel->counter.buffer) ||
                !kernel->CreatePipeline(words.Value()))
                return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
            *free = kernel;
            return Outcome::Success(std::move(kernel));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
        }
    }

    /** @copydoc VulkanStereoCoverageRuntime::Prepare */
    Result<VulkanPreparedStereoCoverage> VulkanStereoCoverageRuntime::Prepare(const RenderGraphStereoCoverageReduction &work,
                                                                              const RenderGraphExecutionRequest &request,
                                                                              std::span<const VulkanGraphResource> resources,
                                                                              const RenderQueueId queue,
                                                                              VulkanCommandRetention &retention) const {
        using Outcome = Result<VulkanPreparedStereoCoverage>;
        const auto kernel = std::dynamic_pointer_cast<Kernel>(work.reducer);
        const auto belongs = [&kernel](const auto &candidate) {
            return candidate.lock() == kernel;
        };
        if (!kernel || !kernel->pipeline || kernel->device != device_ || kernel->queue != queue ||
            !std::ranges::any_of(kernels_, belongs) || retention.resources.size() == retention.resources.capacity())
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        const auto &mask = resources[work.mask.value - 1];
        const auto &reset = resources[work.reset.value - 1];
        const auto &result = resources[work.result.value - 1];
        const auto instance = std::ranges::find(request.resources, work.result, &RenderGraphResourceInstance::resource);
        if (instance == request.resources.end() || instance->instance != kernel->resultInstance || !IsCounter(result) ||
            !IsCoverageMask(mask) || result.buffer != kernel->counter.buffer || reset.buffer != kernel->reset.buffer ||
            mask.image != kernel->mask || work.rectangles != kernel->rectangles)
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        for (std::size_t eye = 0; eye < kernel->eyes.size(); ++eye)
            if (ViewInstance(request, work.eyes[eye]) != kernel->eyes[eye] || !work.rectangles[eye].Fits(mask.textureDescriptor.extent))
                return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        retention.resources.push_back(kernel);
        return Outcome::Success({kernel->pipeline, kernel->layout, kernel->descriptors, result.buffer, kernel->dispatchGroups});
    }

    /** @copydoc VulkanStereoCoverageRuntime::Encode */
    void VulkanStereoCoverageRuntime::Encode(VkCommandBuffer command, const VulkanPreparedStereoCoverage &prepared) const noexcept {
        native_.bindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, prepared.pipeline);
        native_.bindSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, prepared.layout, 0, 1, &prepared.descriptors, 0, nullptr);
        native_.dispatch(command, prepared.groups.width, prepared.groups.height, 2);
        const VkBufferMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                             .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                             .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                             .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                             .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                                             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                             .buffer = prepared.result,
                                             .offset = 0,
                                             .size = CounterBytes};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .bufferMemoryBarrierCount = 1,
                                          .pBufferMemoryBarriers = &barrier};
        native_.barrier(command, &dependency);
    }

    /** @copydoc VulkanStereoCoverageRuntime::Reset */
    void VulkanStereoCoverageRuntime::Reset() noexcept {
        for (auto &entry : kernels_) {
            if (const auto kernel = entry.lock())
                kernel->Reset();
            entry.reset();
        }
        device_ = VK_NULL_HANDLE;
    }
}  // namespace Horo::Render::Detail
