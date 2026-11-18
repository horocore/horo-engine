#include "VulkanReconstructionRuntime.h"

#include "VulkanRenderBackendErrors.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <new>
#include <utility>
#include <vector>

namespace Horo::Render::Detail {
    namespace {
        using Image = NativeReconstructionImage;
        constexpr auto ImageCount = RenderGraphNativeReconstruction::ImageCount;
        constexpr auto Workgroup = NativeReconstructionDispatch::GroupExtent;

        /** @brief Preserves the native outcome at the cold realization boundary. */
        Result<void> Created(VkResult result, const char *operation) {
            if (result == VK_SUCCESS)
                return Result<void>::Success();
            const auto &error =
                result == VK_ERROR_DEVICE_LOST ? VulkanBackendErrors::DeviceLost : VulkanBackendErrors::ResourceCreationFailed;
            return Result<void>::Failure(
                MakeError(error, std::format("{} returned Vulkan status {}.", operation, static_cast<int>(result))));
        }

        /** @brief Uses the shader role as the sole source for descriptor type and shader layout. */
        constexpr VkDescriptorType DescriptorType(Image role) noexcept {
            return NativeReconstructionWritesImage(role) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }

        /** @brief Verifies whole-image Vulkan view provenance through the already resolved frontend identity. */
        Result<VkImageView> ImageView(const RenderGraphReconstructionImage &binding, const VulkanGraphResource &texture,
                                      const RenderGraphExecutionRequest &request, const VulkanResourceRuntime &registry) {
            using Outcome = Result<VkImageView>;
            const auto view = std::ranges::find(request.views, binding.view, &RenderGraphTextureViewInstance::view);
            if (view == request.views.end() || !view->instance)
                return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
            const auto captured = registry.MaterialCapture().ResolveDrawView(view->instance);
            if (captured.HasError())
                return Outcome::Failure(captured.ErrorValue());
            const auto &actual = captured.Value();
            const auto &shape = actual.descriptor;
            if (!actual.view || actual.image != texture.image || shape.format != texture.textureDescriptor.format ||
                shape.aspect != RenderTextureAspect::Color || shape.dimension != RenderTextureViewDimension::TwoD || shape.baseMip != 0 ||
                shape.mipCount != 1 || shape.baseLayer != 0 || shape.layerCount != 1)
                return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
            return Outcome::Success(actual.view);
        }
    }  // namespace

    class VulkanReconstructionRuntime::Kernel final : public IResidentNativeReconstructionKernel {
    public:
        Kernel(VkDevice selected, Dispatch dispatch, NativeReconstructionEntry selectedEntry)
            : device(selected), native(dispatch), entry(selectedEntry) {}

        ~Kernel() override {
            Reset();
        }

        /** @brief Destroys resources in reverse order while the drained device owner is still live. */
        void Reset() noexcept {
            if (!device)
                return;
            if (pipeline)
                native.destroyPipeline(device, std::exchange(pipeline, VK_NULL_HANDLE), nullptr);
            if (layout)
                native.destroyLayout(device, std::exchange(layout, VK_NULL_HANDLE), nullptr);
            if (pool)
                native.destroyPool(device, std::exchange(pool, VK_NULL_HANDLE), nullptr);
            if (setLayout)
                native.destroySetLayout(device, std::exchange(setLayout, VK_NULL_HANDLE), nullptr);
            device = VK_NULL_HANDLE;
        }

