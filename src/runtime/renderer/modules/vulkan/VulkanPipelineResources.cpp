#include "Horo/Runtime/Render/Mesh.h"
#include "VulkanResourceConversions.h"
#include "VulkanShaderRuntimeInternal.h"

namespace Horo::Render::Detail {
    namespace {
        /** @brief Owns fixed-function input pointers through the synchronous native create call. */
        struct GraphicsState {
            VkVertexInputBindingDescription vertexBinding{};
            std::array<VkVertexInputAttributeDescription, 3> attributes{};
            VkPipelineVertexInputStateCreateInfo vertices{};
            VkPipelineInputAssemblyStateCreateInfo assembly{};
            VkPipelineViewportStateCreateInfo viewport{};
            VkPipelineRasterizationStateCreateInfo raster{};
            VkPipelineMultisampleStateCreateInfo samples{};
            VkPipelineDepthStencilStateCreateInfo depth{};
            VkPipelineColorBlendAttachmentState attachment{};
            VkPipelineColorBlendStateCreateInfo blend{};
            std::array<VkDynamicState, 2> dynamicStates{};
            VkPipelineDynamicStateCreateInfo dynamic{};
            VkFormat colorFormat{};
            VkFormat depthFormat{};
            VkPipelineRenderingCreateInfo rendering{};

