#include "VulkanVfxGpuInternal.h"

#include <algorithm>

namespace Horo::Render::Detail {
    namespace {
        bool EarlierProducer(const VulkanVfxGpuSimulation &simulation, const RenderGraphVfxGpuDraw &draw,
                             const RenderGraphExecutionRequest &request) {
            for (const auto &entry : request.workloads) {
                if (std::get_if<RenderGraphVfxGpuDraw>(&entry.workload) == &draw)
                    return false;
                const auto *compute = std::get_if<RenderGraphVfxGpuCompute>(&entry.workload);
                if (!compute || compute->simulationLease.get() != &simulation)
                    continue;
                if (compute->work.step.targetGeneration == draw.generation &&
                    VulkanVfxGpuInstance(compute->destination, request) == VulkanVfxGpuInstance(draw.particles, request))
                    return true;
            }
            return false;
        }

        bool Texture(const VulkanGraphResource &resource, const FramebufferExtent &extent, RenderTextureFormat format) {
            const auto &d = resource.textureDescriptor;
            const std::array valid{resource.kind == RenderGraphResourceKind::Texture,
                                   resource.colorView != VK_NULL_HANDLE,
                                   d.extent == extent,
                                   d.format == format,
                                   d.dimension == RenderTextureDimension::TwoD,
                                   d.depth == 1,
                                   d.sampleCount == 1,
                                   d.layerCount == 1,
                                   d.mipCount == 1,
                                   HasTextureUsage(d.usage, RenderTextureUsage::RenderAttachment)};
            return std::ranges::all_of(valid, [](bool value) {
                return value;
            });
        }

        Result<void> Attachments(const RenderGraphVfxGpuDraw &draw, std::span<const VulkanGraphResource> resources) {
            if (!Texture(resources[draw.color.value - 1], draw.view.extent, RenderTextureFormat::Rgba8Unorm))
                return Result<void>::Failure(VulkanVfxGpuInvalid("Particle color attachment differs from the actual whole RGBA8 target."));
            if (draw.depth.IsValid() && !Texture(resources[draw.depth.value - 1], draw.view.extent, RenderTextureFormat::Depth32Float))
                return Result<void>::Failure(
                    VulkanVfxGpuInvalid("Particle depth attachment differs from the actual whole Depth32 target."));
            return Result<void>::Success();
        }

        void Draw(VkCommandBuffer command, const VulkanVfxGpuSimulation &simulation, std::size_t state, std::size_t operation,
                  const RenderGraphVfxGpuDraw &draw, std::span<const VulkanGraphResource> resources) noexcept {
            const auto &clear = draw.colorOperations.clearColor;
            const VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                  .imageView = resources[draw.color.value - 1].colorView,
                                                  .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                  .loadOp = draw.colorOperations.loadOperation == AttachmentLoadOperation::Load
                                                                ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                                : VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                  .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                  .clearValue = {.color = {{clear.red, clear.green, clear.blue, clear.alpha}}}};
            const VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                  .imageView =
                                                      draw.depth.IsValid() ? resources[draw.depth.value - 1].colorView : VK_NULL_HANDLE,
                                                  .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                  .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                                  .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                            .renderArea = {{0, 0}, {draw.view.extent.width, draw.view.extent.height}},
                                            .layerCount = 1,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachments = &color,
                                            .pDepthAttachment = draw.depth.IsValid() ? &depth : nullptr};
            auto &native = simulation.owner->native;
            native.beginRendering(command, &rendering);
            native.bindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                draw.depth.IsValid() ? simulation.kernel->drawDepth : simulation.kernel->drawColor);
            const VkViewport viewport{0, 0, static_cast<float>(draw.view.extent.width), static_cast<float>(draw.view.extent.height), 0, 1};
            const VkRect2D scissor{{0, 0}, {draw.view.extent.width, draw.view.extent.height}};
            native.viewport(command, 0, 1, &viewport);
            native.scissor(command, 0, 1, &scissor);
            const auto offset = static_cast<std::uint32_t>(operation * simulation.plan.parameterStrideBytes);
            native.bindSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, simulation.kernel->drawLayout, 0, 1, &simulation.drawSets[state], 1,
                            &offset);
            native.draw(command, 6, simulation.descriptor.layout.capacity, 0, 0);
            native.endRendering(command);
        }
    }  // namespace

    Result<void> VulkanVfxGpuRuntime::Impl::ValidateDraw(const RenderGraphVfxGpuDraw &draw, const RenderGraphExecutionRequest &request,
                                                         std::span<const VulkanGraphResource> resources) const {
        auto found = Simulation(draw.simulationLease);
        if (found.HasError())
            return Result<void>::Failure(found.ErrorValue());
        const auto &simulation = *found.Value();
        const std::array valid{!simulation.stopped,
                               draw.simulation == simulation.id,
                               draw.deviceIncarnation == device->incarnation,
                               !draw.lifetimeCancellation.IsCancellationRequested(),
                               draw.kernel.get() == simulation.kernel.get(),
                               SameVfxGpuDescriptor(simulation.descriptor, draw.descriptor)};
        if (!std::ranges::all_of(valid, [](bool value) {
            return value;
        }))
            return Result<void>::Failure(VulkanVfxGpuInvalid("Particle draw descriptor, generation or native lease is stale."));
        const auto index = VulkanVfxGpuState(simulation, draw.particles, request);
        if (index == simulation.states.size())
            return Result<void>::Failure(VulkanVfxGpuInvalid("Particle draw is not an exact prepared native state buffer."));
        const bool accepted = simulation.states[index].generation == draw.generation && draw.generation <= simulation.generation;
        if (!accepted && !EarlierProducer(simulation, draw, request))
            return Result<void>::Failure(
                VulkanVfxGpuInvalid("Particle draw lacks an accepted generation or exact earlier same-graph producer."));
        if (const auto attachments = Attachments(draw, resources); attachments.HasError())
            return attachments;
        std::array<std::uint32_t, VfxGpuDrawWords::Count> parameters{};
        return PackVfxGpuDrawParameters(simulation.descriptor, draw.view, parameters)
                   ? Result<void>::Success()
                   : Result<void>::Failure(VulkanVfxGpuInvalid("Particle view parameters are unsafe."));
    }

    Result<void> VulkanVfxGpuRuntime::Impl::EncodeDraw(VkCommandBuffer command, VulkanCommandTicket ticket,
                                                       const RenderQueueSubmission &submission, const RenderGraphVfxGpuDraw &draw,
                                                       const RenderGraphExecutionRequest &request,
                                                       std::span<const VulkanGraphResource> resources) {
        if (const auto valid = ValidateDraw(draw, request, resources); valid.HasError())
            return valid;
        auto simulation = std::static_pointer_cast<VulkanVfxGpuSimulation>(draw.simulationLease);
        std::array<std::uint32_t, VfxGpuDrawWords::Count> parameters{};
        if (!PackVfxGpuDrawParameters(simulation->descriptor, draw.view, parameters))
            return Result<void>::Failure(VulkanVfxGpuInvalid("Particle draw parameter packing failed."));
        const auto operation = ReserveVulkanVfxGpuOperation(*simulation, ticket, submission.signal);
        if (operation == simulation->operations.size())
            return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
        WriteVulkanVfxGpuParameters(*simulation, operation, parameters);
        Draw(command, *simulation, VulkanVfxGpuState(*simulation, draw.particles, request), operation, draw, resources);
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
