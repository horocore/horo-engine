#include "VulkanGraphRuntimeInternal.h"

#include <algorithm>
#include <cmath>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Requires defined read contents and preserves the graph's queue-ownership authority. */
        Result<void> ValidateMaterialPriorState(const VulkanGraphResourceState &state, RenderQueueId queue, bool requireContents) {
            if (requireContents && !state.contentsDefined)
                return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid,
                                                       "Material read or attachment Load requires defined native contents."));
            if (state.hasNativeState && state.logical.queue != queue)
                return Result<void>::Failure(
                    MakeError(VulkanBackendErrors::UnsupportedOperation,
                              "Material queue ownership requires an explicit graph release/acquire before the pass."));
            return Result<void>::Success();
        }

        /** @brief Checks the complete offscreen attachment policy independently of state mutation. */
        bool ValidMaterialAttachmentRequest(const RenderMaterialExecutionRequest &request) {
            const bool validDepth = !request.depthClear.has_value() ||
                                    (std::isfinite(*request.depthClear) && *request.depthClear >= 0 && *request.depthClear <= 1);
            return request.color.IsValid() && request.color.storeOperation == AttachmentStoreOperation::Store &&
                   request.color.loadOperation != AttachmentLoadOperation::DontCare && validDepth;
        }

        /** @brief Builds the conservative whole-buffer read state used for all fixed graphics inputs. */
        RenderGraphLogicalState MaterialInputState(RenderQueueId queue) {
            return {RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                    RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::NotApplicable, queue};
        }
    }  // namespace

    /** @brief Prepares one whole-resource state; queue transfers require the graph's explicit matched release path. */
    Result<void> VulkanGraphRuntime::Impl::MaterialState(const VulkanGraphResource &resource, const RenderGraphLogicalState next,
                                                         const RenderQueueSubmission &submission, const bool requireContents) {
        if (resource.state == nullptr)
            return Result<void>::Failure(Invalid("Material backing has no canonical resource state."));
        if (const auto existing = std::ranges::find(updates, resource.state, &Update::target); existing != updates.end())
            return existing->value.logical == next ? Result<void>::Success()
                                                   : Result<void>::Failure(Invalid("A material pass aliases incompatible resource roles."));
        if (updates.size() == RenderGraphLimits::HardMaxResources)
            return Result<void>::Failure(Invalid("Material resource-state storage is full."));
        const auto &before = *resource.state;
        if (auto valid = ValidateMaterialPriorState(before, submission.queue, requireContents); valid.HasError())
            return valid;
        auto barrier = TranslateVulkanGraphBarrier(resource, before.hasNativeState ? before.logical : RenderGraphLogicalState{}, next);
        if (barrier.HasError())
            return Result<void>::Failure(barrier.ErrorValue());
        barriers.push_back(barrier.Value());
        updates.push_back({resource.state, {next, submission.signal, true, true}});
        return Result<void>::Success();
    }

    /** @brief Captures attachments and validates preservation before draw resources are inspected. */
    Result<void> VulkanGraphRuntime::Impl::PrepareMaterialTarget(const RenderMaterialExecutionRequest &request,
                                                                 VulkanCommandRetention &pins) {
        const auto &submission = *request.submission;
        const auto target = resources->MaterialCapture().CaptureMaterialTarget(request.target, pins);
        if (target.HasError())
            return Result<void>::Failure(target.ErrorValue());
        materialTarget = target.Value();
        if (materialTarget.extent != request.extent || !ValidMaterialAttachmentRequest(request))
            return Result<void>::Failure(Invalid("Material extent, attachment preservation or depth clear is invalid."));
        const RenderGraphLogicalState color{RenderGraphSynchronizationAccess::ReadWrite,
                                            RenderGraphSynchronizationOperation::ColorAttachment, RenderGraphPipelineScope::Graphics,
                                            RenderGraphTextureLayout::ColorAttachment, submission.queue};
        if (auto prepared =
                MaterialState(materialTarget.color, color, submission, request.color.loadOperation == AttachmentLoadOperation::Load);
            prepared.HasError())
            return prepared;
        if (materialTarget.depthView) {
            const RenderGraphLogicalState depth{RenderGraphSynchronizationAccess::ReadWrite,
                                                RenderGraphSynchronizationOperation::DepthStencilAttachment,
                                                RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::DepthStencilAttachment,
                                                submission.queue};
            if (auto prepared = MaterialState(materialTarget.depth, depth, submission, !request.depthClear.has_value());
                prepared.HasError())
                return prepared;
        }
        return Result<void>::Success();
    }

    /** @brief Validates each immutable descriptor backing and prepares its exact read state. */
    Result<void> VulkanGraphRuntime::Impl::PrepareMaterialResources(const VulkanPreparedMaterialBinding &material,
                                                                    const RenderQueueSubmission &submission) {
        const auto input = MaterialInputState(submission.queue);
        for (const auto &resource : material.resources) {
            if (!resource.backing)
                return Result<void>::Failure(Invalid("Material descriptor has no retained native backing."));
            VulkanGraphResource captured{.kind = resource.image ? RenderGraphResourceKind::Texture : RenderGraphResourceKind::Buffer,
                                         .buffer = resource.buffer,
                                         .image = resource.image,
                                         .bufferDescriptor = resource.bufferDescriptor,
                                         .textureDescriptor = resource.textureDescriptor,
                                         .state = resource.state};
            auto next = input;
            if (resource.image) {
                next.operation = RenderGraphSynchronizationOperation::Sampled;
                next.layout = RenderGraphTextureLayout::ShaderReadOnly;
            }
            if (auto prepared = MaterialState(captured, next, submission, true); prepared.HasError())
                return prepared;
        }
        return Result<void>::Success();
    }

    /** @brief Checks pipeline/target compatibility and captures the exact indexed mesh backing. */
    Result<void> VulkanGraphRuntime::Impl::PrepareMaterialDraw(const RenderMaterialDrawInstance &source, MaterialDraw &draw,
                                                               const RenderQueueSubmission &submission, VulkanCommandRetention &pins) {
        const auto input = MaterialInputState(submission.queue);
        draw.material = dynamic_cast<const VulkanPreparedMaterialBinding *>(source.material);
        if (!draw.material)
            return Result<void>::Failure(Invalid("Material has no Vulkan native binding."));
        if (const std::array validIdentity{draw.material->deviceIncarnation == deviceIncarnation, draw.material->owner != nullptr,
                                           draw.material->pipelineBacking != nullptr, draw.material->pipeline != VK_NULL_HANDLE,
                                           draw.material->layout != VK_NULL_HANDLE};
            !std::ranges::all_of(validIdentity, [](bool valid) {
            return valid;
        }))
            return Result<void>::Failure(Invalid("Material belongs to a different or retired selected-device incarnation."));
        const auto &state = draw.material->state;
        const auto depthFormat = materialTarget.depthView ? std::optional{materialTarget.depth.textureDescriptor.format} : std::nullopt;
        if (const std::array formats{state.colorFormat == materialTarget.color.textureDescriptor.format, state.depthFormat == depthFormat,
                                     state.sampleCount == 1};
            !std::ranges::all_of(formats, [](bool valid) {
            return valid;
        }))
            return Result<void>::Failure(Invalid("Material pipeline attachment formats differ from the captured target."));
        const auto mesh = resources->MaterialCapture().CaptureMaterialMesh(source.mesh, pins);
        if (mesh.HasError())
            return Result<void>::Failure(mesh.ErrorValue());
        draw.mesh = mesh.Value();
        if (draw.mesh.descriptor.vertexStride != sizeof(MeshVertex))
            return Result<void>::Failure(Invalid("Material mesh does not use the canonical MeshVertex layout."));
        if (auto prepared = MaterialState(draw.mesh.vertex, input, submission, true); prepared.HasError())
            return prepared;
        if (auto prepared = MaterialState(draw.mesh.index, input, submission, true); prepared.HasError())
            return prepared;
        return PrepareMaterialResources(*draw.material, submission);
    }

    /** @brief Captures every backing and validates immutable attachment/input contracts before recording. */
    Result<void> VulkanGraphRuntime::Impl::PrepareMaterial(const RenderMaterialExecutionRequest &request, VulkanCommandRetention &pins) {
        barriers.clear();
        updates.clear();
        submissions.clear();
        if (auto target = PrepareMaterialTarget(request, pins); target.HasError())
            return target;
        for (std::size_t index = 0; index < request.draws.size(); ++index)
            if (auto draw = PrepareMaterialDraw(request.draws[index], materialDraws[index], *request.submission, pins); draw.HasError())
                return draw;
        return Result<void>::Success();
    }

    /** @brief Encodes prevalidated state, descriptors and indexed MeshVertex draws through the sole command owner. */
    void VulkanGraphRuntime::Impl::EncodeMaterial(const VkCommandBuffer command,
                                                  const RenderMaterialExecutionRequest &request) const noexcept {
        Barriers(command, {0, barriers.size()});
        const auto &clear = request.color.clearColor;
        const VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = materialTarget.colorView,
                                              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .loadOp = request.color.loadOperation == AttachmentLoadOperation::Clear
                                                            ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                            : VK_ATTACHMENT_LOAD_OP_LOAD,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                              .clearValue = {.color = {{clear.red, clear.green, clear.blue, clear.alpha}}}};
        const VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = materialTarget.depthView,
                                              .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                              .loadOp =
                                                  request.depthClear.has_value() ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                              .clearValue = {.depthStencil = {request.depthClear.value_or(1.0F), 0}}};
        const auto depthFormat = materialTarget.depth.textureDescriptor.format;
        const bool stencil =
            depthFormat == RenderTextureFormat::Depth24Stencil8 || depthFormat == RenderTextureFormat::Depth32FloatStencil8;
        const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                        .renderArea = {{0, 0}, {request.extent.width, request.extent.height}},
                                        .layerCount = 1,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachments = &color,
                                        .pDepthAttachment = materialTarget.depthView ? &depth : nullptr,
                                        .pStencilAttachment = materialTarget.depthView && stencil ? &depth : nullptr};
        native.beginRendering(command, &rendering);
        const VkViewport viewport{0, 0, static_cast<float>(request.extent.width), static_cast<float>(request.extent.height), 0, 1};
        const VkRect2D scissor{{0, 0}, {request.extent.width, request.extent.height}};
        native.viewport(command, 0, 1, &viewport);
        native.scissor(command, 0, 1, &scissor);
        for (std::size_t index = 0; index < request.draws.size(); ++index) {
            const auto &draw = materialDraws[index];
            native.bindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.material->pipeline);
            if (!draw.material->sets.empty())
                native.bindSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.material->layout, 0,
                                static_cast<std::uint32_t>(draw.material->sets.size()), draw.material->sets.data(), 0, nullptr);
            const VkDeviceSize offset = 0;
            native.bindVertices(command, 0, 1, &draw.mesh.vertex.buffer, &offset);
            native.bindIndex(command, draw.mesh.index.buffer, 0,
                             draw.mesh.descriptor.indexFormat == RenderIndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            native.drawIndexed(command, draw.mesh.descriptor.indexCount, 1, 0, 0, 0);
        }
        native.endRendering(command);
    }

    /** @brief Checks active frame, borrowed envelope and required native graphics dispatch. */
    Result<void> VulkanGraphRuntime::Impl::ValidateMaterialAdmission(const RenderMaterialExecutionRequest &request) const {
        if (!lifecycle.frame.IsValid() || lifecycle.frame != request.frame || lifecycle.recorded)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        if (!request.submission)
            return Result<void>::Failure(Invalid("Material requires explicit submission evidence."));
        if (const std::array validRequest{request.lease != nullptr, request.transfer != nullptr, !request.draws.empty(),
                                          request.draws.size() <= RenderMaterialPass::MaximumDraws,
                                          request.submission->waits.size() <= RenderGraphSubmissionLimits::MaximumQueues};
            !std::ranges::all_of(validRequest, [](bool valid) {
            return valid;
        }))
            return Result<void>::Failure(Invalid("Material execution requires bounded draws and a completion lease."));
        if (const std::array dispatch{native.bindPipeline != nullptr, native.bindSets != nullptr, native.bindVertices != nullptr,
                                      native.bindIndex != nullptr, native.viewport != nullptr, native.scissor != nullptr,
                                      native.drawIndexed != nullptr};
            !std::ranges::all_of(dispatch, [](bool valid) {
            return valid;
        }))
            return Result<void>::Failure(MakeError(VulkanBackendErrors::EntryPointMissing));
        if (const auto *queue = Queue(request.submission->queue); !queue || !queue->supportsGraphics)
            return Result<void>::Failure(Unsupported("Material execution requires the admitted graphics queue."));
        return Result<void>::Success();
    }

    /** @brief Transfers the one frontend lease only after native ticket retention has succeeded. */
    Result<void> VulkanGraphRuntime::Impl::RecordMaterial(const RenderMaterialExecutionRequest &request, VulkanCommandRetention &pins) {
        auto reserved = commands.Begin(request.submission->queue);
        if (reserved.HasError()) {
            pins.resources.clear();
            return Result<void>::Failure(reserved.ErrorValue());
        }
        submissions.emplace_back();
        auto &submission = submissions.back();
        submission.ticket = reserved.Value();
        submission.value = *request.submission;
        std::ranges::copy(request.submission->waits, submission.waits.begin());
        submission.value.waits = std::span{submission.waits}.first(request.submission->waits.size());
        submission.updates = {0, updates.size()};
        const auto fail = [&](Result<void> result) {
            Abort();
            pins.resources.clear();
            return result;
        };
        if (auto retained = commands.Retain(submission.ticket, pins); retained.HasError())
            return fail(retained);
        pins.lease = request.lease;
        request.transfer->authority = RenderGraphLeaseAuthority::BackendCompletion;
        auto command = commands.RecordingBuffer(submission.ticket);
        if (command.HasError())
            return fail(Result<void>::Failure(command.ErrorValue()));
        EncodeMaterial(command.Value(), request);
        if (auto finished = commands.Finish(submission.ticket); finished.HasError())
            return fail(finished);
        lifecycle.recorded = true;
        return Result<void>::Success();
    }

    /** @copydoc VulkanGraphRuntime::ExecuteMaterialPass */
    Result<void> VulkanGraphRuntime::ExecuteMaterialPass(const RenderMaterialExecutionRequest &request) {
        if (auto access = impl_->Access(); access.HasError())
            return access;
        if (auto valid = impl_->ValidateMaterialAdmission(request); valid.HasError())
            return valid;
        const RenderGraphQueueSubmission record{*request.submission, {0, 1}};
        if (auto timelines = impl_->commands.ValidateBatch(std::span{&record, 1}); timelines.HasError())
            return timelines;
        auto pins = std::ranges::find_if(impl_->retention, [](const auto &entry) {
            return entry.references == 0;
        });
        if (pins == impl_->retention.end())
            return Result<void>::Failure(MakeError(VulkanBackendErrors::SubmissionBusy));
        pins->resources.clear();
        if (auto prepared = impl_->PrepareMaterial(request, *pins); prepared.HasError()) {
            pins->resources.clear();
            return prepared;
        }
        return impl_->RecordMaterial(request, *pins);
    }
}  // namespace Horo::Render::Detail
