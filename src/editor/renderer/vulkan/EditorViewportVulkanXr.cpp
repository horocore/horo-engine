#include "EditorViewportRendererVulkan.h"
#include "Horo/Runtime/Render/MultiviewShaderParameters.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderFrontendResourceAccess.h"
#include "RenderGraphTemplate.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "host/EditorVulkanXrMirrorProjection.h"
#include "host/EditorVulkanXrViewportAccess.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace Horo::Editor {
    using namespace Render;

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
                                                       const AuxiliaryPlan *auxiliary) {
        return viewport.PrepareXrGraph(meshes, color, acquired, requiredFinal, retainPreviousDepth, auxiliary);
    }

    /** @copydoc EditorVulkanXrViewportAccess::Bind */
    Result<void> EditorVulkanXrViewportAccess::Bind(EditorViewportRendererVulkan &viewport, RenderTextureHandle color,
                                                    const std::array<RenderTextureViewHandle, 2> &views,
                                                    const MultiviewPublication &publication, std::span<const MultiviewSceneView> cameras,
                                                    std::span<const RenderStaticMeshInstance> instances, const MotionInput *motion,
                                                    const EditorXrProjectionAlpha alpha, const AuxiliaryImages *auxiliary) {
        return viewport.BindXrFrame(color, views, publication, cameras, instances, motion ? &motion->frame : nullptr,
                                    motion ? motion->objects : std::span<const RenderMotionObjectId>{}, alpha, auxiliary);
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
    Result<void> EditorVulkanXrViewportAccess::Execute(EditorViewportRendererVulkan &viewport, RenderFrameScope &frame,
                                                       const PrimaryOutputAttachment &output) {
        return viewport.ExecuteXrFrame(frame, output);
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
        static constexpr std::size_t MaximumReads = MaximumDraws * 4;
        static constexpr std::size_t MaximumAttachments = 4;
        ResourceCreation<RenderTextureHandle> depthTexture;
        std::array<ResourceCreation<RenderTextureViewHandle>, 2> depthViews{};

        struct Geometry final {
            std::uint64_t source{};
            RenderMeshSourceHandle sourceGeneration;
            RenderMeshHandle mesh;
            RenderGraphResourceId vertices;
            RenderGraphResourceId indices;
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

            struct Auxiliary final {
                Detail::RenderGraphExternalTextureSlot motion;
                Detail::RenderGraphExternalTextureSlot depth;
                RenderGraphPassRef pass;
                std::array<RenderGraphResourceId, MaximumDraws> parameters{};
            };

            std::optional<Auxiliary> auxiliary;

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

            /** @brief Declares the fixed external/depth attachment topology before any acquisition. */
            Result<RenderGraphBuilder> PrepareAttachments(const RenderTextureDescriptor &colorDescriptor,
                                                          const RenderGraphLogicalState &acquired,
                                                          const RenderGraphLogicalState &requiredFinal, std::uint64_t deviceIncarnation,
                                                          RenderTextureHandle projectionDepth,
                                                          const EditorVulkanXrViewportAccess::AuxiliaryPlan *plan) {
                const auto maximumPasses = plan ? 2U : 1U;
                auto created = RenderGraphBuilder::Create({.maxPasses = maximumPasses,
                                                           .maxResources = MaximumReads + MaximumAttachments,
                                                           .maxUsages = MaximumDraws * 3 * maximumPasses + MaximumAttachments,
                                                           .maxDependencies = 1});
                if (created.HasError())
                    return Result<RenderGraphBuilder>::Failure(created.ErrorValue());
                auto builder = std::move(created).Value();
                auto createdPass = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
                if (createdPass.HasError())
                    return Result<RenderGraphBuilder>::Failure(createdPass.ErrorValue());
                this->pass = createdPass.Value();
                auto external = Detail::PreparedRenderGraphTemplate::DeclareTexture(builder, colorDescriptor, acquired, requiredFinal,
                                                                                    deviceIncarnation);
                if (external.HasError())
                    return Result<RenderGraphBuilder>::Failure(external.ErrorValue());
                this->color = external.Value();
                auto importedDepth = builder.ImportTexture(projectionDepth, RenderGraphResourceClass::Persistent);
                if (importedDepth.HasError())
                    return Result<RenderGraphBuilder>::Failure(importedDepth.ErrorValue());
                this->depth = importedDepth.Value();
                if (auto use = builder.AddUsage(
                        {this->pass, this->color.resource, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
                    use.HasError())
                    return Result<RenderGraphBuilder>::Failure(use.ErrorValue());
                if (auto use =
                        builder.AddUsage({this->pass, this->depth, RenderGraphAccess::Write, RenderGraphUsageKind::DepthStencilAttachment});
                    use.HasError())
                    return Result<RenderGraphBuilder>::Failure(use.ErrorValue());
                if (plan)
                    if (auto declared = DeclareAuxiliary(builder, *plan, deviceIncarnation); declared.HasError())
                        return Result<RenderGraphBuilder>::Failure(declared.ErrorValue());
                return Result<RenderGraphBuilder>::Success(std::move(builder));
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
                                      std::span<const DrawSlot> slots, std::span<const DrawSlot> motionSlots, RenderQueueId queue) {
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
                std::array<bool, MaximumReads + MaximumAttachments> selected{};
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
                }
                std::array<Detail::RenderGraphExternalTextureBinding, 3> bindings{{{color.resource, colorTexture}}};
                if (images) {
                    bindings[1] = {auxiliary->motion.resource, images->motion};
                    bindings[2] = {auxiliary->depth.resource, images->depth};
                }
                return topology->Bind(frontend, 0, std::span{bindings}.first(images ? bindings.size() : 1),
                                      std::span{selectedReads}.first(readCount));
            }

            /** @brief Warms one exact graph variant before acquiring any viewport images. */
            Result<void> Prepare(RenderFrontend &frontend, std::span<const EditorViewportMeshResourceView> meshes,
                                 const EditorViewportResources &resources, std::span<const DrawSlot> slots,
                                 const RenderTextureDescriptor &colorDescriptor, const RenderGraphLogicalState &acquired,
                                 const RenderGraphLogicalState &requiredFinal, RenderTextureHandle projectionDepth,
                                 const EditorVulkanXrViewportAccess::AuxiliaryPlan *plan, std::span<const DrawSlot> motionSlots) {
                auto created = PrepareAttachments(colorDescriptor, acquired, requiredFinal,
                                                  frontend.Capabilities().support.deviceIncarnation, projectionDepth, plan);
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                auto builder = std::move(created).Value();
                std::vector<RenderGraphImportedState> initial;
                initial.reserve(MaximumReads + 1);
                initial.push_back({depth, {}, RenderGraphInitialContents::Discarded});
                reads.reserve(MaximumReads);
                geometry.reserve(meshes.size());
                if (auto imported = PrepareReads(builder, initial, meshes, resources, slots, motionSlots, acquired.queue);
                    imported.HasError())
                    return imported;
                auto graph = builder.Finalize();
                if (graph.HasError())
                    return Result<void>::Failure(graph.ErrorValue());
                const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, acquired.queue}};
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

        /** @brief Both cold graph shapes reference the same owned projection-depth allocation. */
        Result<void> Prepare(RenderFrontend &frontend, std::span<const EditorViewportMeshResourceView> meshes,
                             const EditorViewportResources &resources, std::span<const DrawSlot> slots,
                             const RenderTextureDescriptor &colorDescriptor, const RenderGraphLogicalState &acquired,
                             const RenderGraphLogicalState &requiredFinal, const bool sampled,
                             const EditorVulkanXrViewportAccess::AuxiliaryPlan *plan, std::span<const DrawSlot> motionSlots) {
            if (auto ready = PrepareDepth(frontend, colorDescriptor, sampled); ready.HasError())
                return ready;
            if (auto compiled = ordinary.Prepare(frontend, meshes, resources, slots, colorDescriptor, acquired, requiredFinal,
                                                 depthTexture.handle, nullptr, {});
                compiled.HasError())
                return compiled;
            if (!plan)
                return Result<void>::Success();
            return paired.emplace().Prepare(frontend, meshes, resources, slots, colorDescriptor, acquired, requiredFinal,
                                            depthTexture.handle, plan, motionSlots);
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
        bool historyEnabled{};
        std::size_t activeHistorySlot{};

        struct DepthPublication {
            MultiviewMotionFrame frame;
            std::size_t slot{};
        };

        std::optional<DepthPublication> previousDepth;
        FramebufferExtent extent;
        RenderTimelinePoint observed;

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
        std::array<MultiviewGraphGeometry, RenderMaterialPass::MaximumDraws> draws{};
        std::array<MultiviewGraphTarget, 2> targets{};
        std::size_t drawCount{};

        struct Auxiliary final {
            EditorVulkanXrViewportAccess::AuxiliaryPlan plan;
            std::shared_ptr<SnapshotStorage> payload{std::make_shared<SnapshotStorage>()};
            std::shared_ptr<const MultiviewSceneSnapshot> scene;
            std::array<MultiviewGraphGeometry, RenderMaterialPass::MaximumDraws> draws{};
            std::array<MultiviewGraphTarget, 2> targets{};

            /** @brief Preserves exact scene and cameras while selecting the separately signed material generation. */
            Result<MultiviewSceneSnapshot> Extract(const MultiviewSceneSnapshot &projection, const MultiviewMotionHistory &history,
                                                   const MultiviewMotionFrame &frame, std::span<const DrawSlot> slots,
                                                   const MultiviewCapabilities capabilities) const {
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
            void Publish(MultiviewSceneSnapshot &&snapshot, const Projection::Topology::Auxiliary &topology,
                         const EditorVulkanXrViewportAccess::AuxiliaryImages &images, std::span<const MultiviewGraphGeometry> projection,
                         std::span<const DrawSlot> slots) {
                payload->value.emplace(std::move(snapshot));
                scene = std::shared_ptr<const MultiviewSceneSnapshot>{payload, &*payload->value};
                for (std::size_t index = 0; index < projection.size(); ++index) {
                    const auto &draw = projection[index];
                    const auto instance = draw.instanceIndex;
                    draws[index] = {instance, draw.vertices, draw.indices, topology.parameters[instance], slots[instance].material};
                }
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

        std::optional<Auxiliary> auxiliary;
        const CompiledRenderGraphExecution *bound{};
        EditorVulkanXrMirrorProjection mirror;
        EditorVulkanXrProjectionAlphaState alpha;
        RenderTimelinePoint submitted;
        MultiviewMotionHistory motion;
        std::optional<MultiviewMotionFrame> motionFrame;
        bool motionSubmitted{};

        /** @brief Stores fixed scene/draw values only after canonical Bind proved the previous queue lease retired. */
        void PublishScene(const Projection &projection, const Projection::Topology &topology, MultiviewSceneSnapshot &&extracted,
                          const std::array<RenderTextureViewHandle, 2> &eyeViews, std::span<const DrawSlot> slots,
                          const std::array<const Projection::Geometry *, RenderMaterialPass::MaximumDraws> &meshes) {
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
                                 {0, 0, extent.width, extent.height},
                                 view,
                                 view};
        }
    };

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
                                                              const EditorVulkanXrViewportAccess::AuxiliaryPlan *auxiliary) {
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
        const auto prepare = [&](XrGraph::Projection &target, const bool sampled) {
            return target.Prepare(frontend_, meshes, resources_, xrDrawSlots_[0], color, acquired, requiredFinal, sampled, auxiliary,
                                  xrMotionDrawSlots_);
        };
        if (auxiliary) {
            for (auto &slot : storage.history)
                if (auto prepared = prepare(slot, true); prepared.HasError())
                    return prepared;
            storage.auxiliary.emplace(*auxiliary);
            storage.historyEnabled = true;
            return Result<void>::Success();
        }
        if (retainPreviousDepth && xrMotionShader_.pipeline.IsValid() && xrMotionShader_.materials) {
            auto &first = storage.history.front();
            if (prepare(first, true).HasValue()) {
                auto &second = storage.history.back();
                storage.historyEnabled = prepare(second, true).HasValue();
                if (!storage.historyEnabled) {
                    storage.single = &first;
                    (void)second.RetireDepth(frontend_);  // Any unsuccessful release remains owned for Scene retirement.
                }
                return Result<void>::Success();
            }
            (void)first.RetireDepth(frontend_);
        }
        return prepare(storage.baseline, false);
    }

    /** @copydoc EditorViewportRendererVulkan::BindXrFrame */
    Result<void> EditorViewportRendererVulkan::BindXrFrame(
        RenderTextureHandle color, const std::array<RenderTextureViewHandle, 2> &eyeViews, const MultiviewPublication &publication,
        std::span<const MultiviewSceneView> views, std::span<const RenderStaticMeshInstance> instances, const MultiviewMotionFrame *motion,
        std::span<const RenderMotionObjectId> motionObjects, const EditorXrProjectionAlpha alpha,
        const EditorVulkanXrViewportAccess::AuxiliaryImages *auxiliary) {
        if (owner_ == std::this_thread::get_id() && xrGraph_) {
            xrGraph_->bound = nullptr;
            xrGraph_->active = nullptr;
            xrGraph_->mirror.ClearAcquisition();
            xrGraph_->alpha.Reset();
            xrGraph_->motionFrame.reset();
            xrGraph_->motionSubmitted = false;
        }
        if (owner_ != std::this_thread::get_id() || !initialized_ || !xrGraph_ || !publication.IsValid() || views.size() != 2 ||
            instances.size() > RenderMaterialPass::MaximumDraws ||
            (alpha != EditorXrProjectionAlpha::Opaque && alpha != EditorXrProjectionAlpha::Premultiplied))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        auto &storage = *xrGraph_;
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
        auto &projection = storage.SelectOutput();
        if (auxiliary && (!projection.paired || !storage.auxiliary))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        auto &topology = auxiliary ? *projection.paired : projection.ordinary;
        std::array<const XrGraph::Projection::Geometry *, RenderMaterialPass::MaximumDraws> meshes{};
        auto extracted = topology.ExtractScene(publication, views, instances, frontend_, xrDrawSlots_[0], meshes);
        if (extracted.HasError())
            return Result<void>::Failure(extracted.ErrorValue());
        const bool staged = motion && storage.motion.Stage(*motion, extracted.Value(), motionObjects);
        std::optional<MultiviewSceneSnapshot> auxiliaryScene;
        if (auxiliary) {
            if (!staged || !storage.previousDepth || storage.motion.PreviousFrame(*motion) != storage.previousDepth->frame)
                return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
            auto motionScene = storage.auxiliary->Extract(extracted.Value(), storage.motion, *motion, xrMotionDrawSlots_,
                                                          ResolveMultiviewCapabilities(frontend_.Capabilities().support));
            if (motionScene.HasError())
                return Result<void>::Failure(motionScene.ErrorValue());
            auxiliaryScene.emplace(std::move(motionScene).Value());
        }
        auto bound = topology.BindReads(frontend_, color, extracted.Value(), meshes, auxiliary);
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
        storage.PublishScene(projection, topology, std::move(extracted).Value(), eyeViews, xrDrawSlots_[0], meshes);
        if (auxiliary)
            storage.auxiliary->Publish(std::move(*auxiliaryScene), *topology.auxiliary, *auxiliary,
                                       std::span{storage.draws}.first(storage.drawCount), xrMotionDrawSlots_);
        storage.bound = bound.Value();
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
        return {.textureId = xrGraph_->mirror.LogicalIdentity()};
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
        for (const auto &draw : graph.auxiliary->scene->Draws()) {
            const auto instance = draw.instanceIndex;
            const auto packed =
                PackMultiviewMotionShaderParameters(xrMotionShader_.layout, graph.motion, *graph.motionFrame, instance, scratch);
            if (packed.HasError())
                return Result<void>::Failure(packed.ErrorValue());
            if (packed.Value() != xrMotionShader_.parameterBytes)
                return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
            auto uploaded = bridge_.Port()->WriteFrameBuffer(frame, xrMotionDrawSlots_[instance].instance,
                                                             std::span{scratch}.first(xrMotionShader_.uniformBytes));
            if (uploaded.HasError())
                return uploaded;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::ExecuteXrFrame */
    Result<void> EditorViewportRendererVulkan::ExecuteXrFrame(RenderFrameScope &frame, const PrimaryOutputAttachment &output) {
        const auto fail = [&frame](const Error &error) {
            frame.Cancel();
            return Result<void>::Failure(error);
        };
        if (owner_ != std::this_thread::get_id() || !initialized_ || !xrGraph_ || !xrGraph_->bound || !bridge_.Port())
            return fail(MakeError(RendererErrors::ViewportNotInitialized));
        auto &storage = *xrGraph_;
        const auto *bound = std::exchange(storage.bound, nullptr);
        if (auto prepared = bridge_.Port()->PreparePrimaryOutput(frame.Token(), output); prepared.HasError())
            return fail(prepared.ErrorValue());
        std::array<std::byte, MaximumUniformBytes> scratch{};
        constexpr std::array<std::uint32_t, 2> singleLayerRouting{0, 0};
        for (const auto &draw : storage.scene->Draws()) {
            const auto instance = draw.instanceIndex;
            const auto packed =
                PackMultiviewShaderParameters(xrShader_.layout, storage.scene->Views(), storage.scene->Instances()[instance].localToWorld,
                                              singleLayerRouting, scratch);
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
        if (paired)
            if (auto written = WriteXrMotionDraws(frame.Token(), storage); written.HasError())
                return fail(written.ErrorValue());
        auto reserved = frame.ReserveSubmission(RenderQueueRole::Graphics);
        if (reserved.HasError())
            return fail(reserved.ErrorValue());
        const RenderGraphQueueSubmission submission{reserved.Value(), {0, bound->Passes().size()}};
        RenderGraphMultiview projection{storage.scene, storage.scene->Publication(), storage.targets,
                                        std::span{storage.draws}.first(storage.drawCount)};
        // The immutable packaged XR program writes alpha one for covered pixels. With blending disabled,
        // clearing uncovered pixels to zero RGBA gives the compositor a premultiplied image.
        if (storage.alpha.IsPremultiplied())
            projection.clearColor = {0.0F, 0.0F, 0.0F, 0.0F};
        std::array<RenderGraphPassWorkload, 2> workloads{{{storage.active->pass, projection}}};
        if (paired) {
            const auto &auxiliary = *storage.auxiliary;
            workloads[1] = {storage.active->auxiliary->pass,
                            RenderGraphMultiview{.scene = auxiliary.scene,
                                                 .active = storage.scene->Publication(),
                                                 .targets = auxiliary.targets,
                                                 .geometry = std::span{auxiliary.draws}.first(storage.drawCount),
                                                 .clearColor = {0.0F, 0.0F, 0.0F, 0.0F},
                                                 .output = MultiviewOutput::MotionDepth}};
        }
        recordedEver_ = true;
        auto executed = frame.ExecuteSubmittedGraph(*bound, std::span{workloads}.first(bound->Passes().size()), std::span{&submission, 1});
        if (executed.HasError())
            frame.Cancel();
        else {
            storage.submitted = reserved.Value().signal;
            storage.alpha.Submit(storage.submitted);
            storage.motionSubmitted = storage.motionFrame.has_value();
        }
        return executed;
    }

    /** @copydoc EditorViewportRendererVulkan::CompleteXrMotion */
    void EditorViewportRendererVulkan::CompleteXrMotion(const std::optional<MultiviewMotionFrame> &frame, const bool projected) noexcept {
        if (owner_ != std::this_thread::get_id() || !xrGraph_)
            return;
        auto &storage = *xrGraph_;
        storage.alpha.Reset();
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
        for (auto *projection : {&xrGraph_->baseline, &xrGraph_->history.front(), &xrGraph_->history.back()})
            if (auto retired = projection->RetireDepth(frontend_); retired.HasError())
                return retired;
        xrGraph_->mirror.Retire();
        xrGraph_.reset();
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::ReleaseXrGraph */
    void EditorViewportRendererVulkan::ReleaseXrGraph() noexcept {
        if (!xrGraph_)
            return;
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
