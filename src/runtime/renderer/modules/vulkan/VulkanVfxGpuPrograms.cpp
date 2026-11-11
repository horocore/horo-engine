#include "VulkanVfxGpuInternal.h"

#include <algorithm>
#include <cstring>

namespace Horo::Render::Detail {
    namespace {
        Result<void> Check(VkResult result, const char *operation) {
            return result == VK_SUCCESS ? Result<void>::Success() : Result<void>::Failure(VulkanVfxGpuNativeFailure(operation, result));
        }

        struct Modules final {
            std::shared_ptr<VulkanVfxGpuDevice> owner;
            std::array<VkShaderModule, 3> values{};

            explicit Modules(const std::shared_ptr<VulkanVfxGpuDevice> &device) noexcept : owner(device) {}

            Modules(const Modules &) = delete;
            Modules &operator=(const Modules &) = delete;

            ~Modules() {
                for (auto module : values)
                    if (module)
                        owner->native.destroyModule(owner->device, module, nullptr);
            }
        };

        Result<void> Module(Modules &modules, std::size_t index, const CookedVfxGpuProgram &program, VfxGpuProgramRole role,
                            ShaderStage stage) {
            const auto payload = VfxGpuNativePayload(program.artifact, role, stage);
            if (payload.HasError())
                return Result<void>::Failure(payload.ErrorValue());
            if (payload.Value().size() % 4)
                return Result<void>::Failure(VulkanVfxGpuInvalid("Native particle SPIR-V is not a word stream."));
            std::vector<std::uint32_t> words(payload.Value().size() / 4);
            std::memcpy(words.data(), payload.Value().data(), payload.Value().size());
            const auto entry = stage == ShaderStage::Compute  ? "SimulateParticles"
                               : stage == ShaderStage::Vertex ? "VfxGpuVSMain"
                                                              : "VfxGpuPSMain";
            RenderShaderModuleDescriptor descriptor;
            descriptor.stage = stage;
            descriptor.entryPoint = entry;
            if (!VulkanShaderDetail::HasEntry(words, descriptor))
                return Result<void>::Failure(VulkanVfxGpuInvalid("Particle SPIR-V entry or instruction bounds differ from its package."));
            const VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                .codeSize = payload.Value().size(),
                                                .pCode = words.data()};
            return Check(modules.owner->native.createModule(modules.owner->device, &info, nullptr, &modules.values[index]),
                         "vkCreateShaderModule particle");
        }

        Result<void> Mappings(VulkanVfxGpuKernel &kernel, const CookedVfxGpuPrograms &programs) {
            const auto &compute = programs.simulation.artifact.reflection->reflection.targetBindings;
            const auto &draw = programs.draw.artifact.reflection->reflection.targetBindings;
            if (const auto copy = [](const auto &rows, auto &indices) {
                for (std::size_t i = 0; i < indices.size(); ++i) {
                    const auto found =
                        std::ranges::find(rows, ShaderBindingId{static_cast<std::uint32_t>(i + 1)}, &ShaderTargetBindingMapEntry::id);
                    if (found == rows.end() || !found->active || found->nativeSpace != 0 || found->nativeBinding >= 128)
                        return false;
                    indices[i] = found->nativeBinding;
                }
                return true;
            }; !copy(compute, kernel.computeBindings) || !copy(draw, kernel.drawBindings))
                return Result<void>::Failure(VulkanVfxGpuInvalid("Particle pipeline requires complete actual direct set-zero mappings."));
            return Result<void>::Success();
        }