            void ConfigureVertices() {
                vertexBinding =
                    VkVertexInputBindingDescription{.binding = 0, .stride = sizeof(MeshVertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
                attributes = std::array{VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, position)},
                                        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)},
                                        VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)}};
                vertices =
                    VkPipelineVertexInputStateCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                         .vertexBindingDescriptionCount = 1,
                                                         .pVertexBindingDescriptions = &vertexBinding,
                                                         .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size()),
                                                         .pVertexAttributeDescriptions = attributes.data()};
                assembly = VkPipelineInputAssemblyStateCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
                viewport = VkPipelineViewportStateCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                             .viewportCount = 1,
                                                             .scissorCount = 1};
            }

            void ConfigureRaster(const RenderGraphicsPipelineState &state) {
                VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
                if (state.cull == RenderCullMode::None)
                    cullMode = VK_CULL_MODE_NONE;
                else if (state.cull == RenderCullMode::Front)
                    cullMode = VK_CULL_MODE_FRONT_BIT;
                raster = VkPipelineRasterizationStateCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                                .polygonMode = VK_POLYGON_MODE_FILL,
                                                                .cullMode = cullMode,
                                                                .frontFace = state.frontFace == RenderFrontFace::CounterClockwise
                                                                                 ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                                                 : VK_FRONT_FACE_CLOCKWISE,
                                                                .lineWidth = 1};
                samples = VkPipelineMultisampleStateCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
                ConfigureDepth(state);
                attachment = VkPipelineColorBlendAttachmentState{.blendEnable = state.blend != RenderBlendMode::Disabled,
                                                                 .srcColorBlendFactor = state.blend == RenderBlendMode::Alpha
                                                                                            ? VK_BLEND_FACTOR_SRC_ALPHA
                                                                                            : VK_BLEND_FACTOR_ONE,
                                                                 .dstColorBlendFactor = state.blend == RenderBlendMode::Additive
                                                                                            ? VK_BLEND_FACTOR_ONE
                                                                                            : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                                 .colorBlendOp = VK_BLEND_OP_ADD,
                                                                 .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                                 .dstAlphaBlendFactor = state.blend == RenderBlendMode::Additive
                                                                                            ? VK_BLEND_FACTOR_ONE
                                                                                            : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                                 .alphaBlendOp = VK_BLEND_OP_ADD,
                                                                 .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
                blend = VkPipelineColorBlendStateCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                            .attachmentCount = 1,
                                                            .pAttachments = &attachment};
                dynamicStates = std::array{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
                dynamic = VkPipelineDynamicStateCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                           .dynamicStateCount = 2,
                                                           .pDynamicStates = dynamicStates.data()};
            }

            /** @brief Vulkan depth writes require a native enabled test, even for the logical no-comparison path. */
            void ConfigureDepth(const RenderGraphicsPipelineState &state) {
                depth = VkPipelineDepthStencilStateCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                              .depthTestEnable = state.depthTest || state.depthWrite,
                                                              .depthWriteEnable = state.depthWrite,
                                                              .depthCompareOp =
                                                                  state.depthTest ? Compare(state.depthCompare) : VK_COMPARE_OP_ALWAYS};
            }

            void ConfigureAttachments(const RenderGraphicsPipelineState &state) {
                colorFormat = VulkanResourceConversion::TextureFormat(state.colorFormat);
                depthFormat = state.depthFormat ? VulkanResourceConversion::TextureFormat(*state.depthFormat) : VK_FORMAT_UNDEFINED;
                const bool stencil = state.depthFormat && (*state.depthFormat == RenderTextureFormat::Depth24Stencil8 ||
                                                           *state.depthFormat == RenderTextureFormat::Depth32FloatStencil8);
                rendering = VkPipelineRenderingCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                          .colorAttachmentCount = 1,
                                                          .pColorAttachmentFormats = &colorFormat,
                                                          .depthAttachmentFormat = depthFormat,
                                                          .stencilAttachmentFormat = stencil ? depthFormat : VK_FORMAT_UNDEFINED};
            }

            VkGraphicsPipelineCreateInfo Info(VkPipelineLayout layout, const std::array<VkPipelineShaderStageCreateInfo, 2> &stages) const {
                return {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                        .pNext = &rendering,
                        .stageCount = 2,
                        .pStages = stages.data(),
                        .pVertexInputState = &vertices,
                        .pInputAssemblyState = &assembly,
                        .pViewportState = &viewport,
                        .pRasterizationState = &raster,
                        .pMultisampleState = &samples,
                        .pDepthStencilState = &depth,
                        .pColorBlendState = &blend,
                        .pDynamicState = &dynamic,
                        .layout = layout};
            }
        };
    }  // namespace

    /** @brief Realizes all reflected descriptor set layouts and their owning pipeline layout transactionally. */
    Result<void> VulkanShaderRuntime::Impl::CreateLayouts(Pipeline &pipeline) {
        std::array<std::vector<VkDescriptorSetLayoutBinding>, 4> bindings;
        std::size_t setCount = 0;
        for (const auto &entry : pipeline.material.targetBindings) {
            if (!entry.active)
                continue;
            const auto declaration = std::ranges::find(pipeline.material.resources, entry.id, &ShaderReflectedBinding::id);
            if (declaration == pipeline.material.resources.end())
                return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
            bindings[entry.nativeSpace].push_back({.binding = entry.nativeBinding,
                                                   .descriptorType = DescriptorType(declaration->kind),
                                                   .descriptorCount = declaration->arrayCount,
                                                   .stageFlags = Stages(declaration->stages)});
            setCount = std::max(setCount, static_cast<std::size_t>(entry.nativeSpace + 1));
        }
        pipeline.sets.resize(setCount, VK_NULL_HANDLE);
        for (std::size_t set = 0; set < setCount; ++set) {
            const VkDescriptorSetLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                       .bindingCount = static_cast<std::uint32_t>(bindings[set].size()),
                                                       .pBindings = bindings[set].data()};
            if (const auto nativeResult = native.createDescriptorSetLayout(device, &info, nullptr, &pipeline.sets[set]);
                nativeResult != VK_SUCCESS) {
                pipeline.sets[set] = VK_NULL_HANDLE;
                return Result<void>::Failure(NativeShaderFailure("vkCreateDescriptorSetLayout", nativeResult));
            }
        }
        const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = static_cast<std::uint32_t>(setCount),
                                                .pSetLayouts = pipeline.sets.data()};
        if (const auto nativeResult = native.createPipelineLayout(device, &layout, nullptr, &pipeline.layout); nativeResult != VK_SUCCESS) {
            pipeline.layout = VK_NULL_HANDLE;
            return Result<void>::Failure(NativeShaderFailure("vkCreatePipelineLayout", nativeResult));
        }
        return Result<void>::Success();
    }

    /** @brief Supplies typed runtime overrides or manifest defaults only to their declared visible stages. */
    void VulkanShaderRuntime::Impl::ConfigureStages(const RenderGraphicsPipelineDescriptor &descriptor, std::uint64_t vertex,
                                                    std::uint64_t fragment, StageState &output) const {
        const std::array shaderIds{vertex, fragment};
        for (std::size_t stage = 0; stage < output.stages.size(); ++stage) {
            const auto flag = stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
            for (const auto &input : descriptor.manifest.specializationInputs) {
                if ((Stages(input.stages) & flag) == 0)
                    continue;
                output.entries[stage].push_back(
                    {.constantID = input.id.value, .offset = static_cast<std::uint32_t>(output.data[stage].size() * 4), .size = 4});
                const auto overrideValue = std::ranges::find(descriptor.specializationValues, input.id, &ShaderSpecializationValue::id);
                output.data[stage].push_back(overrideValue == descriptor.specializationValues.end() ? input.defaultValueBits
                                                                                                    : overrideValue->valueBits);
            }
            output.specialization[stage] = {.mapEntryCount = static_cast<std::uint32_t>(output.entries[stage].size()),
                                            .pMapEntries = output.entries[stage].data(),
                                            .dataSize = output.data[stage].size() * 4,
                                            .pData = output.data[stage].data()};
            const auto &shader = shaders.at(shaderIds[stage]);
            output.stages[stage] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                    .stage = static_cast<VkShaderStageFlagBits>(flag),
                                    .module = shader.handle,
                                    .pName = shader.descriptor.entryPoint.c_str(),
                                    .pSpecializationInfo = &output.specialization[stage]};
        }
    }

    /** @copydoc VulkanShaderRuntime::CreateGraphicsPipeline */
    Result<std::uint64_t> VulkanShaderRuntime::CreateGraphicsPipeline(const RenderGraphicsPipelineDescriptor &descriptor,
                                                                      std::uint64_t vertex, std::uint64_t fragment,
                                                                      const RenderMemoryPlacement &placement) {
        const auto cost = QueryGraphicsPipelineMemoryCost(descriptor, vertex, fragment);
        if (cost.HasError())
            return Result<std::uint64_t>::Failure(cost.ErrorValue());
        if (!VulkanResourceConversion::PlacementMatches(cost.Value(), placement))
            return Result<std::uint64_t>::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid));
        try {
            const auto material = PrepareMaterialBindingLayout(descriptor.reflection);
            if (material.HasError())
                return Result<std::uint64_t>::Failure(material.ErrorValue());
            auto pipeline = std::make_shared<Impl::Pipeline>();
            pipeline->device = impl_->device;
            pipeline->state = descriptor.state;
            pipeline->destroy = impl_->native.destroyPipeline;
            pipeline->destroyLayout = impl_->native.destroyPipelineLayout;
            pipeline->destroySetLayout = impl_->native.destroyDescriptorSetLayout;
            pipeline->material = material.Value();
            pipeline->key = ValidateRenderGraphicsPipelineDescriptor(descriptor).Value();
            if (const auto layouts = impl_->CreateLayouts(*pipeline); layouts.HasError())
                return Result<std::uint64_t>::Failure(layouts.ErrorValue());
            Impl::StageState stages;
            impl_->ConfigureStages(descriptor, vertex, fragment, stages);
            GraphicsState state;
            state.ConfigureVertices();
            state.ConfigureRaster(descriptor.state);
            state.ConfigureAttachments(descriptor.state);
            const auto info = state.Info(pipeline->layout, stages.stages);
            // The driver may publish a partial handle on failure; the candidate RAII owner releases it.
            if (const auto nativeResult =
                    impl_->native.createGraphicsPipelines(impl_->device, impl_->cache, 1, &info, nullptr, &pipeline->handle);
                nativeResult != VK_SUCCESS)
                return Result<std::uint64_t>::Failure(NativeShaderFailure("vkCreateGraphicsPipelines", nativeResult));
            const auto id = impl_->issue();
            if (!id.has_value())
                return Result<std::uint64_t>::Failure(
                    MakeError(VulkanBackendErrors::ResourceCreationFailed, "Resource identities exhausted."));
            impl_->pipelines.try_emplace(*id, std::move(pipeline));
            return Result<std::uint64_t>::Success(*id);
        } catch (const std::bad_alloc &) {
            return Result<std::uint64_t>::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
        }
    }

}  // namespace Horo::Render::Detail
