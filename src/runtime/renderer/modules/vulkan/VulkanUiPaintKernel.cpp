#include "VulkanUiPaintKernel.h"

#include "Horo/Runtime/Render/UiPaintFrameUpload.h"
#include "VulkanResourceConversions.h"
#include "VulkanShaderRuntimeInternal.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Requires actual blendable color and stencil-renderable formats on the selected device. */
        bool SupportsAttachments(const VulkanUiPaintDevice &device, const VkFormat color, const VkFormat stencil) noexcept {
            constexpr auto Color =
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            return color != VK_FORMAT_UNDEFINED && stencil != VK_FORMAT_UNDEFINED && (device.colorFeatures & Color) == Color &&
                   (device.stencilFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        }

        /** @brief Checks exact fixed upload stride and the UI shader's closed descriptor shape against selected physical limits. */
        bool SupportsDescriptors(const VkPhysicalDeviceLimits &limits) noexcept {
            return limits.minUniformBufferOffsetAlignment != 0 &&
                   UiPaintFrameUpdate::ParameterStride % limits.minUniformBufferOffsetAlignment == 0 &&
                   limits.maxUniformBufferRange >= sizeof(UiPaintParameters) && limits.maxDescriptorSetUniformBuffersDynamic >= 1 &&
                   limits.maxPerStageDescriptorUniformBuffers >= 1 && limits.maxPerStageDescriptorSampledImages >= 2 &&
                   limits.maxPerStageDescriptorSamplers >= 1;
        }

        /** @brief Resolves canonical roles into distinct active native slots from the actual offline binding map. */
        std::optional<std::array<std::uint32_t, 4>> ReflectedBindings(const NormalizedShaderReflection &reflection) {
            std::array<std::uint32_t, 4> bindings{};
            for (std::uint32_t index = 0; index < bindings.size(); ++index) {
                const auto binding =
                    std::ranges::find(reflection.targetBindings, ShaderBindingId{index + 1}, &ShaderTargetBindingMapEntry::id);
                if (binding == reflection.targetBindings.end() || !binding->active || binding->nativeSpace != 0 ||
                    binding->generatedHelperIndex != 0 ||
                    std::ranges::find(std::span{bindings}.first(index), binding->nativeBinding) != std::span{bindings}.first(index).end())
                    return std::nullopt;
                bindings[index] = binding->nativeBinding;
            }
            return bindings;
        }

        /** @brief Reverse-order rollback for compiler-verified, caller-owned shader stage creation. */
        struct ShaderModule final {
            VkDevice device;
            PFN_vkDestroyShaderModule destroy;
            VkShaderModule handle{};

            ~ShaderModule() {
                if (handle)
                    destroy(device, handle, nullptr);
            }
        };

        Result<void> LoadStage(const CompiledShaderArtifact &artifact, ShaderStage stage, const char *entry,
                               PFN_vkCreateShaderModule create, ShaderModule &module) {
            const auto payload = UiPaintNativePayload(artifact, stage);
            if (payload.HasError())
                return Result<void>::Failure(payload.ErrorValue());
            const auto bytes = payload.Value();
            if (bytes.size() % sizeof(std::uint32_t) != 0)
                return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
            std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
            std::memcpy(words.data(), bytes.data(), bytes.size());
            if (!VulkanShaderDetail::HasEntry(words, {.stage = stage, .entryPoint = entry}))
                return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
            const VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                .codeSize = bytes.size(),
                                                .pCode = words.data()};
            const auto created = create(module.device, &info, nullptr, &module.handle);
            return created == VK_SUCCESS ? Result<void>::Success()
                                         : Result<void>::Failure(VulkanShaderDetail::NativeShaderFailure("vkCreateShaderModule", created));
        }

        /** @brief Fixed vertex ABI and dynamic viewport/scissor used for all operations of the same kernel. */
        class RasterPipeline final {
        public:
            RasterPipeline(VkFormat color, const std::array<VkPipelineShaderStageCreateInfo, 2> &stages, VkPipelineLayout layout)
                : rendering_{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                             .colorAttachmentCount = 1,
                             .pColorAttachmentFormats = &color_},
                  info_{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                        .pNext = &rendering_,
                        .stageCount = static_cast<std::uint32_t>(stages.size()),
                        .pStages = stages.data(),
                        .pVertexInputState = &vertex_,
                        .pInputAssemblyState = &assembly_,
                        .pViewportState = &viewport_,
                        .pRasterizationState = &raster_,
                        .pMultisampleState = &samples_,
                        .pDepthStencilState = &depth_,
                        .pColorBlendState = &blend_,
                        .pDynamicState = &dynamic_,
                        .layout = layout},
                  color_(color) {}

            [[nodiscard]] const VkGraphicsPipelineCreateInfo &Configure(UiPaintOperation operation, VkFormat stencil) noexcept {
                const bool push = operation == UiPaintOperation::PushStencil;
                const bool pop = operation == UiPaintOperation::PopStencil;
                const bool multiply = operation == UiPaintOperation::MultiplyCoverage;
                const VkStencilOp pass = push  ? VK_STENCIL_OP_INCREMENT_AND_CLAMP
                                         : pop ? VK_STENCIL_OP_DECREMENT_AND_CLAMP
                                               : VK_STENCIL_OP_KEEP;
                rendering_.stencilAttachmentFormat = stencil;
                depth_.stencilTestEnable = stencil != VK_FORMAT_UNDEFINED;
                depth_.front = {VK_STENCIL_OP_KEEP, pass, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xff, (push || pop) ? 0xffU : 0U, 0};
                depth_.back = depth_.front;
                constexpr auto AllColor =
                    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                const auto source = multiply ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
                const auto target = multiply ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                attachment_ = {.blendEnable = !push && !pop,
                               .srcColorBlendFactor = source,
                               .dstColorBlendFactor = target,
                               .colorBlendOp = VK_BLEND_OP_ADD,
                               .srcAlphaBlendFactor = source,
                               .dstAlphaBlendFactor = target,
                               .alphaBlendOp = VK_BLEND_OP_ADD,
                               .colorWriteMask = (push || pop) ? VkColorComponentFlags{} : VkColorComponentFlags{AllColor}};
                return info_;
            }

        private:
            static constexpr VkVertexInputBindingDescription Binding{0, sizeof(UiPaintVertex), VK_VERTEX_INPUT_RATE_VERTEX};
            static constexpr std::array Attributes{VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT,
                                                                                     offsetof(UiPaintVertex, position)},
                                                   VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32_SFLOAT,
                                                                                     offsetof(UiPaintVertex, uv)},
                                                   VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                                                                                     offsetof(UiPaintVertex, color)}};
            static constexpr std::array Dynamic{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
            const VkPipelineVertexInputStateCreateInfo vertex_{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                               .vertexBindingDescriptionCount = 1,
                                                               .pVertexBindingDescriptions = &Binding,
                                                               .vertexAttributeDescriptionCount =
                                                                   static_cast<std::uint32_t>(Attributes.size()),
                                                               .pVertexAttributeDescriptions = Attributes.data()};
            const VkPipelineInputAssemblyStateCreateInfo assembly_{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                                   .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
            const VkPipelineViewportStateCreateInfo viewport_{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                              .viewportCount = 1,
                                                              .scissorCount = 1};
            const VkPipelineRasterizationStateCreateInfo raster_{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                                 .polygonMode = VK_POLYGON_MODE_FILL,
                                                                 .cullMode = VK_CULL_MODE_NONE,
                                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                                 .lineWidth = 1.0F};
            const VkPipelineMultisampleStateCreateInfo samples_{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
            VkPipelineDepthStencilStateCreateInfo depth_{.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            VkPipelineColorBlendAttachmentState attachment_{};
            const VkPipelineColorBlendStateCreateInfo blend_{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                             .attachmentCount = 1,
                                                             .pAttachments = &attachment_};
            const VkPipelineDynamicStateCreateInfo dynamic_{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                            .dynamicStateCount = static_cast<std::uint32_t>(Dynamic.size()),
                                                            .pDynamicStates = Dynamic.data()};
            VkPipelineRenderingCreateInfo rendering_;
            const VkGraphicsPipelineCreateInfo info_;
            const VkFormat color_;
        };
    }  // namespace

    VulkanUiPaintKernel::VulkanUiPaintKernel(const VulkanUiPaintDevice &selected, const Dispatch native,
                                             const std::array<std::uint32_t, 4> bindings, const VkFormat color,
                                             const VkFormat stencil) noexcept
        : device_(selected.device), incarnation_(selected.incarnation), native_(native), bindings_(bindings), color_(color),
          stencil_(stencil) {}

    VulkanUiPaintKernel::~VulkanUiPaintKernel() {
        Revoke();
    }

    /** @copydoc VulkanUiPaintKernel::Create */
    Result<std::shared_ptr<VulkanUiPaintKernel>> VulkanUiPaintKernel::Create(const VulkanUiPaintDevice &selected,
                                                                             const VulkanShaderRuntime::Resolver &resolve,
                                                                             const CookedUiPaintKernel &cooked) {
        using Kernel = Result<std::shared_ptr<VulkanUiPaintKernel>>;
        const auto color = VulkanResourceConversion::TextureFormat(cooked.colorFormat);
        const auto stencil = VulkanResourceConversion::TextureFormat(cooked.stencilFormat);
        if (!resolve || !selected.device || selected.incarnation == 0 || cooked.target.payloadFormat != ShaderPayloadFormat::SpirV16)
            return Kernel::Failure(
                MakeError(UiPaintFrameErrors::Unavailable, "Vulkan UI kernel requires the selected device and SPIR-V 1.6."));
        if (!SupportsAttachments(selected, color, stencil))
            return Kernel::Failure(MakeError(UiPaintFrameErrors::Unavailable,
                                             "Selected Vulkan device cannot blend the exact color or render the exact stencil format."));
        if (!SupportsDescriptors(selected.limits))
            return Kernel::Failure(MakeError(UiPaintFrameErrors::Unavailable,
                                             "Selected Vulkan device cannot represent the fixed UI uniform and sampler ABI."));
        try {
            const auto reflection = ValidateCookedUiPaintKernel(cooked, ShaderTargetBackend::Vulkan);
            if (reflection.HasError())
                return Kernel::Failure(reflection.ErrorValue());
            const auto bindings = ReflectedBindings(reflection.Value());
            if (!bindings)
                return Kernel::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));

            const auto entry = [&selected, &resolve](const char *name) {
                return resolve(selected.device, name);
            };
            const Dispatch native{std::bit_cast<PFN_vkCreateShaderModule>(entry("vkCreateShaderModule")),
                                  std::bit_cast<PFN_vkDestroyShaderModule>(entry("vkDestroyShaderModule")),
                                  std::bit_cast<PFN_vkCreateDescriptorSetLayout>(entry("vkCreateDescriptorSetLayout")),
                                  std::bit_cast<PFN_vkDestroyDescriptorSetLayout>(entry("vkDestroyDescriptorSetLayout")),
                                  std::bit_cast<PFN_vkCreatePipelineLayout>(entry("vkCreatePipelineLayout")),
                                  std::bit_cast<PFN_vkDestroyPipelineLayout>(entry("vkDestroyPipelineLayout")),
                                  std::bit_cast<PFN_vkCreateGraphicsPipelines>(entry("vkCreateGraphicsPipelines")),
                                  std::bit_cast<PFN_vkDestroyPipeline>(entry("vkDestroyPipeline"))};
            if (!native.createModule || !native.destroyModule || !native.createSetLayout || !native.destroySetLayout ||
                !native.createLayout || !native.destroyLayout || !native.createPipelines || !native.destroyPipeline)
                return Kernel::Failure(MakeError(UiPaintFrameErrors::Unavailable, "Selected Vulkan UI pipeline dispatch is incomplete."));
            auto kernel = std::shared_ptr<VulkanUiPaintKernel>{new VulkanUiPaintKernel{selected, native, *bindings, color, stencil}};
            if (auto layouts = kernel->PrepareLayouts(); layouts.HasError())
                return Kernel::Failure(std::move(layouts).ErrorValue());
            if (auto pipelines = kernel->PreparePipelines(cooked.artifact); pipelines.HasError())
                return Kernel::Failure(std::move(pipelines).ErrorValue());
            return Kernel::Success(std::move(kernel));
        } catch (const std::bad_alloc &) {
            return Kernel::Failure(MakeError(VulkanBackendErrors::ResourceCapacityExceeded));
        }
    }

    /** @copydoc VulkanUiPaintKernel::PrepareLayouts */
    Result<void> VulkanUiPaintKernel::PrepareLayouts() {
        constexpr auto Fragment = VK_SHADER_STAGE_FRAGMENT_BIT;
        const std::array bindings{VkDescriptorSetLayoutBinding{bindings_[0], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                                                               VK_SHADER_STAGE_VERTEX_BIT | Fragment, nullptr},
                                  VkDescriptorSetLayoutBinding{bindings_[1], VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, Fragment, nullptr},
                                  VkDescriptorSetLayoutBinding{bindings_[2], VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, Fragment, nullptr},
                                  VkDescriptorSetLayoutBinding{bindings_[3], VK_DESCRIPTOR_TYPE_SAMPLER, 1, Fragment, nullptr}};
        const VkDescriptorSetLayoutCreateInfo set{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  .bindingCount = static_cast<std::uint32_t>(bindings.size()),
                                                  .pBindings = bindings.data()};
        const auto created = native_.createSetLayout(device_, &set, nullptr, &setLayout_);
        if (created != VK_SUCCESS)
            return Result<void>::Failure(VulkanShaderDetail::NativeShaderFailure("vkCreateDescriptorSetLayout", created));
        const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = 1,
                                                .pSetLayouts = &setLayout_};
        const auto result = native_.createLayout(device_, &layout, nullptr, &layout_);
        return result == VK_SUCCESS ? Result<void>::Success()
                                    : Result<void>::Failure(VulkanShaderDetail::NativeShaderFailure("vkCreatePipelineLayout", result));
    }

    /** @copydoc VulkanUiPaintKernel::PreparePipelines */
    Result<void> VulkanUiPaintKernel::PreparePipelines(const CompiledShaderArtifact &artifact) {
        constexpr char Vertex[] = "PaintVertex";
        constexpr char Fragment[] = "PaintFragment";
        ShaderModule vertex{device_, native_.destroyModule};
        ShaderModule fragment{device_, native_.destroyModule};
        if (auto loaded = LoadStage(artifact, ShaderStage::Vertex, Vertex, native_.createModule, vertex); loaded.HasError())
            return loaded;
        if (auto loaded = LoadStage(artifact, ShaderStage::Fragment, Fragment, native_.createModule, fragment); loaded.HasError())
            return loaded;
        const std::array stages{VkPipelineShaderStageCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                                .module = vertex.handle,
                                                                .pName = Vertex},
                                VkPipelineShaderStageCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                                .module = fragment.handle,
                                                                .pName = Fragment}};
        RasterPipeline pipeline{color_, stages, layout_};
        using enum UiPaintOperation;
        const std::array operations{Paint, Paint, PushStencil, PopStencil, MultiplyCoverage, MultiplyCoverage};
        const std::array stencils{VK_FORMAT_UNDEFINED, stencil_, stencil_, stencil_, VK_FORMAT_UNDEFINED, stencil_};
        static_assert(operations.size() == static_cast<std::size_t>(Variant::Count));
        for (std::size_t index = 0; index < operations.size(); ++index) {
            const auto &info = pipeline.Configure(operations[index], stencils[index]);
            const auto made = native_.createPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipelines_[index]);
            if (made != VK_SUCCESS)
                return Result<void>::Failure(VulkanShaderDetail::NativeShaderFailure("vkCreateGraphicsPipelines", made));
        }
        return Result<void>::Success();
    }

    /** @copydoc VulkanUiPaintKernel::Pipeline */
    VkPipeline VulkanUiPaintKernel::Pipeline(const UiPaintOperation operation, const bool stencilAttached) const noexcept {
        using enum UiPaintOperation;
        switch (operation) {
            case Paint:
                return pipelines_[static_cast<std::size_t>(stencilAttached ? Variant::StenciledColor : Variant::Color)];
            case PushStencil:
                return stencilAttached ? pipelines_[static_cast<std::size_t>(Variant::Push)] : VK_NULL_HANDLE;
            case PopStencil:
                return stencilAttached ? pipelines_[static_cast<std::size_t>(Variant::Pop)] : VK_NULL_HANDLE;
            case MultiplyCoverage:
                return pipelines_[static_cast<std::size_t>(stencilAttached ? Variant::StenciledMultiply : Variant::Multiply)];
        }
        return VK_NULL_HANDLE;
    }

    /** @copydoc VulkanUiPaintKernel::Matches */
    bool VulkanUiPaintKernel::Matches(const VkDevice currentDevice, const std::uint64_t currentIncarnation) const noexcept {
        return device_ && device_ == currentDevice && incarnation_ == currentIncarnation;
    }

    /** @copydoc VulkanUiPaintKernel::Revoke */
    void VulkanUiPaintKernel::Revoke() noexcept {
        if (!device_)
            return;
        for (auto &pipeline : pipelines_)
            if (pipeline)
                native_.destroyPipeline(device_, std::exchange(pipeline, VK_NULL_HANDLE), nullptr);
        if (layout_)
            native_.destroyLayout(device_, std::exchange(layout_, VK_NULL_HANDLE), nullptr);
        if (setLayout_)
            native_.destroySetLayout(device_, std::exchange(setLayout_, VK_NULL_HANDLE), nullptr);
        device_ = VK_NULL_HANDLE;
    }
}  // namespace Horo::Render::Detail