        /** @brief Creates an exact set layout and allocates every frame/pass rotation before publication. */
        Result<void> PrepareSets(std::uint32_t slotCount) {
            std::array<VkDescriptorSetLayoutBinding, ImageCount> bindings;
            std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0}}};
            std::uint32_t count{};
            for (std::uint32_t slot = 0; slot < ImageCount; ++slot) {
                const auto role = static_cast<Image>(slot);
                if (!NativeReconstructionUsesImage(entry, role))
                    continue;
                const auto type = DescriptorType(role);
                bindings[count++] = {slot, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
                sizes[NativeReconstructionWritesImage(role) ? 1 : 0].descriptorCount += slotCount;
            }
            const VkDescriptorSetLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                             .bindingCount = count,
                                                             .pBindings = bindings.data()};
            if (const auto made = Created(native.createSetLayout(device, &layoutInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
                made.HasError())
                return made;
            const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                      .maxSets = slotCount,
                                                      .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
                                                      .pPoolSizes = sizes.data()};
            if (const auto made = Created(native.createPool(device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool"); made.HasError())
                return made;
            std::vector<VkDescriptorSetLayout> layouts(slotCount, setLayout);
            sets.resize(slotCount);
            const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                       .descriptorPool = pool,
                                                       .descriptorSetCount = slotCount,
                                                       .pSetLayouts = layouts.data()};
            return Created(native.allocateSets(device, &allocate, sets.data()), "vkAllocateDescriptorSets");
        }

        /** @brief Builds the exact interface and compute pipeline from the real cook payload. */
        Result<void> PreparePipeline(std::span<const std::uint8_t> payload) {
            constexpr VkPushConstantRange constants{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(NativeReconstructionDispatch)};
            const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                        .setLayoutCount = 1,
                                                        .pSetLayouts = &setLayout,
                                                        .pushConstantRangeCount = 1,
                                                        .pPushConstantRanges = &constants};
            if (const auto made = Created(native.createLayout(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
                made.HasError())
                return made;
            if (payload.size() % sizeof(std::uint32_t) != 0)
                return Result<void>::Failure(MakeError(NativeReconstructionErrors::InvalidArtifact));
            std::vector<std::uint32_t> words(payload.size() / sizeof(std::uint32_t));
            std::memcpy(words.data(), payload.data(), payload.size());
            RenderShaderModuleDescriptor descriptor;
            descriptor.stage = ShaderStage::Compute;
            descriptor.entryPoint = NativeReconstructionEntryName(entry);
            if (!VulkanShaderDetail::HasEntry(words, descriptor))
                return Result<void>::Failure(MakeError(NativeReconstructionErrors::InvalidArtifact));

            struct Module final {
                VkDevice device;
                PFN_vkDestroyShaderModule destroy;
                VkShaderModule handle{};

                ~Module() {
                    if (handle)
                        destroy(device, handle, nullptr);
                }
            } shader{device, native.destroyModule};

            const VkShaderModuleCreateInfo module{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                  .codeSize = payload.size(),
                                                  .pCode = words.data()};
            if (const auto made = Created(native.createModule(device, &module, nullptr, &shader.handle), "vkCreateShaderModule");
                made.HasError())
                return made;
            const VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                                   .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                             .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                             .module = shader.handle,
                                                             .pName = descriptor.entryPoint.c_str()},
                                                   .layout = layout};
            return Created(native.createPipeline(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "vkCreateComputePipelines");
        }

        VkDevice device;
        Dispatch native;
        NativeReconstructionEntry entry;
        VkDescriptorSetLayout setLayout{};
        VkDescriptorPool pool{};
        VkPipelineLayout layout{};
        VkPipeline pipeline{};
        std::vector<VkDescriptorSet> sets;
    };

    /** @copydoc VulkanReconstructionRuntime::Initialize */
    void VulkanReconstructionRuntime::Initialize(VkDevice device, const VkPhysicalDeviceLimits &limits, std::uint32_t frames,
                                                 const VulkanShaderRuntime::Resolver &resolve) {
        Reset();
        if (limits.maxComputeWorkGroupSize[0] < Workgroup || limits.maxComputeWorkGroupSize[1] < Workgroup ||
            limits.maxComputeWorkGroupCount[2] == 0 || limits.maxComputeWorkGroupSize[2] == 0 ||
            limits.maxComputeWorkGroupInvocations < Workgroup * Workgroup || limits.maxPerStageDescriptorSampledImages < 5 ||
            limits.maxPerStageDescriptorStorageImages < 2 || limits.maxPushConstantsSize < sizeof(NativeReconstructionDispatch))
            return;
        const auto get = [device, &resolve]<typename T>(T &destination, const char *name) {
            destination = std::bit_cast<T>(resolve(device, name));
            return destination != nullptr;
        };
        const std::array loaded{get(native_.createModule, "vkCreateShaderModule"),
                                get(native_.destroyModule, "vkDestroyShaderModule"),
                                get(native_.createSetLayout, "vkCreateDescriptorSetLayout"),
                                get(native_.destroySetLayout, "vkDestroyDescriptorSetLayout"),
                                get(native_.createLayout, "vkCreatePipelineLayout"),
                                get(native_.destroyLayout, "vkDestroyPipelineLayout"),
                                get(native_.createPipeline, "vkCreateComputePipelines"),
                                get(native_.destroyPipeline, "vkDestroyPipeline"),
                                get(native_.createPool, "vkCreateDescriptorPool"),
                                get(native_.destroyPool, "vkDestroyDescriptorPool"),
                                get(native_.allocateSets, "vkAllocateDescriptorSets"),
                                get(native_.updateSets, "vkUpdateDescriptorSets"),
                                get(native_.bindPipeline, "vkCmdBindPipeline"),
                                get(native_.bindSets, "vkCmdBindDescriptorSets"),
                                get(native_.pushConstants, "vkCmdPushConstants"),
                                get(native_.dispatch, "vkCmdDispatch")};
        if (!std::ranges::all_of(loaded, [](bool present) {
            return present;
        }))
            return;
        device_ = device;
        frames_ = frames;
        maxGroups_ = {limits.maxComputeWorkGroupCount[0], limits.maxComputeWorkGroupCount[1]};
    }

    /** @copydoc VulkanReconstructionRuntime::Supported */
    bool VulkanReconstructionRuntime::Supported() const noexcept {
        return device_ != VK_NULL_HANDLE && frames_ != 0 && maxGroups_.IsValid();
    }

    /** @copydoc VulkanReconstructionRuntime::Realize */
    Result<std::shared_ptr<IResidentNativeReconstructionKernel>> VulkanReconstructionRuntime::Realize(
        const CookedNativeReconstructionKernel &cooked) {
        using Outcome = Result<std::shared_ptr<IResidentNativeReconstructionKernel>>;
        if (!Supported())
            return Outcome::Failure(MakeError(NativeReconstructionErrors::Unsupported));
        const auto free = std::ranges::find_if(kernels_, [](const auto &candidate) {
            return candidate.expired();
        });
        if (free == kernels_.end())
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceCapacityExceeded));
        const auto payload = AdmitNativeReconstructionShader(cooked.entry, cooked.target, cooked.artifact);
        if (payload.HasError())
            return Outcome::Failure(payload.ErrorValue());
        try {
            auto kernel = std::make_shared<Kernel>(device_, native_, cooked.entry);
            if (const auto sets = kernel->PrepareSets(frames_ * MaximumPasses); sets.HasError())
                return Outcome::Failure(sets.ErrorValue());
            if (const auto pipeline = kernel->PreparePipeline(payload.Value()); pipeline.HasError())
                return Outcome::Failure(pipeline.ErrorValue());
            *free = kernel;
            return Outcome::Success(std::move(kernel));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
        }
    }

    /** @copydoc VulkanReconstructionRuntime::Prepare */
    Result<VulkanPreparedReconstruction> VulkanReconstructionRuntime::Prepare(const RenderGraphNativeReconstruction &work,
                                                                              const RenderGraphExecutionRequest &request,
                                                                              std::span<const VulkanGraphResource> textures,
                                                                              const VulkanResourceRuntime &registry,
                                                                              VulkanCommandRetention &retention, std::size_t frame,
                                                                              std::size_t pass) {
        using Outcome = Result<VulkanPreparedReconstruction>;
        const auto kernel = std::dynamic_pointer_cast<Kernel>(work.kernel);
        const auto groups = work.dispatch.constants.Groups();
        if (!Supported() || !kernel || !kernel->pipeline || kernel->device != device_ || kernel->entry != work.dispatch.entry ||
            frame >= frames_ || pass >= MaximumPasses || groups.width > maxGroups_.width || groups.height > maxGroups_.height ||
            retention.resources.size() == retention.resources.capacity())
            return Outcome::Failure(MakeError(NativeReconstructionErrors::InvalidDispatch));
        const bool owned = std::ranges::any_of(kernels_, [&](const auto &candidate) {
            return candidate.lock() == kernel;
        });
        if (!owned)
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        const auto set = kernel->sets[frame * MaximumPasses + pass];
        std::array<VkDescriptorImageInfo, ImageCount> images;
        std::array<VkWriteDescriptorSet, ImageCount> writes;
        std::uint32_t count{};
        for (std::uint32_t slot = 0; slot < work.images.size(); ++slot) {
            const auto role = static_cast<Image>(slot);
            if (!NativeReconstructionUsesImage(work.dispatch.entry, role))
                continue;
            const auto &binding = work.images[slot];
            if (!binding.resource.IsValid() || binding.resource.value > textures.size())
                return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid));
            const auto &texture = textures[binding.resource.value - 1];
            if (texture.kind != RenderGraphResourceKind::Texture || !texture.image ||
                !MatchesNativeReconstructionImage(work.dispatch, role, texture.textureDescriptor))
                return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid));
            const auto image = ImageView(binding, texture, request, registry);
            if (image.HasError())
                return Outcome::Failure(image.ErrorValue());
            images[count] = {.imageView = image.Value(),
                             .imageLayout = NativeReconstructionWritesImage(role) ? VK_IMAGE_LAYOUT_GENERAL
                                                                                  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[count] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = set,
                             .dstBinding = slot,
                             .descriptorCount = 1,
                             .descriptorType = DescriptorType(role),
                             .pImageInfo = &images[count]};
            ++count;
        }
        retention.resources.push_back(kernel);
        native_.updateSets(device_, count, writes.data(), 0, nullptr);
        return Outcome::Success({kernel->pipeline, kernel->layout, set});
    }

    /** @copydoc VulkanReconstructionRuntime::Encode */
    void VulkanReconstructionRuntime::Encode(VkCommandBuffer command, const NativeReconstructionDispatch &constants,
                                             const VulkanPreparedReconstruction &prepared) const noexcept {
        const auto groups = constants.Groups();
        native_.bindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, prepared.pipeline);
        native_.bindSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, prepared.layout, 0, 1, &prepared.images, 0, nullptr);
        native_.pushConstants(command, prepared.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        native_.dispatch(command, groups.width, groups.height, 1);
    }

    /** @copydoc VulkanReconstructionRuntime::Reset */
    void VulkanReconstructionRuntime::Reset() noexcept {
        for (auto &weak : kernels_) {
            if (const auto kernel = weak.lock())
                kernel->Reset();
            weak.reset();
        }
        device_ = VK_NULL_HANDLE;
        frames_ = 0;
        maxGroups_ = {};
        native_ = {};
    }
}  // namespace Horo::Render::Detail
