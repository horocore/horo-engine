#include "EditorViewportRendererVulkan.h"
#include "EditorViewportVulkanCanvasProjection.h"
#include "Horo/Foundation/Assertions.h"
#include "Horo/Runtime/Render/MultiviewShaderParameters.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderFrontendResourceAccess.h"
#include "RenderGraphTemplate.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "host/EditorVulkanXrMirrorProjection.h"
#include "host/EditorVulkanXrViewportAccess.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace Horo::Editor {
    using namespace Render;

    /** @copydoc EditorViewportRendererVulkan::StageXrCoverageReductionProgram */
    void EditorViewportRendererVulkan::StageXrCoverageReductionProgram(EditorVulkanCoverageReductionCook cooked) noexcept {
        if (owner_ == std::this_thread::get_id() && initialized_ && !recordedEver_ && xrCoverageReductionArtifact_.empty())
            xrCoverageReductionArtifact_ = std::move(cooked.payload);
    }

    /** @copydoc EditorVulkanXrViewportAccess::CanPrepareReduction */
    bool EditorVulkanXrViewportAccess::CanPrepareReduction(const EditorViewportRendererVulkan &viewport,
                                                           const RenderTextureFormat depth) noexcept {
        if (viewport.owner_ != std::this_thread::get_id() || !viewport.initialized_ || viewport.xrGraph_ ||
            depth != RenderTextureFormat::Depth32Float || !viewport.xrCoverageCook_ || viewport.xrCoverageReductionArtifact_.empty())
            return false;
        const auto *port = viewport.bridge_.Port();
        return port && port->SupportsStereoCoverageReduction(viewport.device_.graphicsIdentity);
    }

    /** @copydoc EditorVulkanXrViewportAccess::SupportsEnvironmentLighting */
    bool EditorVulkanXrViewportAccess::SupportsEnvironmentLighting(const EditorViewportRendererVulkan &viewport) noexcept {
        return viewport.owner_ == std::this_thread::get_id() && viewport.initialized_ && viewport.xrEnvironmentLighting_;
    }

    /** @copydoc EditorVulkanXrViewportAccess::PreparedTargets::PreparedTargets */
    EditorVulkanXrViewportAccess::PreparedTargets::PreparedTargets(std::weak_ptr<const void> graph, const std::uint64_t device,
                                                                   const std::uint64_t capabilities) noexcept
        : graph_(std::move(graph)), device_(device), capabilities_(capabilities) {}

    /** @copydoc EditorVulkanXrViewportAccess::PreparePairedTargets */
    Result<EditorVulkanXrViewportAccess::PreparedTargets> EditorVulkanXrViewportAccess::PreparePairedTargets(
        EditorViewportRendererVulkan &viewport, const std::span<const RenderMeshResourceView> meshes, const RenderTextureDescriptor &color,
        const RenderGraphLogicalState &acquired, const RenderGraphLogicalState &requiredFinal, const AuxiliaryPlan &auxiliary,
        const std::size_t coverageDrawCapacity) {
        using Outcome = Result<PreparedTargets>;
        const auto prepared = Prepare(viewport, meshes, color, acquired, requiredFinal, true, &auxiliary, coverageDrawCapacity);
        if (prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
        const auto &support = viewport.frontend_.Capabilities().support;
        return Outcome::Success(PreparedTargets{viewport.xrGraph_, support.deviceIncarnation, support.capabilityRevision});
    }

    /** @copydoc EditorVulkanXrViewportAccess::Device */
    Result<Detail::VulkanEditorDevice> EditorVulkanXrViewportAccess::Device(const EditorViewportRendererVulkan &viewport) {
        return viewport.BorrowXrDevice();
    }

    /** @copydoc EditorVulkanXrViewportAccess::PrepareMotionPipeline */
    Result<bool> EditorVulkanXrViewportAccess::PrepareMotionPipeline(EditorViewportRendererVulkan &viewport,
                                                                     const RenderTextureFormat motion, const RenderTextureFormat depth) {
        return viewport.PrepareXrMotionPipeline(motion, depth);
    }

    /** @copydoc EditorVulkanXrViewportAccess::Prepare */
    Result<void> EditorVulkanXrViewportAccess::Prepare(EditorViewportRendererVulkan &viewport,
                                                       std::span<const RenderMeshResourceView> meshes, const RenderTextureDescriptor &color,
                                                       const RenderGraphLogicalState &acquired,
                                                       const RenderGraphLogicalState &requiredFinal, const bool retainPreviousDepth,
                                                       const AuxiliaryPlan *auxiliary, const std::size_t coverageDrawCapacity) {
        return viewport.PrepareXrGraph(meshes, color, acquired, requiredFinal, retainPreviousDepth, auxiliary, coverageDrawCapacity);
    }

    /** @copydoc EditorVulkanXrViewportAccess::PrepareCanvas */
    Result<bool> EditorVulkanXrViewportAccess::PrepareCanvas(EditorViewportRendererVulkan &viewport,
                                                             std::span<const RenderMeshResourceView> meshes,
                                                             const CanvasPreparation &canvas) {
        return viewport.PrepareXrCanvas(meshes, canvas);
    }

    /** @copydoc EditorVulkanXrViewportAccess::Bind */
    Result<void> EditorVulkanXrViewportAccess::Bind(EditorViewportRendererVulkan &viewport, RenderTextureHandle color,
                                                    const std::array<RenderTextureViewHandle, 2> &views,
                                                    const MultiviewPublication &publication, std::span<const MultiviewSceneView> cameras,
                                                    std::span<const RenderStaticMeshInstance> instances, const MotionInput *motion,
                                                    const EditorXrProjectionAlpha alpha, const AuxiliaryImages *auxiliary,
                                                    const MultiviewEnvironmentLighting *lighting,
                                                    const std::array<MultiviewRenderRectangle, 2> *rectangles,
                                                    const CanvasPlacement *canvas) {
        return viewport.BindXrFrame(color, views, publication, cameras, instances, motion ? &motion->frame : nullptr,
                                    motion ? motion->objects : std::span<const RenderMotionObjectId>{}, alpha, auxiliary, lighting,
                                    rectangles, canvas);
    }

    /** @copydoc EditorVulkanXrViewportAccess::TakePremultipliedProjection */
    std::optional<EditorXrPremultipliedProjection> EditorVulkanXrViewportAccess::TakePremultipliedProjection(
        EditorViewportRendererVulkan &viewport, const MultiviewPublication &publication, const RenderTextureHandle color) noexcept {
        return viewport.TakeXrPremultipliedProjection(publication, color);
    }

    /** @copydoc EditorVulkanXrViewportAccess::CompleteMotion */
    void EditorVulkanXrViewportAccess::CompleteMotion(EditorViewportRendererVulkan &viewport,
                                                      const std::optional<MultiviewMotionFrame> &frame, bool projected) noexcept {
        viewport.CompleteXrMotion(frame, projected);
    }

    /** @copydoc EditorVulkanXrViewportAccess::Execute */
    Result<EditorVulkanXrViewportAccess::ProjectionSubmission> EditorVulkanXrViewportAccess::Execute(EditorViewportRendererVulkan &viewport,
                                                                                                     RenderFrameScope &frame,
                                                                                                     const PrimaryOutputAttachment &output,
                                                                                                     const CanvasSource *canvas) {
        return viewport.ExecuteXrFrame(frame, output, canvas);
    }

    /** @copydoc EditorVulkanXrViewportAccess::Mirror */
    EditorViewportTextureView EditorVulkanXrViewportAccess::Mirror(const EditorViewportRendererVulkan &viewport, const std::uint64_t frame,
                                                                   const RenderTextureViewHandle expected) noexcept {
        return viewport.XrMirrorView(frame, expected);
    }

    /** @copydoc EditorVulkanXrViewportAccess::RevokeMirror */
    void EditorVulkanXrViewportAccess::RevokeMirror(const EditorViewportRendererVulkan &viewport) noexcept {
        viewport.RevokeXrMirror();
    }

    /** @copydoc EditorVulkanXrViewportAccess::Retire */
    Result<void> EditorVulkanXrViewportAccess::Retire(EditorViewportRendererVulkan &viewport) {
        return viewport.RetireXrGraph();
    }

    /** @copydoc EditorVulkanXrViewportAccess::Poll */
    Result<void> EditorVulkanXrViewportAccess::Poll(EditorViewportRendererVulkan &viewport) {
        return viewport.PollXrQueue();
    }

    /** @brief One cold scene topology and its exact owned depth allocation. */
    struct EditorViewportRendererVulkan::XrProjectionGraph final {
        static constexpr std::size_t MaximumDraws = RenderMaterialPass::MaximumDraws;
        static constexpr std::size_t MaximumReads = MaximumDraws * 5;
        static constexpr std::size_t MaximumAttachments = 10;
        ResourceCreation<RenderTextureHandle> depthTexture;
        std::array<ResourceCreation<RenderTextureViewHandle>, 2> depthViews{};

        struct Geometry final {
            std::uint64_t source{};
            RenderMeshSourceHandle sourceGeneration;
            RenderMeshHandle mesh;
            RenderGraphResourceId vertices;
            RenderGraphResourceId indices;
        };

        struct CoveragePlan final {
            RenderTextureHandle color;
            RenderTextureHandle depth;
            RenderTextureHandle predecessor;
            std::span<const DrawSlot> slots;
            RenderBufferHandle readback;
            RenderBufferHandle reset, counters;
        };

        /** @brief Cold-creates depth storage and views, retaining every successful prefix for owner teardown. */
        Result<void> PrepareDepth(RenderFrontend &frontend, const RenderTextureDescriptor &colorDescriptor, const bool sampled) {
            auto createdDepth =
                Detail::RenderFrontendResourceAccess::CreateMultiviewDepth(frontend, RenderMemoryScopes::ViewportResources,
                                                                           colorDescriptor.extent,
                                                                           static_cast<std::uint32_t>(depthViews.size()), sampled);
            if (createdDepth.HasError())
                return Result<void>::Failure(createdDepth.ErrorValue());
            depthTexture = createdDepth.Value();
            if (auto processed = frontend.ProcessResourceRequests(); processed.HasError())
                return Result<void>::Failure(processed.ErrorValue());
            if (auto ready = frontend.ResourceOperationResult(createdDepth.Value().operation); ready.HasError())
                return ready;
            for (std::uint32_t layer = 0; layer < depthViews.size(); ++layer) {
                auto view = frontend.CreateTextureView({.texture = depthTexture.handle,
                                                        .format = RenderTextureFormat::Depth32Float,
                                                        .aspect = RenderTextureAspect::Depth,
                                                        .baseLayer = layer,
                                                        .layerCount = 1});
                if (view.HasError())
                    return Result<void>::Failure(view.ErrorValue());
                depthViews[layer] = view.Value();
                if (auto processed = frontend.ProcessResourceRequests(); processed.HasError())
                    return Result<void>::Failure(processed.ErrorValue());
                if (auto ready = frontend.ResourceOperationResult(view.Value().operation); ready.HasError())
                    return ready;
            }
            return Result<void>::Success();
        }

        /** @brief Each cold variant has one exact graph identity and shares the same owned projection depth. */
        struct Topology final {
            std::optional<Detail::PreparedRenderGraphTemplate> topology;
            Detail::RenderGraphExternalTextureSlot color;
            RenderGraphResourceId depth;
            RenderGraphPassRef pass;
            std::vector<Geometry> geometry;
            std::array<RenderGraphResourceId, MaximumDraws> parameters{};
            std::vector<Detail::RenderGraphOptionalRead> reads;

            struct CanvasEye final {
                RenderGraphPassRef pass;
                std::array<RenderGraphMaterialDraw, 1> draws;
                std::array<RenderGraphMaterialRead, 2> reads;

                /** @brief Records the painted surface over one acquired color and depth layer, preserving both attachments. */
                RenderGraphMaterialRaster Raster(const MultiviewGraphTarget &target, FramebufferExtent extent) const noexcept {
                    const auto &rect = target.rectangle;
                    return {.color = target.color,
                            .depth = target.depth,
                            .colorView = target.colorView,
                            .depthView = target.depthView,
                            .extent = extent,
                            .colorOperations = {.loadOperation = AttachmentLoadOperation::Load},
                            .depthClear = std::nullopt,
                            .region = RenderMaterialRegion{rect.x, rect.y, rect.width, rect.height},
                            .draws = draws,
                            .reads = reads};
                }
            };

            struct CanvasGraph final {
                std::unique_ptr<CanvasQuadProgram> quad;
                std::unique_ptr<UiPaintProgram> paint;
                std::array<CanvasEye, CanvasQuadProgram::StereoViews> eyes;
                Sha256Digest writer;
                RenderTextureFormat stencil{};
            };

            std::optional<CanvasGraph> canvas;

            /** @brief Cold scene inputs shared by standalone and sampled-canvas graph owners. */
            struct ScenePreparation final {
                const RenderFrontend &frontend;
                const EditorViewportResources &resources;
                std::span<const EditorViewportMeshResourceView> meshes;
                std::span<const DrawSlot> slots;
                EditorVulkanXrViewportAccess::AuxiliaryTexture color;
                RenderTextureHandle projectionDepth;
                const EditorVulkanXrViewportAccess::AuxiliaryPlan *auxiliary{};
                std::span<const DrawSlot> motionSlots;
                const CoveragePlan *coverage{};
            };

            struct Auxiliary final {
                Detail::RenderGraphExternalTextureSlot motion;
                Detail::RenderGraphExternalTextureSlot depth;
                RenderGraphPassRef pass;
                std::array<RenderGraphResourceId, MaximumDraws> parameters{};
            };

            std::optional<Auxiliary> auxiliary;

            struct Coverage final {
                RenderGraphPassRef pass;
                RenderGraphResourceId color, depth, predecessor;
                std::array<RenderGraphResourceId, MaximumDraws> parameters{};
                RenderGraphPassRef copyPass;
                RenderGraphResourceId readback;
                RenderGraphPassRef clearPass, resetPass, reductionPass;
                RenderGraphResourceId reset, counters;
            };

            std::optional<Coverage> coverage;

            /** @brief Declares the full owned-mask clear, counter reset and compute on the exact graphics queue. */
            Result<void> DeclareCoverageReduction(RenderGraphBuilder &builder, const CoveragePlan &plan) {
                auto &output = *coverage;
                const auto clear = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
                const auto reset = builder.AddPass(RenderPassKind::Copy, RenderQueueRole::Graphics);
                const auto reduce = builder.AddPass(RenderPassKind::Compute, RenderQueueRole::Graphics);
                if (clear.HasError() || reset.HasError() || reduce.HasError())
                    return Result<void>::Failure(clear.HasError()   ? clear.ErrorValue()
                                                 : reset.HasError() ? reset.ErrorValue()
                                                                    : reduce.ErrorValue());
                output.clearPass = clear.Value();
                output.resetPass = reset.Value();
                output.reductionPass = reduce.Value();
                for (const auto &edge :
                     {RenderGraphDependency{output.clearPass, output.pass}, RenderGraphDependency{output.pass, output.reductionPass},
                      RenderGraphDependency{output.resetPass, output.reductionPass}})
                    if (auto added = builder.AddDependency(edge); added.HasError())
                        return added;
                const auto zeros = builder.ImportBuffer(plan.reset, RenderGraphResourceClass::Persistent);
                const auto result = builder.ImportBuffer(plan.counters, RenderGraphResourceClass::Persistent);
                if (zeros.HasError() || result.HasError())
                    return Result<void>::Failure(zeros.HasError() ? zeros.ErrorValue() : result.ErrorValue());
                output.reset = zeros.Value();
                output.counters = result.Value();
                using enum RenderGraphAccess;
                using enum RenderGraphUsageKind;
                for (const auto &use : {RenderGraphResourceUsage{output.clearPass, output.color, Write, ColorAttachment},
                                        RenderGraphResourceUsage{output.resetPass, output.reset, Read, CopySource},
                                        RenderGraphResourceUsage{output.resetPass, output.counters, Write, CopyDestination},
                                        RenderGraphResourceUsage{output.reductionPass, output.color, Read, Sampled},
                                        RenderGraphResourceUsage{output.reductionPass, output.counters, ReadWrite, Storage}})
                    if (auto added = builder.AddUsage(use); added.HasError())
                        return added;
                return Result<void>::Success();
            }

            /** @brief Adds a finite diagnostic copy after the exact coverage raster on the same graphics queue. */
            Result<void> DeclareCoverageCopy(RenderGraphBuilder &builder, RenderBufferHandle staging) {
                auto &output = *coverage;
                auto added = builder.AddPass(RenderPassKind::Copy, RenderQueueRole::Graphics);
                if (added.HasError())
                    return Result<void>::Failure(added.ErrorValue());
                output.copyPass = added.Value();
                if (auto order = builder.AddDependency({output.pass, output.copyPass}); order.HasError())
                    return order;
                auto destination = builder.ImportBuffer(staging, RenderGraphResourceClass::Persistent);
                if (destination.HasError())
                    return Result<void>::Failure(destination.ErrorValue());
                output.readback = destination.Value();
                if (auto read =
                        builder.AddUsage({output.copyPass, output.color, RenderGraphAccess::Read, RenderGraphUsageKind::CopySource});
                    read.HasError())
                    return read;
                return builder.AddUsage(
                    {output.copyPass, output.readback, RenderGraphAccess::Write, RenderGraphUsageKind::CopyDestination});
            }

            /** @brief Adds the private coverage targets and the preserved opposite history slot on the graphics queue. */
            Result<void> DeclareCoverage(RenderGraphBuilder &builder, const CoveragePlan &plan) {
                auto added = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
                if (added.HasError())
                    return Result<void>::Failure(added.ErrorValue());
                auto &output = coverage.emplace();
                output.pass = added.Value();
                if (auto order = builder.AddDependency({auxiliary ? auxiliary->pass : pass, output.pass}); order.HasError())
                    return order;
                const auto declare = [&](const RenderTextureHandle texture, RenderGraphResourceId &resource, const RenderGraphAccess access,
                                         const RenderGraphUsageKind usage) -> Result<void> {
                    auto imported = builder.ImportTexture(texture, RenderGraphResourceClass::Persistent);
                    if (imported.HasError())
                        return Result<void>::Failure(imported.ErrorValue());
                    resource = imported.Value();
                    return builder.AddUsage({output.pass, resource, access, usage});
                };
                if (auto color = declare(plan.color, output.color, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment);
                    color.HasError())
                    return color;
                if (auto depth = declare(plan.depth, output.depth, RenderGraphAccess::Write, RenderGraphUsageKind::DepthStencilAttachment);
                    depth.HasError())
                    return depth;
                if (auto prior = declare(plan.predecessor, output.predecessor, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled);
                    prior.HasError())
                    return prior;
                if (plan.counters.IsValid())
                    return DeclareCoverageReduction(builder, plan);
                return plan.readback.IsValid() ? DeclareCoverageCopy(builder, plan.readback) : Result<void>::Success();
            }

            /** @brief Declares the two exact auxiliary slots and one ordered graphics pass. */
            Result<void> DeclareAuxiliary(RenderGraphBuilder &builder, const EditorVulkanXrViewportAccess::AuxiliaryPlan &plan,
                                          std::uint64_t deviceIncarnation) {
                auto added = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
                if (added.HasError())
                    return Result<void>::Failure(added.ErrorValue());
                auto &output = auxiliary.emplace();
                output.pass = added.Value();
                if (auto order = builder.AddDependency({pass, output.pass}); order.HasError())
                    return order;
                const auto declare = [&](const auto &texture, const RenderGraphUsageKind usage,
                                         Detail::RenderGraphExternalTextureSlot &slot) -> Result<void> {
                    auto external = Detail::PreparedRenderGraphTemplate::DeclareTexture(builder, texture.allocation, texture.acquired,
                                                                                        texture.requiredFinal, deviceIncarnation);
                    if (external.HasError())
                        return Result<void>::Failure(external.ErrorValue());
                    slot = external.Value();
                    return builder.AddUsage({output.pass, slot.resource, RenderGraphAccess::Write, usage});
                };
                if (auto admitted = declare(plan.motion, RenderGraphUsageKind::ColorAttachment, output.motion); admitted.HasError())
                    return admitted;
                return declare(plan.depth, RenderGraphUsageKind::DepthStencilAttachment, output.depth);
            }

            /** @brief Bounds the opaque scene and optional native producers before an owner adds its own passes. */
            static RenderGraphLimits Limits(const EditorVulkanXrViewportAccess::AuxiliaryPlan *plan, const CoveragePlan *coveragePlan) {
                const auto drawPasses = 1U + (plan ? 1U : 0U) + (coveragePlan ? 1U : 0U);
                const auto extraPasses = !coveragePlan                      ? 0U
                                         : coveragePlan->counters.IsValid() ? 3U
                                         : coveragePlan->readback.IsValid() ? 1U
                                                                            : 0U;
                const auto maximumPasses = drawPasses + extraPasses;
                return {.maxPasses = maximumPasses,
                        .maxResources = MaximumReads + MaximumAttachments,
                        .maxUsages = MaximumDraws * 3 * drawPasses + MaximumAttachments + extraPasses * 2U,
                        .maxDependencies = maximumPasses};
            }

            /** @brief Declares the fixed external/depth attachment topology in the owner's existing graph. */
            Result<void> DeclareAttachments(RenderGraphBuilder &builder, const ScenePreparation &scene) {
                auto createdPass = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
                if (createdPass.HasError())
                    return Result<void>::Failure(createdPass.ErrorValue());
                this->pass = createdPass.Value();
                const auto device = scene.frontend.Capabilities().support.deviceIncarnation;
                auto external = Detail::PreparedRenderGraphTemplate::DeclareTexture(builder, scene.color.allocation, scene.color.acquired,
                                                                                    scene.color.requiredFinal, device);
                if (external.HasError())
                    return Result<void>::Failure(external.ErrorValue());
                this->color = external.Value();
                auto importedDepth = builder.ImportTexture(scene.projectionDepth, RenderGraphResourceClass::Persistent);
                if (importedDepth.HasError())
                    return Result<void>::Failure(importedDepth.ErrorValue());
                this->depth = importedDepth.Value();
                if (auto use = builder.AddUsage(
                        {this->pass, this->color.resource, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
                    use.HasError())
                    return use;
                if (auto use =
                        builder.AddUsage({this->pass, this->depth, RenderGraphAccess::Write, RenderGraphUsageKind::DepthStencilAttachment});
                    use.HasError())
                    return use;
                if (scene.auxiliary)
                    if (auto declared = DeclareAuxiliary(builder, *scene.auxiliary, device); declared.HasError())
                        return declared;
                if (scene.coverage)
                    if (auto declared = DeclareCoverage(builder, *scene.coverage); declared.HasError())
                        return declared;
                return Result<void>::Success();
            }

            struct Input final {
                RenderBufferHandle handle;
                RenderGraphResourceId resource;
                RenderGraphUsageKind role;
            };

            /** @brief Imports one deduplicated immutable read into the same cold graph and retention superset. */
            Result<RenderGraphResourceId> ImportRead(RenderGraphBuilder &builder, std::vector<RenderGraphImportedState> &initial,
                                                     std::vector<Input> &inputs, RenderQueueId queue, RenderBufferHandle handle,
                                                     RenderGraphUsageKind role, RenderGraphPassRef consumer, bool sharedGeometry = false) {
                for (const auto &existing : inputs)
                    if (existing.handle == handle)
                        return existing.role == role
                                   ? Result<RenderGraphResourceId>::Success(existing.resource)
                                   : Result<RenderGraphResourceId>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
                auto imported = builder.ImportBuffer(handle, RenderGraphResourceClass::Persistent);
                if (imported.HasError())
                    return imported;
                if (auto use = builder.AddUsage({consumer, imported.Value(), RenderGraphAccess::Read, role}); use.HasError())
                    return Result<RenderGraphResourceId>::Failure(use.ErrorValue());
                if (sharedGeometry && auxiliary)
                    if (auto use = builder.AddUsage({auxiliary->pass, imported.Value(), RenderGraphAccess::Read, role}); use.HasError())
                        return Result<RenderGraphResourceId>::Failure(use.ErrorValue());
                if (sharedGeometry && coverage)
                    if (auto use = builder.AddUsage({coverage->pass, imported.Value(), RenderGraphAccess::Read, role}); use.HasError())
                        return Result<RenderGraphResourceId>::Failure(use.ErrorValue());
                initial.push_back({imported.Value(),
                                   {RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                    RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::NotApplicable, queue}});
                inputs.emplace_back(handle, imported.Value(), role);
                reads.emplace_back(imported.Value(), role);
                return imported;
            }

            /** @brief Cold-captures the canonical resident meshes and material slots without another geometry owner. */
            Result<void> PrepareReads(RenderGraphBuilder &builder, std::vector<RenderGraphImportedState> &initial,
                                      std::span<const EditorViewportMeshResourceView> meshes, const EditorViewportResources &resources,
                                      std::span<const DrawSlot> slots, std::span<const DrawSlot> motionSlots,
                                      std::span<const DrawSlot> coverageSlots, RenderQueueId queue) {
                std::vector<Input> inputs;
                inputs.reserve(MaximumReads);
                for (const auto &source : meshes) {
                    const auto mesh = resources.FindMesh(source.handle.id.value);
                    if (!mesh)
                        return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleMeshResource));
                    auto vertices = ImportRead(builder, initial, inputs, queue, mesh->descriptor.vertexBuffer,
                                               RenderGraphUsageKind::VertexBuffer, pass, true);
                    if (vertices.HasError())
                        return Result<void>::Failure(vertices.ErrorValue());
                    auto indices = ImportRead(builder, initial, inputs, queue, mesh->descriptor.indexBuffer,
                                              RenderGraphUsageKind::IndexBuffer, pass, true);
                    if (indices.HasError())
                        return Result<void>::Failure(indices.ErrorValue());
                    geometry.emplace_back(source.handle.id.value, source.handle, mesh->mesh, vertices.Value(), indices.Value());
                }
                std::ranges::sort(geometry, {}, &Geometry::source);
                for (std::size_t draw = 0; draw < this->parameters.size(); ++draw) {
                    auto importedParameters =
                        ImportRead(builder, initial, inputs, queue, slots[draw].buffer, RenderGraphUsageKind::UniformBuffer, pass);
                    if (importedParameters.HasError())
                        return Result<void>::Failure(importedParameters.ErrorValue());
                    this->parameters[draw] = importedParameters.Value();
                    if (auxiliary) {
                        auto motion = ImportRead(builder, initial, inputs, queue, motionSlots[draw].buffer,
                                                 RenderGraphUsageKind::UniformBuffer, auxiliary->pass);
                        if (motion.HasError())
                            return Result<void>::Failure(motion.ErrorValue());
                        auxiliary->parameters[draw] = motion.Value();
                    }
                    if (coverage && draw < coverageSlots.size()) {
                        auto uniform = ImportRead(builder, initial, inputs, queue, coverageSlots[draw].buffer,
                                                  RenderGraphUsageKind::UniformBuffer, coverage->pass);
                        if (uniform.HasError())
                            return Result<void>::Failure(uniform.ErrorValue());
                        coverage->parameters[draw] = uniform.Value();
                    }
                }
                return Result<void>::Success();
            }

            /** @brief Resolves exact immutable mesh generations and extracts fixed multiview values without allocation. */
            Result<MultiviewSceneSnapshot> ExtractScene(const MultiviewPublication &publication, std::span<const MultiviewSceneView> views,
                                                        std::span<const RenderStaticMeshInstance> instances, const RenderFrontend &frontend,
                                                        std::span<const DrawSlot> slots,
                                                        std::array<const Geometry *, RenderMaterialPass::MaximumDraws> &meshes) const {
                std::array<MultiviewSceneInstance, RenderMaterialPass::MaximumDraws> values{};
                for (std::size_t index = 0; index < instances.size(); ++index) {
                    const auto &source = instances[index];
                    auto mesh = std::ranges::lower_bound(geometry, source.mesh.id.value, {}, &Geometry::source);
                    if (!source.IsValid() || source.material != CoreDefaultMaterial || mesh == geometry.end() ||
                        mesh->sourceGeneration != source.mesh || !slots[index].material)
                        return Result<MultiviewSceneSnapshot>::Failure(MakeError(RendererErrors::ViewportStaleMeshResource));
                    meshes[index] = std::to_address(mesh);
                    values[index] = {mesh->mesh, slots[index].material->Id(), source.localToWorld, source.localBounds};
                }
                return MultiviewSceneSnapshot::Extract(publication, publication, views, std::span{values}.first(instances.size()),
                                                       ResolveMultiviewCapabilities(frontend.Capabilities().support),
                                                       MultiviewExecutionPolicy::Multipass);
            }

            /** @brief Binds exact selected read IDs through the canonical retired-instance gate and immutable superset. */
            Result<const CompiledRenderGraphExecution *> BindReads(
                const RenderFrontend &frontend, RenderTextureHandle colorTexture, const MultiviewSceneSnapshot &snapshot,
                const std::array<const Geometry *, RenderMaterialPass::MaximumDraws> &meshes,
                const EditorVulkanXrViewportAccess::AuxiliaryImages *images) {
                std::array<Detail::RenderGraphOptionalRead, MaximumReads> selectedReads{};
                std::bitset<RenderGraphLimits::HardMaxResources> selected{};
                std::size_t readCount{};
                const auto select = [&](RenderGraphResourceId resource, RenderGraphUsageKind role) {
                    // Every ID comes from this immutable cold superset, not from the caller's scene.
                    const auto index = resource.value - 1;
                    if (!selected[index]) {
                        selected[index] = true;
                        selectedReads[readCount++] = {resource, role};
                    }
                };
                for (const auto &draw : snapshot.Draws()) {
                    const auto index = draw.instanceIndex;
                    select(meshes[index]->vertices, RenderGraphUsageKind::VertexBuffer);
                    select(meshes[index]->indices, RenderGraphUsageKind::IndexBuffer);
                    select(parameters[index], RenderGraphUsageKind::UniformBuffer);
                    if (auxiliary)
                        select(auxiliary->parameters[index], RenderGraphUsageKind::UniformBuffer);
                    if (coverage)
                        select(coverage->parameters[index], RenderGraphUsageKind::UniformBuffer);
                }
                std::array<Detail::RenderGraphExternalTextureBinding, 3> bindings{{{color.resource, colorTexture}}};
                if (images) {
                    bindings[1] = {auxiliary->motion.resource, images->motion};
                    bindings[2] = {auxiliary->depth.resource, images->depth};
                }
                const auto external = std::span{bindings}.first(images ? bindings.size() : 1);
                const auto readsForFrame = std::span{selectedReads}.first(readCount);
                if (canvas)
                    return Detail::RenderFrontendResourceAccess::BindExternalSceneCanvas(*canvas->paint, 0, external, readsForFrame);
                return topology->Bind(frontend, 0, external, readsForFrame);
            }

            /** @brief Adds the two depth-tested canvas draws after opaque projection in the same ordered graph. */
            Result<RenderGraphPassRef> AuthorCanvas(RenderGraphBuilder &builder, const UiPaintSceneCanvasSource &source,
                                                    std::vector<RenderGraphImportedState> &initial) {
                using Outcome = Result<RenderGraphPassRef>;
                auto &quad = *canvas->quad;
                const auto queue = color.acquired.queue;
                const auto &first = quad.slots[source.slot].front();
                const RenderGraphLogicalState initialBuffer{RenderGraphSynchronizationAccess::Read,
                                                            RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                                            RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::NotApplicable,
                                                            queue};
                const auto importBuffer = [&](RenderBufferHandle buffer) {
                    auto id = builder.ImportBuffer(buffer, RenderGraphResourceClass::Persistent);
                    if (id.HasValue())
                        initial.emplace_back(id.Value(), initialBuffer);
                    return id;
                };
                const auto vertices = importBuffer(first.vertices);
                if (vertices.HasError())
                    return Outcome::Failure(vertices.ErrorValue());
                const auto indices = importBuffer(first.indices);
                if (indices.HasError())
                    return Outcome::Failure(indices.ErrorValue());
                auto previous = pass;
                for (std::size_t index = 0; index < canvas->eyes.size(); ++index) {
                    auto &eye = canvas->eyes[index];
                    const auto added = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
                    if (added.HasError())
                        return added;
                    eye.pass = added.Value();
                    const auto &draw = quad.slots[source.slot][index];
                    const auto uniform = importBuffer(draw.uniforms.buffer);
                    if (uniform.HasError())
                        return Outcome::Failure(uniform.ErrorValue());
                    const std::array uses{RenderGraphResourceUsage{eye.pass, vertices.Value(), RenderGraphAccess::Read,
                                                                   RenderGraphUsageKind::VertexBuffer},
                                          RenderGraphResourceUsage{eye.pass, indices.Value(), RenderGraphAccess::Read,
                                                                   RenderGraphUsageKind::IndexBuffer},
                                          RenderGraphResourceUsage{eye.pass, uniform.Value(), RenderGraphAccess::Read,
                                                                   RenderGraphUsageKind::UniformBuffer},
                                          RenderGraphResourceUsage{eye.pass, source.color, RenderGraphAccess::Read,
                                                                   RenderGraphUsageKind::Sampled},
                                          RenderGraphResourceUsage{eye.pass, color.resource, RenderGraphAccess::ReadWrite,
                                                                   RenderGraphUsageKind::ColorAttachment},
                                          RenderGraphResourceUsage{eye.pass, depth, RenderGraphAccess::ReadWrite,
                                                                   RenderGraphUsageKind::DepthStencilAttachment}};
                    for (const auto &use : uses)
                        if (auto accepted = builder.AddUsage(use); accepted.HasError())
                            return Outcome::Failure(accepted.ErrorValue());
                    if (auto ordered = builder.AddDependency({previous, eye.pass}); ordered.HasError())
                        return Outcome::Failure(ordered.ErrorValue());
                    eye.draws = {{{draw.mesh, vertices.Value(), indices.Value(), draw.uniforms.material}}};
                    eye.reads = {{{uniform.Value(), draw.uniforms.buffer, {}}, {source.color, {}, quad.sampledViews[source.slot]}}};
                    previous = eye.pass;
                }
                return Outcome::Success(canvas->eyes.front().pass);
            }

            /** @brief Authors the scene and its exact imported state without owning or finalizing the builder. */
            Result<void> AuthorScene(RenderGraphBuilder &builder, std::vector<RenderGraphImportedState> &initial,
                                     const ScenePreparation &scene) {
                using enum RenderGraphInitialContents;
                if (auto declared = DeclareAttachments(builder, scene); declared.HasError())
                    return declared;
                const auto queue = scene.color.acquired.queue;
                initial.push_back({depth, {}, Discarded});
                if (coverage) {
                    initial.push_back({coverage->color, {}, Discarded});
                    initial.push_back({coverage->depth, {}, Discarded});
                    if (coverage->readback.IsValid())
                        initial.push_back({coverage->readback, {}, Discarded});
                    if (coverage->counters.IsValid()) {
                        initial.push_back({coverage->counters, {}, Discarded});
                        initial.push_back({coverage->reset,
                                           {RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::CopySource,
                                            RenderGraphPipelineScope::Transfer, RenderGraphTextureLayout::NotApplicable, queue}});
                    }
                    initial.push_back(
                        {coverage->predecessor,
                         {RenderGraphSynchronizationAccess::Write, RenderGraphSynchronizationOperation::DepthStencilAttachment,
                          RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::DepthStencilAttachment, queue}});
                }
                reads.reserve(MaximumReads);
                geometry.reserve(scene.meshes.size());
                return PrepareReads(builder, initial, scene.meshes, scene.resources, scene.slots, scene.motionSlots,
                                    scene.coverage ? scene.coverage->slots : std::span<const DrawSlot>{}, queue);
            }

            /** @brief Warms one exact graph variant before acquiring any viewport images. */
            Result<void> Prepare(const ScenePreparation &scene) {
                auto created = RenderGraphBuilder::Create(Limits(scene.auxiliary, scene.coverage));
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                auto builder = std::move(created).Value();
                std::vector<RenderGraphImportedState> initial;
                initial.reserve(MaximumReads + MaximumAttachments);
                if (auto authored = AuthorScene(builder, initial, scene); authored.HasError())
                    return authored;
                auto graph = builder.Finalize();
                if (graph.HasError())
                    return Result<void>::Failure(graph.ErrorValue());
                const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, scene.color.acquired.queue}};
                std::array<Detail::RenderGraphExternalTextureSlot, 3> external{color};
                if (auxiliary) {
                    external[1] = auxiliary->motion;
                    external[2] = auxiliary->depth;
                }
                auto prepared = Detail::PreparedRenderGraphTemplate::Prepare(std::move(graph).Value(),
                                                                             std::span{external}.first(auxiliary ? external.size() : 1),
                                                                             initial, queues, 1, reads);
                if (prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
                topology.emplace(std::move(prepared).Value());
                return Result<void>::Success();
            }
        };

        Topology ordinary;
        std::optional<Topology> paired;
        std::optional<Topology> covered, pairedCovered, pairedReduced;

        /** @brief Both cold graph shapes reference the same owned projection-depth allocation. */
        Result<void> Prepare(RenderFrontend &frontend, std::span<const EditorViewportMeshResourceView> meshes,
                             const EditorViewportResources &resources, std::span<const DrawSlot> slots,
                             const RenderTextureDescriptor &colorDescriptor, const RenderGraphLogicalState &acquired,
                             const RenderGraphLogicalState &requiredFinal, const bool sampled,
                             const EditorVulkanXrViewportAccess::AuxiliaryPlan *plan, std::span<const DrawSlot> motionSlots) {
            if (auto ready = PrepareDepth(frontend, colorDescriptor, sampled); ready.HasError())
                return ready;
            Topology::ScenePreparation scene{frontend,           resources, meshes, slots, {colorDescriptor, acquired, requiredFinal},
                                             depthTexture.handle};
            if (auto compiled = ordinary.Prepare(scene); compiled.HasError())
                return compiled;
            if (!plan)
                return Result<void>::Success();
            scene.auxiliary = plan;
            scene.motionSlots = motionSlots;
            return paired.emplace().Prepare(scene);
        }

        /** @brief Retires complete and failed allocation prefixes using their exact creation journal. */
        Result<void> RetireDepth(RenderFrontend &frontend) {
            for (auto &view : depthViews) {
                if (!view.handle.IsValid())
                    continue;
                if (auto retired = Detail::RenderFrontendResourceAccess::RetireCreation(frontend, view); retired.HasError())
                    return retired;
                view = {};
            }
            if (depthTexture.handle.IsValid()) {
                if (auto retired = Detail::RenderFrontendResourceAccess::RetireCreation(frontend, depthTexture); retired.HasError())
                    return retired;
                depthTexture = {};
            }
            return Result<void>::Success();
        }
    };

    /** @brief Complete selected XR ownership including staged frame data and depth history. */
    struct EditorViewportRendererVulkan::XrGraph final {
        using Projection = XrProjectionGraph;

        Projection baseline;
        std::array<Projection, 2> history;
        Projection *single{&baseline};
        const Projection::Topology *active{};
        std::unique_ptr<Projection::Topology> canvas;
        std::optional<EditorVulkanXrViewportAccess::CanvasPlacement> canvasPlacement;
        EditorVulkanXrViewportAccess::AuxiliaryTexture allocation;
        const CompiledRenderGraphExecution *lastCanvasVariant{}; /**< Ordinary or canvas execution borrowing shared scene scratch. */

        /** @brief Records the canvas raster and both eye draws in the same native submission as the projection. */
        Result<RenderTimelinePoint> RecordCanvas(RenderFrameScope &frame, UiRenderSubmission source,
                                                 std::span<const RenderGraphPassWorkload> workloads) {
            auto &program = *canvas->canvas->paint;
            const auto prepared = program.PrepareSceneFrame(frame, 0, source);
            if (prepared.HasError())
                return Result<RenderTimelinePoint>::Failure(prepared.ErrorValue());
            return Detail::RenderFrontendResourceAccess::RecordSubmittedSceneCanvas(program, frame, prepared.Value(), std::move(source),
                                                                                    workloads);
        }

        /** @brief Cold-prepares the one ordinary XR projection plus its sampled canvas, using the same owned depth. */
        Result<void> PrepareCanvas(EditorViewportRendererVulkan &owner, const Projection::Topology::ScenePreparation &sceneInputs,
                                   const EditorVulkanXrViewportAccess::CanvasPreparation &request) {
            auto paint = request.paint;
            paint.frameSlots = 1;
            const auto writer = request.writer.artifact.reflection->cookedArtifactKey;
            if (canvas && canvas->canvas->writer == writer && canvas->canvas->stencil == request.writer.stencilFormat &&
                canvas->canvas->quad->shader.metadata.cookedArtifactKey == request.consumer.metadata.cookedArtifactKey &&
                canvas->geometry.size() == sceneInputs.meshes.size() &&
                std::ranges::all_of(sceneInputs.meshes,
                                    [this](const auto &mesh) {
                return std::ranges::find(canvas->geometry, mesh.handle, &Projection::Geometry::sourceGeneration) != canvas->geometry.end();
            }) &&
                Detail::RenderFrontendResourceAccess::MatchesSceneCanvas(*canvas->canvas->paint, paint, request.prototype,
                                                                         request.textures))
                return Result<void>::Success();
            auto candidate = std::make_unique<Projection::Topology>();
            auto quad = owner.CreateCanvasQuadProgram(request.consumer, UiRenderCompositionSpace::World,
                                                      sceneInputs.color.allocation.format, CanvasQuadProgram::StereoViews);
            if (quad.HasError())
                return Result<void>::Failure(quad.ErrorValue());
            candidate->canvas.emplace().quad = std::move(quad).Value();
            const RenderGraphLimits limits{paint.maximumPasses, RenderGraphLimits::HardMaxResources, RenderGraphLimits::HardMaxUsages,
                                           RenderGraphLimits::HardMaxDependencies};
            const Detail::UiPaintSceneCanvasRecipe recipe{limits, sceneInputs.color.acquired.queue,
                                                          [&owner, &candidate,
                                                           &sceneInputs](RenderGraphBuilder &builder,
                                                                         const UiPaintSceneCanvasSource &source,
                                                                         std::vector<RenderGraphImportedState> &initial,
                                                                         std::vector<Detail::RenderGraphExternalTextureSlot> &external,
                                                                         std::vector<Detail::RenderGraphOptionalRead> &reads) {
                if (auto authored = candidate->AuthorScene(builder, initial, sceneInputs); authored.HasError())
                    return Result<RenderGraphPassRef>::Failure(authored.ErrorValue());
                if (auto bound = owner.BindCanvasQuadSource(*candidate->canvas->quad, source.slot, source.surface.color); bound.HasError())
                    return Result<RenderGraphPassRef>::Failure(bound.ErrorValue());
                external.push_back(candidate->color);
                reads = candidate->reads;
                return candidate->AuthorCanvas(builder, source, initial);
            }};
            auto program = Detail::RenderFrontendResourceAccess::CreateSceneCanvas(owner.frontend_, paint, request.prototype,
                                                                                   request.textures, request.writer, recipe);
            if (program.HasError())
                return Result<void>::Failure(program.ErrorValue());
            if (auto processed = owner.frontend_.ProcessResourceRequests(); processed.HasError())
                return Result<void>::Failure(processed.ErrorValue());
            if (auto ready = program.Value()->ValidatePreparation(); ready.HasError())
                return ready;
            candidate->canvas->paint = std::move(program).Value();
            candidate->canvas->writer = writer;
            candidate->canvas->stencil = request.writer.stencilFormat;
            lastCanvasVariant = nullptr;
            canvas = std::move(candidate);
            return Result<void>::Success();
        }

        bool historyEnabled{};
        std::size_t activeHistorySlot{};

        struct DepthPublication {
            MultiviewMotionFrame frame;
            std::size_t slot{};
        };

        std::optional<DepthPublication> previousDepth;
        FramebufferExtent extent;
        RenderTimelinePoint observed;

        /** @brief Supplies the stable depth for canvas draws, which never publish motion history. */
        Projection &CanvasOutput() noexcept {
            return historyEnabled ? history.front() : *single;
        }

        /** @brief Selects a writable slot without overwriting the exact retained predecessor. */
        Projection &SelectOutput() noexcept {
            if (!historyEnabled)
                return *single;
            activeHistorySlot = previousDepth ? 1 - previousDepth->slot : 0;
            return history[activeHistorySlot];
        }

        struct SnapshotStorage final {
            std::optional<MultiviewSceneSnapshot> value;
        };

        // Cold control block. The first real extraction constructs only fixed inline value storage;
        // its aliasing shared owner adds no allocation and pins that exact value through queue completion.
        std::shared_ptr<SnapshotStorage> payload{std::make_shared<SnapshotStorage>()};
        std::shared_ptr<const MultiviewSceneSnapshot> scene;
        MultiviewEnvironmentLighting environment;
        std::array<MultiviewGraphGeometry, RenderMaterialPass::MaximumDraws> draws{};
        std::array<MultiviewGraphTarget, 2> targets{};
        std::size_t drawCount{};

        struct ReprojectionDraws {
            std::shared_ptr<SnapshotStorage> payload{std::make_shared<SnapshotStorage>()};
            std::shared_ptr<const MultiviewSceneSnapshot> scene;
            std::array<MultiviewGraphGeometry, RenderMaterialPass::MaximumDraws> draws{};
            std::array<MultiviewGraphTarget, 2> targets{};

            /** @brief Preserves exact scene and cameras while selecting the separately signed material generation. */
            Result<MultiviewSceneSnapshot> Extract(const MultiviewSceneSnapshot &projection, const MultiviewMotionHistory &history,
                                                   const MultiviewMotionFrame &frame, std::span<const DrawSlot> slots,
                                                   const MultiviewCapabilities capabilities) const {
                if (projection.Instances().size() > slots.size())
                    return Result<MultiviewSceneSnapshot>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
                std::array<MultiviewSceneInstance, RenderMaterialPass::MaximumDraws> instances{};
                std::ranges::copy(projection.Instances(), instances.begin());
                for (const auto &draw : projection.Draws())
                    if (!history.ReadDraw(frame, draw.instanceIndex))
                        return Result<MultiviewSceneSnapshot>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
                for (std::size_t index = 0; index < projection.Instances().size(); ++index) {
                    if (!slots[index].material)
                        return Result<MultiviewSceneSnapshot>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
                    instances[index].material = slots[index].material->Id();
                }
                return MultiviewSceneSnapshot::Extract(frame.publication, frame.publication, projection.Views(),
                                                       std::span{instances}.first(projection.Instances().size()), capabilities,
                                                       MultiviewExecutionPolicy::Multipass);
            }

            /** @brief Publishes only current selected draw leases and the exact admitted eye rectangles. */
            void Publish(MultiviewSceneSnapshot &&snapshot, std::span<const RenderGraphResourceId> parameters,
                         std::span<const MultiviewGraphGeometry> projection, std::span<const DrawSlot> slots) {
                payload->value.emplace(std::move(snapshot));
                scene = std::shared_ptr<const MultiviewSceneSnapshot>{payload, &*payload->value};
                for (std::size_t index = 0; index < projection.size(); ++index) {
                    const auto &draw = projection[index];
                    const auto instance = draw.instanceIndex;
                    draws[index] = {instance, draw.vertices, draw.indices, parameters[instance], slots[instance].material};
                }
            }
        };

        struct Auxiliary final : ReprojectionDraws {
            explicit Auxiliary(const EditorVulkanXrViewportAccess::AuxiliaryPlan &value) : plan(value) {}

            EditorVulkanXrViewportAccess::AuxiliaryPlan plan;

            /** @brief Binds the exact current runtime-owned auxiliary rectangles and acquired views. */
            void PublishTargets(const Projection::Topology::Auxiliary &topology,
                                const EditorVulkanXrViewportAccess::AuxiliaryImages &images) {
                for (std::uint32_t eye = 0; eye < targets.size(); ++eye) {
                    const auto [width, height] = plan.viewExtents[eye];
                    targets[eye] = {topology.motion.resource,
                                    topology.depth.resource,
                                    images.motionViews[eye],
                                    images.depthViews[eye],
                                    {0, 0, width, height},
                                    eye,
                                    eye};
                }
            }
        };

        /** @brief One cold mask allocation and its matching scratch depth. */
        struct CoverageTargets final {
            FramebufferExtent extent;
            ResourceCreation<RenderTextureHandle> color;
            ResourceCreation<RenderBufferHandle> readback;
            std::optional<std::array<MultiviewRenderRectangle, 2>> probe;
            std::uint64_t readbackInstance{};
            std::array<ResourceCreation<RenderTextureViewHandle>, 2> colorViews;
            Projection scratchDepth;
            ResourceCreation<RenderBufferHandle> reset, counters, rectangles;
            std::shared_ptr<IResidentStereoCoverageReducer> reducer;
            std::uint64_t countersInstance{};

            /** @brief Retires optional reduction before its immutable mask views and remaining coverage allocations. */
            Result<void> RetireReduction(RenderFrontend &frontend) {
                reducer.reset();
                countersInstance = 0;
                for (auto *buffer : {&rectangles, &counters, &reset}) {
                    if (!buffer->handle.IsValid())
                        continue;
                    if (auto result = Detail::RenderFrontendResourceAccess::RetireCreation(frontend, *buffer); result.HasError())
                        return result;
                    *buffer = {};
                }
                return Result<void>::Success();
            }

            /** @brief Retires exact scratch allocations after sampled descriptors and graph queue leases drain. */
            Result<void> RetireTargets(RenderFrontend &frontend) {
                if (auto retired = RetireReduction(frontend); retired.HasError())
                    return retired;
                if (readback.handle.IsValid()) {
                    if (auto result = Detail::RenderFrontendResourceAccess::RetireCreation(frontend, readback); result.HasError())
                        return result;
                    readback = {};
                    readbackInstance = 0;
                }
                for (auto &view : colorViews) {
                    if (!view.handle.IsValid())
                        continue;
                    if (auto result = Detail::RenderFrontendResourceAccess::RetireCreation(frontend, view); result.HasError())
                        return result;
                    view = {};
                }
                if (color.handle.IsValid()) {
                    if (auto result = Detail::RenderFrontendResourceAccess::RetireCreation(frontend, color); result.HasError())
                        return result;
                    color = {};
                }
                return scratchDepth.RetireDepth(frontend);
            }

            /** @brief Allocates the exact immutable copy source and rect block plus one mutable GPU counter. */
            Result<void> PrepareReductionBuffers(RenderFrontend &frontend, const std::array<MultiviewRenderRectangle, 2> &eyes) {
                constexpr auto scope = RenderMemoryScopes::ViewportResources;
                constexpr std::array<std::byte, RenderGraphStereoCoverageReduction::CounterBytes> zeros{};
                auto zero =
                    frontend.CreateBuffer(scope, {zeros.size(), RenderBufferUsage::CopySource, RenderBufferAccess::HostVisible}, zeros);
                if (zero.HasError())
                    return Result<void>::Failure(zero.ErrorValue());
                reset = zero.Value();
                auto counter = Detail::RenderFrontendResourceAccess::CreateStereoCoverageCounter(frontend, scope);
                if (counter.HasError())
                    return Result<void>::Failure(counter.ErrorValue());
                counters = counter.Value();
                const auto &left = eyes.front();
                const auto &right = eyes.back();
                const std::array packed{left.x, left.y, left.width, left.height, right.x, right.y, right.width, right.height};
                const auto data = std::as_bytes(std::span{packed});
                auto uniform =
                    frontend.CreateBuffer(scope, {data.size(), RenderBufferUsage::Uniform, RenderBufferAccess::HostVisible}, data);
                if (uniform.HasError())
                    return Result<void>::Failure(uniform.ErrorValue());
                rectangles = uniform.Value();
                const auto processed = frontend.ProcessResourceRequests();
                return processed.HasError() ? Result<void>::Failure(processed.ErrorValue()) : Result<void>::Success();
            }

            /** @brief Seals the exact frontend-owned mask layer descriptors and buffer generations through the bridge. */
            Result<void> PrepareReduction(RenderFrontend &frontend, Detail::IVulkanEditorGraphicsPort &port,
                                          const std::span<const std::uint8_t> artifact, const RenderQueueId queue,
                                          const std::array<FramebufferExtent, 2> &extents) {
                const std::array eyes{MultiviewRenderRectangle{0, 0, extents[0].width, extents[0].height},
                                      MultiviewRenderRectangle{0, 0, extents[1].width, extents[1].height}};
                if (auto prepared = PrepareReductionBuffers(frontend, eyes); prepared.HasError())
                    return prepared;
                Detail::VulkanStereoCoveragePreparation request{.artifact = artifact, .rectangles = eyes, .queue = queue};
                for (std::size_t eye = 0; eye < colorViews.size(); ++eye) {
                    const auto native = Detail::RenderFrontendResourceAccess::BackendInstance(frontend, colorViews[eye].handle);
                    if (native.HasError())
                        return Result<void>::Failure(native.ErrorValue());
                    request.eyes[eye] = native.Value();
                }
                const std::array slots{std::pair{reset.handle, &request.reset}, std::pair{counters.handle, &request.result},
                                       std::pair{rectangles.handle, &request.parameters}};
                for (const auto &[buffer, instance] : slots) {
                    const auto native = Detail::RenderFrontendResourceAccess::BackendInstance(frontend, buffer);
                    if (native.HasError())
                        return Result<void>::Failure(native.ErrorValue());
                    *instance = native.Value();
                }
                auto prepared = port.PrepareStereoCoverageReduction(request);
                if (prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
                reducer = std::move(prepared).Value();
                countersInstance = request.result;
                return Result<void>::Success();
            }

            /** @brief Creates one bounded coherent staging allocation before submission. */
            Result<void> PrepareReadback(RenderFrontend &frontend) {
                const auto bytes = RenderGraphStereoCoverageCopy{.tiles = *probe}.PackedBytes();
                auto created = frontend.CreateBuffer(RenderMemoryScopes::ViewportResources,
                                                     {bytes, RenderBufferUsage::CopyDestination, RenderBufferAccess::HostVisible}, {});
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                readback = created.Value();
                if (auto processed = frontend.ProcessResourceRequests(); processed.HasError())
                    return Result<void>::Failure(processed.ErrorValue());
                if (auto ready = frontend.ResourceOperationResult(readback.operation); ready.HasError())
                    return ready;
                const auto native = Detail::RenderFrontendResourceAccess::BackendInstance(frontend, readback.handle);
                if (native.HasError())
                    return Result<void>::Failure(native.ErrorValue());
                readbackInstance = native.Value();
                return Result<void>::Success();
            }

            /** @brief Cold allocates owned scalar color plus a separate stereo hardware-depth target. */
            Result<void> PrepareTargets(RenderFrontend &frontend, const FramebufferExtent allocation) {
                extent = allocation;
                RenderTextureDescriptor output{.extent = allocation,
                                               .format = RenderTextureFormat::R32Float,
                                               .layerCount = static_cast<std::uint32_t>(colorViews.size()),
                                               .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment};
                if (auto depth = scratchDepth.PrepareDepth(frontend, output, false); depth.HasError())
                    return depth;
                if (probe) {
                    auto copySource = output;
                    copySource.usage = copySource.usage | RenderTextureUsage::CopySource;
                    if (!frontend.Capabilities().support.Supports(copySource) || !std::ranges::all_of(*probe, [&](const auto &tile) {
                        return tile.Fits(output.extent);
                    }))
                        probe.reset();
                    else
                        output = copySource;
                }
                auto created = frontend.CreateTexture(RenderMemoryScopes::ViewportResources, output);
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                color = created.Value();
                if (auto processed = frontend.ProcessResourceRequests(); processed.HasError())
                    return Result<void>::Failure(processed.ErrorValue());
                if (auto ready = frontend.ResourceOperationResult(color.operation); ready.HasError())
                    return ready;
                for (std::uint32_t eye = 0; eye < colorViews.size(); ++eye) {
                    auto view = frontend.CreateTextureView({.texture = color.handle, .format = output.format, .baseLayer = eye});
                    if (view.HasError())
                        return Result<void>::Failure(view.ErrorValue());
                    colorViews[eye] = view.Value();
                    if (auto processed = frontend.ProcessResourceRequests(); processed.HasError())
                        return Result<void>::Failure(processed.ErrorValue());
                    if (auto ready = frontend.ResourceOperationResult(view.Value().operation); ready.HasError())
                        return ready;
                }
                return probe ? PrepareReadback(frontend) : Result<void>::Success();
            }

            /** @brief Returns only known GPU codes from the exact completed staging generation. */
            std::optional<EditorVulkanXrViewportAccess::CoveragePixels> ReadPixels(Detail::IVulkanEditorGraphicsPort *port,
                                                                                   const RenderTimelinePoint submission) const {
                if (!probe || !port || readbackInstance == 0)
                    return std::nullopt;
                EditorVulkanXrViewportAccess::CoveragePixels output{.tiles = *probe};
                output.count = RenderGraphStereoCoverageCopy{.tiles = *probe}.PackedBytes() / sizeof(float);
                auto samples = std::span{output.samples}.first(output.count);
                const auto copied = port->ReadCompletedStereoCoverage(readbackInstance, submission, std::as_writable_bytes(samples));
                if (copied.HasError() || !copied.Value() || !std::ranges::all_of(samples, [](float value) {
                    return std::isfinite(value) && value >= 0.0F &&
                           value <= static_cast<float>(EditorVulkanReprojectionCoverage::DepthConsistent) && std::trunc(value) == value;
                }))
                    return std::nullopt;
                return output;
            }
        };

        /** @brief Shared draw bindings sample the same opposite history slot in both mask layouts. */
        struct Coverage final : ReprojectionDraws {
            CoverageTargets ordinary, paired;
            std::array<std::array<DrawSlot, RenderMaterialPass::MaximumDraws>, 2> slots;
            std::optional<MultiviewMotionFrame> frame, predecessor;
            std::optional<EditorVulkanXrViewportAccess::AuxiliaryImages> pairedImages;
            RenderTimelinePoint submission;
            std::size_t drawCapacity{};
            bool ready{};
            bool coverageTaken{};
            bool reductionTaken{};

            /** @brief Discards any unconsumed live observation on rebind, completion or invalidation. */
            void ResetObservation() noexcept {
                frame.reset();
                predecessor.reset();
                pairedImages.reset();
                submission = {};
                coverageTaken = false;
                reductionTaken = false;
            }

            /** @brief Returns the exact scratch storage selected by the live coverage binding. */
            [[nodiscard]] const CoverageTargets &CurrentTargets() const noexcept {
                return pairedImages ? paired : ordinary;
            }

            /** @brief Retires each independently owned mask and its partial creation debt. */
            Result<void> RetireTargets(RenderFrontend &frontend) {
                if (auto retired = paired.RetireTargets(frontend); retired.HasError())
                    return retired;
                return ordinary.RetireTargets(frontend);
            }

            /** @brief Maps coverage to the current auxiliary raster when paired, or the full projection for diagnostics. */
            void PublishTargets(const Projection::Topology::Coverage &topology, const Projection &prior, const Auxiliary *auxiliary,
                                const EditorVulkanXrViewportAccess::AuxiliaryImages *images) {
                if (images)
                    pairedImages = *images;
                const auto &surface = CurrentTargets();
                for (std::uint32_t eye = 0; eye < targets.size(); ++eye) {
                    const auto rectangle = images ? auxiliary->targets[eye].rectangle
                                                  : MultiviewRenderRectangle{0, 0, surface.extent.width, surface.extent.height};
                    targets[eye] = {topology.color,
                                    topology.depth,
                                    surface.colorViews[eye].handle,
                                    surface.scratchDepth.depthViews[eye].handle,
                                    rectangle,
                                    eye,
                                    eye,
                                    topology.predecessor,
                                    prior.depthViews[eye].handle,
                                    eye};
                }
            }
        };

        std::optional<Auxiliary> auxiliary;
        std::optional<Coverage> coverage;
        bool retiring{};
        const CompiledRenderGraphExecution *bound{};
        EditorVulkanXrMirrorProjection mirror;
        EditorVulkanXrProjectionAlphaState alpha;
        RenderTextureHandle projectionColor;
        std::optional<EditorVulkanXrViewportAccess::ProjectionSubmission> encoded;
        RenderTimelinePoint submitted;
        MultiviewMotionHistory motion;
        std::optional<MultiviewMotionFrame> motionFrame;
        bool motionSubmitted{};

        /** @brief Checks the exact live frame, retained predecessor and observed graphics submission. */
        [[nodiscard]] bool ObservedCoverage(const MultiviewMotionFrame &frame) const noexcept {
            return coverage && coverage->frame == frame && previousDepth && coverage->predecessor == previousDepth->frame &&
                   motionFrame == frame && coverage->submission.IsValid() && coverage->submission == observed;
        }

        /** @brief Binds each cold material table to the other stable depth slot before a frame can be acquired. */
        Result<void> PrepareCoverage(EditorViewportRendererVulkan &viewport, std::span<const EditorViewportMeshResourceView> meshes,
                                     const RenderTextureDescriptor &color, const RenderGraphLogicalState &acquired,
                                     const RenderGraphLogicalState &requiredFinal, const std::size_t drawCapacity) {
            auto &work = coverage.emplace();
            work.drawCapacity = drawCapacity;
            if (const auto *port = viewport.bridge_.Port(); port && port->SupportsStereoCoverageReadback())
                work.ordinary.probe = viewport.xrCoverageProbe_;
            if (auto targets = work.ordinary.PrepareTargets(viewport.frontend_, color.extent); targets.HasError())
                return targets;
            const auto prepare = [&](const std::size_t index, const CoverageTargets &targets,
                                     const EditorVulkanXrViewportAccess::AuxiliaryPlan *aux, const bool reduce = false) {
                auto &output = history[index];
                const auto &prior = history[1 - index];
                auto &topology = reduce ? output.pairedReduced : aux ? output.pairedCovered : output.covered;
                const Projection::CoveragePlan plan{targets.color.handle,
                                                    targets.scratchDepth.depthTexture.handle,
                                                    prior.depthTexture.handle,
                                                    std::span{work.slots[index]}.first(drawCapacity),
                                                    targets.readback.handle,
                                                    reduce ? targets.reset.handle : RenderBufferHandle{},
                                                    reduce ? targets.counters.handle : RenderBufferHandle{}};
                return topology.emplace().Prepare({viewport.frontend_,
                                                   viewport.resources_,
                                                   meshes,
                                                   viewport.xrDrawSlots_[0],
                                                   {color, acquired, requiredFinal},
                                                   output.depthTexture.handle,
                                                   aux,
                                                   viewport.xrMotionDrawSlots_,
                                                   &plan});
            };
            for (std::size_t index = 0; index < history.size(); ++index) {
                const auto &prior = history[1 - index];
                const auto &bindings = MultiviewShaderParameterIds::PreviousEyeDepthBindings;
                const std::array sampled{MaterialResourceBinding{bindings.front(), 0, prior.depthViews.front().handle},
                                         MaterialResourceBinding{bindings.back(), 0, prior.depthViews.back().handle}};
                const auto &program = viewport.xrCoverageShader_;
                auto slots = std::span{work.slots[index]}.first(drawCapacity);
                for (std::size_t draw = 0; draw < slots.size(); ++draw) {
                    const auto identity = index * slots.size() + draw + 1;
                    if (auto prepared = ProgramResources::CreateUniformSlot(viewport.frontend_, program, slots[draw], identity,
                                                                            program.pipeline, {}, sampled);
                        prepared.HasError())
                        return prepared;
                }
                if (auto result = prepare(index, work.ordinary, nullptr); result.HasError())
                    return result;
            }
            work.ready = true;
            // D16 quantization can give the motion pass a different visible surface than our D32 mask.
            if (!auxiliary || auxiliary->plan.depth.allocation.format != RenderTextureFormat::Depth32Float)
                return Result<void>::Success();

            auto paired = work.paired.PrepareTargets(viewport.frontend_, auxiliary->plan.motion.allocation.extent);
            for (std::size_t index = 0; index < history.size() && paired.HasValue(); ++index)
                paired = prepare(index, work.paired, &auxiliary->plan);
            if (paired.HasError()) {
                for (auto &output : history)
                    output.pairedCovered.reset();
                // If retiring a partial allocation fails, the same owner will retry at Scene teardown.
                (void)work.paired.RetireTargets(viewport.frontend_);
                return Result<void>::Success();
            }
            auto *port = viewport.bridge_.Port();
            const auto &artifact = viewport.xrCoverageReductionArtifact_;
            if (artifact.empty() || !port || !port->SupportsStereoCoverageReduction(acquired.queue))
                return Result<void>::Success();
            auto reduction = work.paired.PrepareReduction(viewport.frontend_, *port, artifact, acquired.queue, auxiliary->plan.viewExtents);
            for (std::size_t index = 0; index < history.size() && reduction.HasValue(); ++index)
                reduction = prepare(index, work.paired, &auxiliary->plan, true);
            if (reduction.HasError()) {
                for (auto &output : history)
                    output.pairedReduced.reset();
                (void)work.paired.RetireReduction(viewport.frontend_);
            }
            return Result<void>::Success();
        }

        /** @brief Stores fixed scene/draw values only after canonical Bind proved the previous queue lease retired. */
        void PublishScene(const Projection &projection, const Projection::Topology &topology, MultiviewSceneSnapshot &&extracted,
                          const std::array<RenderTextureViewHandle, 2> &eyeViews, std::span<const DrawSlot> slots,
                          const std::array<const Projection::Geometry *, RenderMaterialPass::MaximumDraws> &meshes,
                          const std::array<MultiviewRenderRectangle, 2> &rectangles) {
            active = &topology;
            payload->value.emplace(std::move(extracted));
            scene = std::shared_ptr<const MultiviewSceneSnapshot>{payload, &*payload->value};
            drawCount = scene->Draws().size();
            for (std::size_t draw = 0; draw < drawCount; ++draw) {
                const auto instance = scene->Draws()[draw].instanceIndex;
                draws[draw] = {instance, meshes[instance]->vertices, meshes[instance]->indices, topology.parameters[instance],
                               slots[instance].material};
            }
            for (std::uint32_t view = 0; view < targets.size(); ++view)
                targets[view] = {topology.color.resource,
                                 topology.depth,
                                 eyeViews[view],
                                 projection.depthViews[view].handle,
                                 rectangles[view],
                                 view,
                                 view};
        }
    };

    /** @copydoc EditorVulkanXrViewportAccess::IsPrepared */
    bool EditorVulkanXrViewportAccess::IsPrepared(const EditorViewportRendererVulkan &viewport, const PreparedTargets &receipt) noexcept {
        if (viewport.owner_ != std::this_thread::get_id() || !viewport.initialized_ || !viewport.xrGraph_ || viewport.xrGraph_->retiring)
            return false;
        const auto &support = viewport.frontend_.Capabilities().support;
        return receipt.graph_.lock() == viewport.xrGraph_ && receipt.device_ == support.deviceIncarnation &&
               receipt.capabilities_ == support.capabilityRevision;
    }

    /** @copydoc EditorVulkanXrViewportAccess::HasPreparedReduction */
    bool EditorVulkanXrViewportAccess::HasPreparedReduction(const EditorViewportRendererVulkan &viewport,
                                                            const PreparedTargets &receipt) noexcept {
        if (!IsPrepared(viewport, receipt))
            return false;
        const auto &graph = *viewport.xrGraph_;
        return graph.historyEnabled && graph.auxiliary && graph.coverage && graph.coverage->ready && graph.coverage->paired.reducer &&
               std::ranges::all_of(graph.history, [](const auto &slot) {
            return slot.pairedReduced && slot.pairedReduced->topology;
        });
    }

    /** @copydoc EditorVulkanXrViewportAccess::PreviousDepth */
    std::optional<EditorVulkanXrViewportAccess::PreviousEyeDepth> EditorVulkanXrViewportAccess::PreviousDepth(
        const EditorViewportRendererVulkan &viewport) noexcept {
        if (viewport.owner_ != std::this_thread::get_id() || !viewport.initialized_ || !viewport.xrGraph_)
            return std::nullopt;
        const auto &graph = *viewport.xrGraph_;
        if (!graph.bound || !graph.previousDepth || !graph.motionFrame)
            return std::nullopt;
        const auto &support = viewport.frontend_.Capabilities().support;
        const auto &current = graph.motionFrame->publication;
        if (support.deviceIncarnation != current.deviceIncarnation || support.capabilityRevision != current.capabilityRevision)
            return std::nullopt;
        const auto compatible = graph.motion.PreviousFrame(*graph.motionFrame);
        if (!compatible || *compatible != graph.previousDepth->frame)
            return std::nullopt;
        const auto &depth = graph.history[graph.previousDepth->slot];
        const RenderGraphLogicalState state{RenderGraphSynchronizationAccess::Write,
                                            RenderGraphSynchronizationOperation::DepthStencilAttachment, RenderGraphPipelineScope::Graphics,
                                            RenderGraphTextureLayout::DepthStencilAttachment, viewport.device_.graphicsIdentity};
        return PreviousEyeDepth{*compatible,
                                depth.depthTexture.handle,
                                {depth.depthViews.front().handle, depth.depthViews.back().handle},
                                graph.extent,
                                state};
    }

    /** @copydoc EditorVulkanXrViewportAccess::RequestCoverageProbe */
    bool EditorVulkanXrViewportAccess::RequestCoverageProbe(EditorViewportRendererVulkan &viewport,
                                                            const std::array<MultiviewRenderRectangle, 2> &tiles) noexcept {
        const auto *port = viewport.bridge_.Port();
        if (viewport.owner_ != std::this_thread::get_id() || !viewport.initialized_ || viewport.xrGraph_ || !viewport.xrCoverageCook_ ||
            !port || !port->SupportsStereoCoverageReadback() || RenderGraphStereoCoverageCopy{.tiles = tiles}.PackedBytes() == 0)
            return false;
        viewport.xrCoverageProbe_ = tiles;
        return true;
    }

    /** @copydoc EditorVulkanXrViewportAccess::TakeCoverage */
    std::optional<EditorVulkanXrViewportAccess::CoverageObservation> EditorVulkanXrViewportAccess::TakeCoverage(
        EditorViewportRendererVulkan &viewport, const MultiviewMotionFrame &frame) noexcept {
        if (viewport.owner_ != std::this_thread::get_id() || !viewport.initialized_ || !viewport.xrGraph_)
            return std::nullopt;
        auto &graph = *viewport.xrGraph_;
        const auto &support = viewport.frontend_.Capabilities().support;
        if (!graph.ObservedCoverage(frame) || graph.coverage->coverageTaken ||
            frame.publication.deviceIncarnation != support.deviceIncarnation ||
            frame.publication.capabilityRevision != support.capabilityRevision)
            return std::nullopt;
        auto &coverage = *graph.coverage;
        const auto &targets = coverage.CurrentTargets();
        CoverageObservation observation{.frame = frame,
                                        .predecessor = *coverage.predecessor,
                                        .texture = targets.color.handle,
                                        .eyes = {targets.colorViews.front().handle, targets.colorViews.back().handle},
                                        .extent = targets.extent,
                                        .rectangles = {coverage.targets.front().rectangle, coverage.targets.back().rectangle},
                                        .completion = coverage.submission,
                                        .auxiliary = coverage.pairedImages};
        try {
            observation.pixels = targets.ReadPixels(viewport.bridge_.Port(), coverage.submission);
        } catch (const std::bad_alloc &) {
            // Preserve the already proven completion fact when optional diagnostic error reporting exhausts host memory.
        }
        coverage.coverageTaken = true;
        return observation;
    }

    /** @copydoc EditorVulkanXrViewportAccess::CoverageReduction::Complete */
    bool EditorVulkanXrViewportAccess::CoverageReduction::Complete() const noexcept {
        for (std::size_t eye = 0; eye < tallies.size(); ++eye) {
            const auto &rect = rectangles[eye];
            const auto area = std::uint64_t{rect.width} * rect.height;
            if (!area || tallies[eye].visited != area || tallies[eye].unresolved != 0)
                return false;
        }
        return true;
    }

    /** @copydoc EditorVulkanXrViewportAccess::TakeCoverageReduction */
    std::optional<EditorVulkanXrViewportAccess::CoverageReduction> EditorVulkanXrViewportAccess::TakeCoverageReduction(
        EditorViewportRendererVulkan &viewport, const MultiviewMotionFrame &frame) noexcept {
        if (viewport.owner_ != std::this_thread::get_id() || !viewport.xrGraph_ || !viewport.initialized_)
            return std::nullopt;
        auto &graph = *viewport.xrGraph_;
        const auto &support = viewport.frontend_.Capabilities().support;
        if (!graph.ObservedCoverage(frame) || frame.publication.deviceIncarnation != support.deviceIncarnation ||
            frame.publication.capabilityRevision != support.capabilityRevision)
            return std::nullopt;
        auto &coverage = *graph.coverage;
        auto *port = viewport.bridge_.Port();
        if (coverage.reductionTaken || !coverage.pairedImages || !graph.active || !graph.active->coverage ||
            !graph.active->coverage->reductionPass.IsValid() || !port)
            return std::nullopt;
        const auto &target = coverage.CurrentTargets();
        CoverageReduction result{.frame = frame,
                                 .predecessor = *coverage.predecessor,
                                 .auxiliary = *coverage.pairedImages,
                                 .rectangles = {coverage.targets.front().rectangle, coverage.targets.back().rectangle},
                                 .completion = coverage.submission};
        try {
            const auto read = port->ReadCompletedStereoCoverageReduction(target.countersInstance, coverage.submission, result.tallies);
            if (read.HasError() || !read.Value())
                return std::nullopt;
        } catch (const std::bad_alloc &) {
            return std::nullopt;
        }
        coverage.reductionTaken = true;
        for (const auto &eye : result.tallies)
            if (eye.unresolved > eye.visited)
                return std::nullopt;
        return result;
    }

    /** @copydoc EditorViewportRendererVulkan::BorrowXrDevice */
    Result<Detail::VulkanEditorDevice> EditorViewportRendererVulkan::BorrowXrDevice() const {
        if (owner_ != std::this_thread::get_id() || !initialized_ || !bridge_.Port() || !xrShader_.pipeline.IsValid() ||
            !xrShader_.materials || !xrShader_.uniformBytes)
            return Result<Detail::VulkanEditorDevice>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        auto device = bridge_.Port()->QueryDevice();
        if (device.HasError())
            return device;
        if (device.Value().device != device_.device || device.Value().instance != device_.instance ||
            device.Value().physical != device_.physical || device.Value().graphicsIdentity != device_.graphicsIdentity)
            return Result<Detail::VulkanEditorDevice>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        return device;
    }

    namespace {
        /** @brief Accepts only exact sampled native stereo array storage for both auxiliary attachments. */
        bool ValidAuxiliaryAllocation(const EditorVulkanXrViewportAccess::AuxiliaryTexture &texture, const FramebufferExtent extent,
                                      const RenderQueueId queue, const RenderGraphTextureLayout layout) {
            const auto &allocation = texture.allocation;
            const auto usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment;
            return allocation.IsValid() && allocation.dimension == RenderTextureDimension::TwoD && allocation.extent == extent &&
                   allocation.layerCount == 2 && allocation.sampleCount == 1 && allocation.mipCount == 1 &&
                   HasTextureUsage(allocation.usage, usage) && texture.acquired.queue == queue && texture.requiredFinal.queue == queue &&
                   texture.acquired.layout == layout && texture.requiredFinal.layout == layout;
        }

        /** @brief Validates the same native extent envelope from which exact per-eye rectangles will be written. */
        bool ValidAuxiliaryPlan(const EditorVulkanXrViewportAccess::AuxiliaryPlan &plan, const RenderQueueId queue) {
            const auto &[left, right] = plan.viewExtents;
            if (!left.IsValid() || !right.IsValid())
                return false;
            const FramebufferExtent extent{std::max(left.width, right.width), std::max(left.height, right.height)};
            return ValidAuxiliaryAllocation(plan.motion, extent, queue, RenderGraphTextureLayout::ColorAttachment) &&
                   ValidAuxiliaryAllocation(plan.depth, extent, queue, RenderGraphTextureLayout::DepthStencilAttachment);
        }
    }  // namespace

    /** @copydoc EditorViewportRendererVulkan::PrepareXrGraph */
    Result<void> EditorViewportRendererVulkan::PrepareXrGraph(std::span<const EditorViewportMeshResourceView> meshes,
                                                              const RenderTextureDescriptor &color, const RenderGraphLogicalState &acquired,
                                                              const RenderGraphLogicalState &requiredFinal, const bool retainPreviousDepth,
                                                              const EditorVulkanXrViewportAccess::AuxiliaryPlan *auxiliary,
                                                              const std::size_t coverageDrawCapacity) {
        if (owner_ != std::this_thread::get_id() || !initialized_ || !xrShader_.pipeline.IsValid() || xrGraph_ ||
            meshes.size() > RenderMaterialPass::MaximumDraws || !color.IsValid() || color.dimension != RenderTextureDimension::TwoD ||
            color.format != RenderTextureFormat::Rgba8Unorm || color.layerCount != 2 || color.mipCount != 1 || color.sampleCount != 1 ||
            color.usage != (RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled) ||
            acquired.queue != device_.graphicsIdentity || requiredFinal.queue != device_.graphicsIdentity ||
            acquired.layout != RenderGraphTextureLayout::ColorAttachment ||
            requiredFinal.layout != RenderGraphTextureLayout::ColorAttachment)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        if (auxiliary && (!retainPreviousDepth ||
                          xrMotionFormats_ != std::pair{auxiliary->motion.allocation.format, auxiliary->depth.allocation.format} ||
                          !xrMotionShader_.pipeline.IsValid() || !ValidAuxiliaryPlan(*auxiliary, acquired.queue)))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        if (auto prepared = resources_.PrepareMeshesCold(meshes); prepared.HasError())
            return prepared;
        if (const auto support = ResolveMultiviewCapabilities(frontend_.Capabilities().support);
            !support.multipass || !support.indexedShaders || support.maximumViews < 2)
            return Result<void>::Failure(
                MakeError(RendererErrors::ViewportInvalidScene, "The selected renderer has no admitted two-view indexed shader path."));

        // PERF: All graph vectors, optional-read scratch, native depth storage and views are cold allocations.
        // Partial preparation remains in this owner; no normal acquisition compiles or grows this storage.
        xrGraph_ = std::make_shared<XrGraph>();
        auto &storage = *xrGraph_;
        storage.extent = color.extent;
        storage.allocation = {color, acquired, requiredFinal};
        const auto prepare = [&](XrGraph::Projection &target, const bool sampled) {
            return target.Prepare(frontend_, meshes, resources_, xrDrawSlots_[0], color, acquired, requiredFinal, sampled, auxiliary,
                                  xrMotionDrawSlots_);
        };
        const bool wantsCoverage = xrCoverageCook_ && coverageDrawCapacity > 0 && coverageDrawCapacity <= RenderMaterialPass::MaximumDraws;
        const auto prepareCoverage = [&] {
            if (!storage.historyEnabled || !wantsCoverage)
                return;
            if (PrepareXrCoveragePipeline().HasError() ||
                storage.PrepareCoverage(*this, meshes, color, acquired, requiredFinal, coverageDrawCapacity).HasError())
                (void)RetireXrCoverage();
        };
        if (auxiliary) {
            for (auto &slot : storage.history)
                if (auto prepared = prepare(slot, true); prepared.HasError())
                    return prepared;
            storage.auxiliary.emplace(*auxiliary);
            storage.historyEnabled = true;
            prepareCoverage();
            return Result<void>::Success();
        }
        if (retainPreviousDepth && (wantsCoverage || (xrMotionShader_.pipeline.IsValid() && xrMotionShader_.materials))) {
            auto &first = storage.history.front();
            if (prepare(first, true).HasValue()) {
                auto &second = storage.history.back();
                storage.historyEnabled = prepare(second, true).HasValue();
                if (!storage.historyEnabled) {
                    storage.single = &first;
                    (void)second.RetireDepth(frontend_);  // Any unsuccessful release remains owned for Scene retirement.
                }
                prepareCoverage();
                return Result<void>::Success();
            }
            (void)first.RetireDepth(frontend_);
        }
        return prepare(storage.baseline, false);
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareXrCanvas */
    Result<bool> EditorViewportRendererVulkan::PrepareXrCanvas(std::span<const EditorViewportMeshResourceView> meshes,
                                                               const EditorVulkanXrViewportAccess::CanvasPreparation &canvas) {
        using Outcome = Result<bool>;
        if (const auto owner = Detail::RenderFrontendResourceAccess::RequirePreparationBoundary(frontend_); owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        if (!initialized_ || !xrGraph_ || xrGraph_->retiring || !canvas.writer.artifact.reflection)
            return Outcome::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        auto &storage = *xrGraph_;
        if (storage.bound || (storage.lastCanvasVariant && Detail::PreparedRenderGraphTemplate::InFlight(*storage.lastCanvasVariant)))
            return Outcome::Success(false);
        const XrGraph::Projection::Topology::ScenePreparation scene{frontend_,
                                                                    resources_,
                                                                    meshes,
                                                                    xrDrawSlots_[0],
                                                                    storage.allocation,
                                                                    storage.CanvasOutput().depthTexture.handle};
        if (const auto ready = storage.PrepareCanvas(*this, scene, canvas); ready.HasError())
            return Outcome::Failure(ready.ErrorValue());
        return Outcome::Success(true);
    }

    /** @copydoc EditorViewportRendererVulkan::BindXrFrame */
    Result<void> EditorViewportRendererVulkan::BindXrFrame(
        RenderTextureHandle color, const std::array<RenderTextureViewHandle, 2> &eyeViews, const MultiviewPublication &publication,
        std::span<const MultiviewSceneView> views, std::span<const RenderStaticMeshInstance> instances, const MultiviewMotionFrame *motion,
        std::span<const RenderMotionObjectId> motionObjects, const EditorXrProjectionAlpha alpha,
        const EditorVulkanXrViewportAccess::AuxiliaryImages *auxiliary, const MultiviewEnvironmentLighting *lighting,
        const std::array<MultiviewRenderRectangle, 2> *rectangles, const EditorVulkanXrViewportAccess::CanvasPlacement *canvas) {
        if (owner_ == std::this_thread::get_id() && xrGraph_) {
            xrGraph_->bound = nullptr;
            xrGraph_->active = nullptr;
            xrGraph_->mirror.ClearAcquisition();
            xrGraph_->alpha.Reset();
            xrGraph_->encoded.reset();
            xrGraph_->motionFrame.reset();
            xrGraph_->environment = {};
            xrGraph_->motionSubmitted = false;
            xrGraph_->canvasPlacement.reset();
            if (auto &coverage = xrGraph_->coverage) {
                coverage->ResetObservation();
            }
        }
        if (owner_ != std::this_thread::get_id() || !initialized_ || !xrGraph_ || !publication.IsValid() || views.size() != 2 ||
            instances.size() > RenderMaterialPass::MaximumDraws ||
            (alpha != EditorXrProjectionAlpha::Opaque && alpha != EditorXrProjectionAlpha::Premultiplied))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        auto &storage = *xrGraph_;
        if (canvas && (!storage.canvas || canvas->publication != publication || auxiliary || motion))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        if (storage.lastCanvasVariant && Detail::PreparedRenderGraphTemplate::InFlight(*storage.lastCanvasVariant))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        const MultiviewRenderRectangle whole{0, 0, storage.extent.width, storage.extent.height};
        const auto activeRectangles = rectangles ? *rectangles : std::array{whole, whole};
        if (!std::ranges::all_of(activeRectangles, [extent = storage.extent](const auto &rectangle) {
            return rectangle.Fits(extent);
        }))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene, "XR eye region exceeds the acquired allocation."));
        if (lighting && (!xrEnvironmentLighting_ || !lighting->IsValid()))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        if (const auto &support = frontend_.Capabilities().support;
            support.deviceIncarnation != publication.deviceIncarnation || support.capabilityRevision != publication.capabilityRevision)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));

        if (storage.historyEnabled && storage.submitted.IsValid() && storage.observed != storage.submitted) {
            if (auto polled = PollXrQueue(); polled.HasError())
                return polled;
            if (storage.observed != storage.submitted)
                return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        }

        // Temporary fixed values do not touch a potentially queued slot. Only successful canonical Bind
        // below proves the sole instance has retired, permitting mutation of its retained payload storage.
        auto &projection = canvas ? storage.CanvasOutput() : storage.SelectOutput();
        if (auxiliary && (!projection.paired || !storage.auxiliary))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        auto &ordinary = auxiliary ? *projection.paired : projection.ordinary;
        auto &baseline = canvas ? *storage.canvas : ordinary;
        std::array<const XrGraph::Projection::Geometry *, RenderMaterialPass::MaximumDraws> meshes{};
        auto extracted = baseline.ExtractScene(publication, views, instances, frontend_, xrDrawSlots_[0], meshes);
        if (extracted.HasError())
            return Result<void>::Failure(extracted.ErrorValue());
        const bool staged = motion && storage.motion.Stage(*motion, extracted.Value(), motionObjects);
        const bool hasPrior = staged && storage.previousDepth && storage.motion.PreviousFrame(*motion) == storage.previousDepth->frame;
        std::optional<MultiviewSceneSnapshot> auxiliaryScene;
        if (auxiliary) {
            if (!hasPrior)
                return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
            auto motionScene = storage.auxiliary->Extract(extracted.Value(), storage.motion, *motion, xrMotionDrawSlots_,
                                                          ResolveMultiviewCapabilities(frontend_.Capabilities().support));
            if (motionScene.HasError())
                return Result<void>::Failure(motionScene.ErrorValue());
            auxiliaryScene.emplace(std::move(motionScene).Value());
        }
        auto *topology = &baseline;
        std::optional<MultiviewSceneSnapshot> coverageScene;
        auto &covered = auxiliary ? (projection.pairedReduced ? projection.pairedReduced : projection.pairedCovered) : projection.covered;
        if (hasPrior && covered && storage.coverage && storage.coverage->ready && !extracted.Value().Draws().empty()) {
            auto &coverage = *storage.coverage;
            const auto coverageSlots = std::span{coverage.slots[storage.activeHistorySlot]}.first(coverage.drawCapacity);
            auto scene = coverage.Extract(extracted.Value(), storage.motion, *motion, coverageSlots,
                                          ResolveMultiviewCapabilities(frontend_.Capabilities().support));
            if (scene.HasValue()) {
                coverageScene.emplace(std::move(scene).Value());
                topology = &*covered;
                // Geometry graph identities belong to the chosen immutable topology.
                for (std::size_t i = 0; i < instances.size(); ++i)
                    meshes[i] = std::addressof(topology->geometry[meshes[i] - baseline.geometry.data()]);
            }
        }
        auto bound = topology->BindReads(frontend_, color, extracted.Value(), meshes, auxiliary);
        if (bound.HasError() && coverageScene) {
            for (std::size_t i = 0; i < instances.size(); ++i)
                meshes[i] = std::addressof(baseline.geometry[meshes[i] - topology->geometry.data()]);
            topology = &baseline;
            coverageScene.reset();
            bound = topology->BindReads(frontend_, color, extracted.Value(), meshes, auxiliary);
        }
        if (bound.HasError())
            return Result<void>::Failure(bound.ErrorValue());
        for (auto view : eyeViews) {
            const auto state = frontend_.ResourceState(view);
            if (state.HasError())
                return Result<void>::Failure(state.ErrorValue());
            if (state.Value() != RenderResourceState::Ready)
                return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        }
        if (!storage.mirror.Bind(publication.frame, eyeViews[0]))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        storage.PublishScene(projection, *topology, std::move(extracted).Value(), eyeViews, xrDrawSlots_[0], meshes, activeRectangles);
        const auto draws = std::span{storage.draws}.first(storage.drawCount);
        if (auxiliary) {
            storage.auxiliary->Publish(std::move(*auxiliaryScene), topology->auxiliary->parameters, draws, xrMotionDrawSlots_);
            storage.auxiliary->PublishTargets(*topology->auxiliary, *auxiliary);
        }
        if (coverageScene) {
            auto &coverage = *storage.coverage;
            const auto &prior = *storage.previousDepth;
            coverage.Publish(std::move(*coverageScene), topology->coverage->parameters, draws, coverage.slots[storage.activeHistorySlot]);
            coverage.PublishTargets(*topology->coverage, storage.history[prior.slot], auxiliary ? &*storage.auxiliary : nullptr, auxiliary);
            coverage.frame = *motion;
            coverage.predecessor = prior.frame;
        }
        storage.bound = bound.Value();
        if (canvas)
            storage.canvasPlacement = *canvas;
        if (lighting)
            storage.environment = *lighting;
        storage.projectionColor = color;
        storage.alpha.Bind(alpha, storage.scene->Publication(), color);
        if (staged)
            storage.motionFrame = *motion;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::XrLogicalProjection */
    EditorViewportTextureView EditorViewportRendererVulkan::XrLogicalProjection() const noexcept {
        if (owner_ != std::this_thread::get_id() || !initialized_ || !xrGraph_)
            return {};
        // This reserved GUI identity is cold selection intent, never an acquired image or native readiness.
        // GUI texture creation excludes this identity; only XrMirrorView supplies its actual sampled generation.
        EditorViewportTextureView projection{.textureId = xrGraph_->mirror.LogicalIdentity()};
        const auto extent = xrGraph_->extent;
        if (const auto &eye = xrGraph_->targets.front().rectangle; eye.Fits(extent)) {
            const auto width = static_cast<float>(extent.width);
            const auto height = static_cast<float>(extent.height);
            projection.u0 = static_cast<float>(eye.x) / width;
            projection.v0 = static_cast<float>(eye.y) / height;
            projection.u1 = static_cast<float>(eye.x + eye.width) / width;
            projection.v1 = static_cast<float>(eye.y + eye.height) / height;
        }
        return projection;
    }

    /** @copydoc EditorViewportRendererVulkan::XrMirrorView */
    EditorViewportTextureView EditorViewportRendererVulkan::XrMirrorView(const std::uint64_t frame,
                                                                         const RenderTextureViewHandle expected) const noexcept {
        auto projection = XrLogicalProjection();
        if (!projection.IsValid())
            return {};
        projection.sampledView = xrGraph_->mirror.Borrow(frame, expected);
        if (!projection.sampledView.IsValid())
            return {};
        return projection;
    }

    /** @copydoc EditorViewportRendererVulkan::RevokeXrMirror */
    void EditorViewportRendererVulkan::RevokeXrMirror() const noexcept {
        if (owner_ == std::this_thread::get_id() && xrGraph_) {
            xrGraph_->mirror.Revoke();
            xrGraph_->alpha.Reset();
        }
    }

    /** @copydoc EditorViewportRendererVulkan::TakeXrPremultipliedProjection */
    std::optional<EditorXrPremultipliedProjection> EditorViewportRendererVulkan::TakeXrPremultipliedProjection(
        const MultiviewPublication &publication, const RenderTextureHandle color) noexcept {
        if (owner_ != std::this_thread::get_id() || !initialized_ || !xrGraph_)
            return std::nullopt;
        const auto &support = frontend_.Capabilities().support;
        if (support.deviceIncarnation != publication.deviceIncarnation || support.capabilityRevision != publication.capabilityRevision)
            return std::nullopt;
        const auto completion = xrGraph_->alpha.Take(publication, color, xrGraph_->observed);
        if (!completion)
            return std::nullopt;
        return EditorXrPremultipliedProjection{publication, color, *completion};
    }

    /** @copydoc EditorViewportRendererVulkan::WriteXrMotionDraws */
    Result<void> EditorViewportRendererVulkan::WriteXrMotionDraws(const FrameToken frame, const XrGraph &graph) {
        std::array<std::byte, MaximumUniformBytes> scratch{};
        const auto write = [&](const ShaderResources &program, const MultiviewSceneSnapshot &scene,
                               std::span<const DrawSlot> slots) -> Result<void> {
            for (const auto &draw : scene.Draws()) {
                const auto instance = draw.instanceIndex;
                const auto packed =
                    PackMultiviewMotionShaderParameters(program.layout, graph.motion, *graph.motionFrame, instance, scratch);
                if (packed.HasError())
                    return Result<void>::Failure(packed.ErrorValue());
                if (packed.Value() != program.parameterBytes)
                    return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
                auto uploaded =
                    bridge_.Port()->WriteFrameBuffer(frame, slots[instance].instance, std::span{scratch}.first(program.uniformBytes));
                if (uploaded.HasError())
                    return uploaded;
            }
            return Result<void>::Success();
        };
        if (graph.active->auxiliary) {
            if (auto motion = write(xrMotionShader_, *graph.auxiliary->scene, xrMotionDrawSlots_); motion.HasError())
                return motion;
        }
        if (graph.active->coverage)
            return write(xrCoverageShader_, *graph.coverage->scene, graph.coverage->slots[graph.activeHistorySlot]);
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::ExecuteXrFrame */
    Result<EditorVulkanXrViewportAccess::ProjectionSubmission> EditorViewportRendererVulkan::ExecuteXrFrame(
        RenderFrameScope &frame, const PrimaryOutputAttachment &output, const EditorVulkanXrViewportAccess::CanvasSource *canvas) {
        using Outcome = Result<EditorVulkanXrViewportAccess::ProjectionSubmission>;
        const auto fail = [&frame](const Error &error) {
            frame.Cancel();
            return Outcome::Failure(error);
        };
        if (owner_ != std::this_thread::get_id() || !initialized_ || !xrGraph_ || !xrGraph_->bound || !bridge_.Port())
            return fail(MakeError(RendererErrors::ViewportNotInitialized));
        auto &storage = *xrGraph_;
        const auto *bound = std::exchange(storage.bound, nullptr);
        auto *canvasGraph = storage.canvasPlacement ? std::addressof(*storage.canvas->canvas) : nullptr;
        if (canvasGraph && (!canvas || !*canvas))
            return fail(MakeError(RendererErrors::ViewportInvalidScene));
        if (auto prepared = bridge_.Port()->PreparePrimaryOutput(frame.Token(), output); prepared.HasError())
            return fail(prepared.ErrorValue());
        std::array<std::byte, MaximumUniformBytes> scratch{};
        constexpr std::array<std::uint32_t, 2> singleLayerRouting{0, 0};
        for (const auto &draw : storage.scene->Draws()) {
            const auto instance = draw.instanceIndex;
            const auto &localToWorld = storage.scene->Instances()[instance].localToWorld;
            const auto packed =
                xrEnvironmentLighting_
                    ? PackMultiviewEnvironmentParameters(xrShader_.layout, storage.scene->Views(), localToWorld, singleLayerRouting,
                                                         storage.environment, scratch)
                    : PackMultiviewShaderParameters(xrShader_.layout, storage.scene->Views(), localToWorld, singleLayerRouting, scratch);
            if (packed.HasError())
                return fail(packed.ErrorValue());
            if (packed.Value() != xrShader_.parameterBytes)
                return fail(MakeError(RendererErrors::ViewportInvalidScene));
            auto uploaded = bridge_.Port()->WriteFrameBuffer(frame.Token(), xrDrawSlots_[0][instance].instance,
                                                             std::span{scratch}.first(xrShader_.uniformBytes));
            if (uploaded.HasError())
                return fail(uploaded.ErrorValue());
        }
        const bool paired = storage.active->auxiliary.has_value();
        const bool covered = storage.active->coverage.has_value();
        if (paired || covered)
            if (auto written = WriteXrMotionDraws(frame.Token(), storage); written.HasError())
                return fail(written.ErrorValue());
        RenderGraphMultiview projection{storage.scene, storage.scene->Publication(), storage.targets,
                                        std::span{storage.draws}.first(storage.drawCount)};
        projection.clearArea = MultiviewClearArea::AttachmentLayer;
        // The immutable packaged XR program writes alpha one for covered pixels. With blending disabled,
        // clearing uncovered pixels to zero RGBA gives the compositor a premultiplied image.
        if (storage.alpha.IsPremultiplied())
            projection.clearColor = {0.0F, 0.0F, 0.0F, 0.0F};
        std::array<RenderGraphPassWorkload, 6> workStorage{};
        const auto count = canvasGraph ? 1 + canvasGraph->eyes.size() : bound->Passes().size();
        auto workloads = std::span{workStorage}.first(count);
        const auto publish = [&](RenderGraphPassRef pass, RenderGraphWorkload workload) {
            const auto passes = bound->Passes().last(count);
            const auto found = std::ranges::find(passes, pass, &RenderGraphExecutionPass::pass);
            HORO_INVARIANT(found != passes.end());
            workloads[static_cast<std::size_t>(found - passes.begin())] = {pass, std::move(workload)};
        };
        publish(storage.active->pass, std::move(projection));
        if (paired) {
            const auto &auxiliary = *storage.auxiliary;
            publish(storage.active->auxiliary->pass, RenderGraphMultiview{.scene = auxiliary.scene,
                                                                          .active = storage.scene->Publication(),
                                                                          .targets = auxiliary.targets,
                                                                          .geometry = std::span{auxiliary.draws}.first(storage.drawCount),
                                                                          .clearColor = {0.0F, 0.0F, 0.0F, 0.0F},
                                                                          .output = MultiviewOutput::MotionDepth});
        }
        if (covered) {
            const auto &coverage = *storage.coverage;
            const auto &topology = *storage.active->coverage;
            publish(topology.pass, RenderGraphMultiview{.scene = coverage.scene,
                                                        .active = storage.scene->Publication(),
                                                        .targets = coverage.targets,
                                                        .geometry = std::span{coverage.draws}.first(storage.drawCount),
                                                        .clearColor = {0.0F, 0.0F, 0.0F, 0.0F},
                                                        .output = MultiviewOutput::ReprojectionCoverage});
            if (topology.copyPass.IsValid())
                publish(topology.copyPass,
                        RenderGraphStereoCoverageCopy{topology.color, topology.readback, *coverage.CurrentTargets().probe});
            if (topology.reductionPass.IsValid()) {
                const auto &targets = coverage.CurrentTargets();
                constexpr PrimaryOutputAttachment zero{.loadOperation = AttachmentLoadOperation::Clear,
                                                       .storeOperation = AttachmentStoreOperation::Store,
                                                       .clearColor = {0, 0, 0, 0}};
                publish(topology.clearPass, RenderGraphColorAttachment{topology.color, zero});
                publish(topology.resetPass,
                        RenderGraphBufferCopy{topology.reset, topology.counters, 0, 0, RenderGraphStereoCoverageReduction::CounterBytes});
                publish(topology.reductionPass,
                        RenderGraphStereoCoverageReduction{.mask = topology.color,
                                                           .reset = topology.reset,
                                                           .result = topology.counters,
                                                           .eyes = {targets.colorViews[0].handle, targets.colorViews[1].handle},
                                                           .rectangles = {coverage.targets[0].rectangle, coverage.targets[1].rectangle},
                                                           .reducer = targets.reducer});
            }
        }
        if (canvasGraph) {
            auto clips = storage.canvasPlacement->clips;
            for (auto &clip : clips)
                clip = FlipVulkanCanvasClipY(clip);
            if (auto written = WriteCanvasQuadViews(frame.Token(), *canvasGraph->quad, 0, clips); written.HasError())
                return fail(written.ErrorValue());
            for (std::size_t eye = 0; eye < canvasGraph->eyes.size(); ++eye) {
                const auto &draw = canvasGraph->eyes[eye];
                publish(draw.pass, draw.Raster(storage.targets[eye], storage.extent));
            }
            recordedEver_ = true;
            const auto retained = xrGraph_;
            auto source = (*canvas)(*canvasGraph->paint, 0);
            if (source.HasError())
                return fail(source.ErrorValue());
            if (xrGraph_ != retained)
                return fail(MakeError(RendererErrors::ViewportStaleTarget));
            auto executed = retained->RecordCanvas(frame, std::move(source).Value(), workloads);
            if (executed.HasError())
                return fail(executed.ErrorValue());
            if (xrGraph_ != retained)
                return fail(MakeError(RendererErrors::ViewportStaleTarget));
            storage.submitted = executed.Value();
        } else {
            auto reserved = frame.ReserveSubmission(RenderQueueRole::Graphics);
            if (reserved.HasError())
                return fail(reserved.ErrorValue());
            const RenderGraphQueueSubmission submission{reserved.Value(), {0, workloads.size()}};
            recordedEver_ = true;
            if (auto executed = frame.ExecuteSubmittedGraph(*bound, workloads, std::span{&submission, 1}); executed.HasError())
                return fail(executed.ErrorValue());
            storage.submitted = reserved.Value().signal;
        }
        if (storage.canvas)
            storage.lastCanvasVariant = bound;
        storage.alpha.Submit(storage.submitted);
        storage.motionSubmitted = storage.motionFrame.has_value();
        if (covered)
            storage.coverage->submission = storage.submitted;
        storage.encoded.emplace(EditorVulkanXrViewportAccess::ProjectionSubmission{frame.Token(), storage.scene->Publication(),
                                                                                   storage.projectionColor, storage.submitted});
        return Outcome::Success(*storage.encoded);
    }

    /** @copydoc EditorVulkanXrViewportAccess::ResolveSubmission */
    Result<EditorVulkanXrViewportAccess::SubmissionState> EditorVulkanXrViewportAccess::ResolveSubmission(
        EditorViewportRendererVulkan &viewport, const ProjectionSubmission &submission) {
        using Outcome = Result<SubmissionState>;
        if (viewport.owner_ != std::this_thread::get_id())
            return Outcome::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        auto *port = viewport.bridge_.Port();
        if (!viewport.initialized_ || !viewport.xrGraph_ || !port || viewport.xrGraph_->encoded != submission)
            return Outcome::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        const auto accepted = port->WasSubmissionAccepted(submission.signal, submission.publication.deviceIncarnation);
        if (accepted.HasError())
            return Outcome::Failure(accepted.ErrorValue());
        if (accepted.Value())
            return Outcome::Success(SubmissionState::Accepted);
        if (port->FrameSlot(submission.frame).HasValue())
            return Outcome::Success(SubmissionState::Pending);

        auto &graph = *viewport.xrGraph_;
        graph.encoded.reset();
        graph.submitted = {};
        return Outcome::Success(SubmissionState::Rejected);
    }

    /** @copydoc EditorVulkanXrViewportAccess::SubmissionCompleted */
    bool EditorVulkanXrViewportAccess::SubmissionCompleted(const EditorViewportRendererVulkan &viewport,
                                                           const ProjectionSubmission &submission) noexcept {
        return viewport.owner_ == std::this_thread::get_id() && viewport.initialized_ && viewport.xrGraph_ &&
               viewport.xrGraph_->encoded == submission && viewport.xrGraph_->observed == submission.signal;
    }

    /** @copydoc EditorViewportRendererVulkan::CompleteXrMotion */
    void EditorViewportRendererVulkan::CompleteXrMotion(const std::optional<MultiviewMotionFrame> &frame, const bool projected) noexcept {
        if (owner_ != std::this_thread::get_id() || !xrGraph_)
            return;
        auto &storage = *xrGraph_;
        storage.alpha.Reset();
        storage.encoded.reset();
        if (storage.coverage && (!frame || storage.motionFrame == frame))
            storage.coverage->ResetObservation();
        if (!frame) {
            storage.motion.Reset();
            storage.previousDepth.reset();
            return;
        }
        if (frame != storage.motionFrame)
            return;
        if (!projected || !storage.motionSubmitted || !storage.motion.Publish(*frame)) {
            storage.motion.Reset();
            storage.previousDepth.reset();
        } else if (storage.historyEnabled && storage.submitted.IsValid() && storage.observed == storage.submitted) {
            storage.previousDepth = XrGraph::DepthPublication{*frame, storage.activeHistorySlot};
        } else {
            storage.previousDepth.reset();
        }
        storage.motionFrame.reset();
        storage.motionSubmitted = false;
    }

    /** @copydoc EditorViewportRendererVulkan::PollXrQueue */
    Result<void> EditorViewportRendererVulkan::PollXrQueue() {
        if (owner_ != std::this_thread::get_id() || !initialized_ || !xrGraph_)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        if (!xrGraph_->submitted.IsValid())
            return Result<void>::Success();
        // Vulkan's canonical Observe(Poll) drains actual completed slots on every call, including later
        // GUI work on this queue. The older scene point is only an admitted poll address; its observed
        // completion is NOT sufficient to release the acquired image or end the compositor frame.
        const auto observed = frontend_.ObserveSubmission(
            {.completion = xrGraph_->submitted, .expectedDeviceIncarnation = frontend_.Capabilities().support.deviceIncarnation});
        if (observed.HasError()) {
            xrGraph_->alpha.Reset();
            return Result<void>::Failure(observed.ErrorValue());
        }
        if (observed.Value())
            xrGraph_->observed = xrGraph_->submitted;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::RetireXrGraph */
    Result<void> EditorViewportRendererVulkan::RetireXrGraph() {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        if (!xrGraph_)
            return Result<void>::Success();
        xrGraph_->retiring = true;
        if (auto retired = RetireXrCoverage(); retired.HasError())
            return retired;
        for (auto *projection : {&xrGraph_->baseline, &xrGraph_->history.front(), &xrGraph_->history.back()})
            if (auto retired = projection->RetireDepth(frontend_); retired.HasError())
                return retired;
        xrGraph_->mirror.Retire();
        xrGraph_.reset();
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::RetireXrCoverage */
    Result<void> EditorViewportRendererVulkan::RetireXrCoverage() {
        ReleaseXrCoverageMaterials();
        if (auto &coverage = xrGraph_->coverage) {
            coverage->ready = false;
            for (auto &history : xrGraph_->history) {
                history.covered.reset();
                history.pairedCovered.reset();
                history.pairedReduced.reset();
            }
            for (auto &slots : coverage->slots)
                for (auto &draw : slots) {
                    if (draw.buffer.IsValid())
                        if (auto result = frontend_.ReleaseBuffer(draw.buffer); result.HasError())
                            return result;
                    draw = {};
                }
            if (auto retired = coverage->RetireTargets(frontend_); retired.HasError())
                return retired;
            coverage.reset();
        }
        RetireProgram(xrCoverageShader_, {});
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::ReleaseXrCoverageMaterials */
    void EditorViewportRendererVulkan::ReleaseXrCoverageMaterials() noexcept {
        if (xrGraph_ && xrGraph_->coverage) {
            auto &coverage = *xrGraph_->coverage;
            coverage.ResetObservation();
            coverage.draws = {};
            for (auto &slots : coverage.slots)
                for (auto &draw : slots)
                    draw.material.reset();
        }
        if (xrCoverageShader_.materials)
            static_cast<void>(xrCoverageShader_.materials->Shutdown());
        xrCoverageShader_.materials.reset();
    }

    /** @copydoc EditorViewportRendererVulkan::ReleaseXrGraph */
    void EditorViewportRendererVulkan::ReleaseXrGraph() noexcept {
        if (!xrGraph_)
            return;
        if (auto &coverage = xrGraph_->coverage) {
            for (auto &slots : coverage->slots)
                for (auto &draw : slots)
                    RetireDraw(draw);
            (void)coverage->RetireTargets(frontend_);
        }
        for (const auto *projection : {&xrGraph_->baseline, &xrGraph_->history.front(), &xrGraph_->history.back()}) {
            for (const auto &view : projection->depthViews)
                if (view.handle.IsValid())
                    static_cast<void>(frontend_.ReleaseTextureView(view.handle));
            if (projection->depthTexture.handle.IsValid())
                static_cast<void>(frontend_.ReleaseTexture(projection->depthTexture.handle));
        }
        xrGraph_.reset();
    }
}  // namespace Horo::Editor