        Result<void> Layout(VulkanVfxGpuKernel &kernel, bool compute) {
            const auto &owner = *kernel.owner;
            std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
            const auto count = compute ? 5u : 2u;
            for (std::uint32_t i = 0; i < count; ++i)
                bindings[i] = {.binding = compute ? kernel.computeBindings[i] : kernel.drawBindings[i],
                               .descriptorType =
                                   i + 1 == count ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               .descriptorCount = 1,
                               .stageFlags =
                                   static_cast<VkShaderStageFlags>(compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_VERTEX_BIT)};
            auto &set = compute ? kernel.computeSet : kernel.drawSet;
            const VkDescriptorSetLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                       .bindingCount = count,
                                                       .pBindings = bindings.data()};
            if (const auto made =
                    Check(owner.native.createSetLayout(owner.device, &info, nullptr, &set), "vkCreateDescriptorSetLayout particle");
                made.HasError())
                return made;
            const VkPipelineLayoutCreateInfo pipeline{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                      .setLayoutCount = 1,
                                                      .pSetLayouts = &set};
            return Check(owner.native.createLayout(owner.device, &pipeline, nullptr, compute ? &kernel.computeLayout : &kernel.drawLayout),
                         "vkCreatePipelineLayout particle");
        }

        struct DrawState final {
            VkPipelineVertexInputStateCreateInfo vertices{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
            VkPipelineViewportStateCreateInfo viewport{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                       .viewportCount = 1,
                                                       .scissorCount = 1};
            VkPipelineRasterizationStateCreateInfo raster{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                          .polygonMode = VK_POLYGON_MODE_FILL,
                                                          .cullMode = VK_CULL_MODE_NONE,
                                                          .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                          .lineWidth = 1};
            VkPipelineMultisampleStateCreateInfo samples{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                         .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
            VkPipelineDepthStencilStateCreateInfo depth{.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
            VkPipelineColorBlendAttachmentState attachment{.blendEnable = VK_TRUE,
                                                           .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                                           .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                                           .colorBlendOp = VK_BLEND_OP_ADD,
                                                           .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                           .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                           .alphaBlendOp = VK_BLEND_OP_ADD,
                                                           .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
            VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                      .attachmentCount = 1,
                                                      .pAttachments = &attachment};
            std::array<VkDynamicState, 2> values{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                     .dynamicStateCount = 2,
                                                     .pDynamicStates = values.data()};
            VkFormat color{VK_FORMAT_R8G8B8A8_UNORM};
            VkPipelineRenderingCreateInfo rendering{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                    .colorAttachmentCount = 1,
                                                    .pColorAttachmentFormats = &color};

            VkGraphicsPipelineCreateInfo Info(VkPipelineLayout layout, std::span<const VkPipelineShaderStageCreateInfo> stages,
                                              bool hasDepth) {
                depth.depthTestEnable = hasDepth;
                rendering.depthAttachmentFormat = hasDepth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;
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

        Result<void> Pipelines(VulkanVfxGpuKernel &kernel, const Modules &modules) {
            auto &owner = *kernel.owner;
            const VkComputePipelineCreateInfo compute{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                                .module = modules.values[0],
                                                                .pName = "SimulateParticles"},
                                                      .layout = kernel.computeLayout};
            if (const auto made = Check(owner.native.createCompute(owner.device, VK_NULL_HANDLE, 1, &compute, nullptr, &kernel.compute),
                                        "vkCreateComputePipelines particle");
                made.HasError())
                return made;
            const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                                          .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                                          .module = modules.values[1],
                                                                          .pName = "VfxGpuVSMain"},
                                                                         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                                          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                                          .module = modules.values[2],
                                                                          .pName = "VfxGpuPSMain"}}};
            DrawState state;
            for (const auto depth : {false, true}) {
                auto info = state.Info(kernel.drawLayout, stages, depth);
                if (const auto made = Check(owner.native.createGraphics(owner.device, VK_NULL_HANDLE, 1, &info, nullptr,
                                                                        depth ? &kernel.drawDepth : &kernel.drawColor),
                                            "vkCreateGraphicsPipelines particle");
                    made.HasError())
                    return made;
            }
            return Result<void>::Success();
        }
    }  // namespace

    namespace {
        Result<void> PrepareFamily(VulkanVfxGpuKernel &kernel, const CookedVfxGpuPrograms &programs, const Modules &modules) {
            if (auto ready = Mappings(kernel, programs); ready.HasError())
                return ready;
            if (auto ready = Layout(kernel, true); ready.HasError())
                return ready;
            if (auto ready = Layout(kernel, false); ready.HasError())
                return ready;
            return Pipelines(kernel, modules);
        }
    }  // namespace

    VulkanVfxGpuKernel::~VulkanVfxGpuKernel() {
        Reset();
    }

    void VulkanVfxGpuKernel::Reset() noexcept {
        if (!owner)
            return;
        for (auto *pipeline : {&compute, &drawColor, &drawDepth})
            if (*pipeline) {
                owner->native.destroyPipeline(owner->device, *pipeline, nullptr);
                *pipeline = {};
            }
        for (auto *layout : {&computeLayout, &drawLayout})
            if (*layout) {
                owner->native.destroyLayout(owner->device, *layout, nullptr);
                *layout = {};
            }
        for (auto *set : {&computeSet, &drawSet})
            if (*set) {
                owner->native.destroySetLayout(owner->device, *set, nullptr);
                *set = {};
            }
    }

    Result<std::shared_ptr<IResidentVfxGpuKernel>> VulkanVfxGpuRuntime::Programs(const CookedVfxGpuPrograms &programs) {
        using enum Horo::Render::ShaderStage;
        using enum Horo::Render::VfxGpuProgramRole;
        using Outcome = Result<std::shared_ptr<IResidentVfxGpuKernel>>;
        if (const auto access = impl_->Access(); access.HasError())
            return Outcome::Failure(access.ErrorValue());
        if (const auto valid = ValidateCookedVfxGpuPrograms(programs, ShaderTargetBackend::Vulkan); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (const auto retired = impl_->Retire(64); retired.HasError())
            return Outcome::Failure(retired.ErrorValue());
        if (impl_->kernels.size() == impl_->kernels.capacity())
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceCapacityExceeded));
        try {
            auto kernel = std::make_shared<VulkanVfxGpuKernel>();
            kernel->owner = impl_->device;
            Modules modules{impl_->device};
            if (const auto made = Module(modules, 0, programs.simulation, Simulation, Compute); made.HasError())
                return Outcome::Failure(made.ErrorValue());
            if (const auto made = Module(modules, 1, programs.draw, Draw, Vertex); made.HasError())
                return Outcome::Failure(made.ErrorValue());
            if (const auto made = Module(modules, 2, programs.draw, Draw, Fragment); made.HasError())
                return Outcome::Failure(made.ErrorValue());
            if (const auto ready = PrepareFamily(*kernel, programs, modules); ready.HasError())
                return Outcome::Failure(ready.ErrorValue());
            impl_->kernels.push_back(kernel);
            return Outcome::Success(std::move(kernel));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceCreationFailed));
        }
    }
}  // namespace Horo::Render::Detail
