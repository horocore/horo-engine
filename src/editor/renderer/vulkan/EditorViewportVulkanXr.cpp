#include "EditorViewportRendererVulkan.h"
#include "Horo/Runtime/Render/MultiviewShaderParameters.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderGraphTemplate.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "host/EditorVulkanXrMirrorProjection.h"
#include "host/EditorVulkanXrViewportAccess.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Horo::Editor {
    using namespace Render;

    /** @copydoc EditorVulkanXrViewportAccess::Device */
    Result<Detail::VulkanEditorDevice> EditorVulkanXrViewportAccess::Device(const EditorViewportRendererVulkan &viewport) {
        return viewport.BorrowXrDevice();
    }

    /** @copydoc EditorVulkanXrViewportAccess::Prepare */
    Result<void> EditorVulkanXrViewportAccess::Prepare(EditorViewportRendererVulkan &viewport,
                                                       std::span<const RenderMeshResourceView> meshes, const RenderTextureDescriptor &color,
                                                       const RenderGraphLogicalState &acquired, const RenderGraphLogicalState &final) {
        return viewport.PrepareXrGraph(meshes, color, acquired, final);
    }

    /** @copydoc EditorVulkanXrViewportAccess::Bind */
    Result<void> EditorVulkanXrViewportAccess::Bind(EditorViewportRendererVulkan &viewport, RenderTextureHandle color,
                                                    const std::array<RenderTextureViewHandle, 2> &views,
                                                    const MultiviewPublication &publication, std::span<const MultiviewSceneView> cameras,
                                                    std::span<const RenderStaticMeshInstance> instances) {
        return viewport.BindXrFrame(color, views, publication, cameras, instances);
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
    void EditorVulkanXrViewportAccess::RevokeMirror(EditorViewportRendererVulkan &viewport) noexcept {
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

    /** @brief Cold scene topology and exact immutable resident superset, never another scene or submission owner. */
    struct EditorViewportRendererVulkan::XrGraph final {
        std::optional<Detail::PreparedRenderGraphTemplate> topology;
        Detail::RenderGraphExternalTextureSlot color;
        RenderGraphResourceId depth;
        RenderGraphPassRef pass;
        RenderTextureHandle depthTexture;
        std::array<RenderTextureViewHandle, 2> depthViews{};
        FramebufferExtent extent;

        struct Geometry final {
            std::uint64_t source{};
            RenderMeshSourceHandle sourceGeneration;
            RenderMeshHandle mesh;
            RenderGraphResourceId vertices, indices;
        };

        std::vector<Geometry> geometry;
        std::array<RenderGraphResourceId, RenderMaterialPass::MaximumDraws> parameters{};
        std::vector<Detail::RenderGraphOptionalRead> reads;

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
        const CompiledRenderGraphExecution *bound{};
        EditorVulkanXrMirrorProjection mirror;
        RenderTimelinePoint submitted;
    };

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

    /** @copydoc EditorViewportRendererVulkan::PrepareXrGraph */
    Result<void> EditorViewportRendererVulkan::PrepareXrGraph(std::span<const EditorViewportMeshResourceView> meshes,
                                                              const RenderTextureDescriptor &color, const RenderGraphLogicalState &acquired,
                                                              const RenderGraphLogicalState &requiredFinal) {
        if (owner_ != std::this_thread::get_id() || !initialized_ || !xrShader_.pipeline.IsValid() || xrGraph_ ||
            meshes.size() > RenderMaterialPass::MaximumDraws || !color.IsValid() || color.dimension != RenderTextureDimension::TwoD ||
            color.format != RenderTextureFormat::Rgba8Unorm || color.layerCount != 2 || color.mipCount != 1 || color.sampleCount != 1 ||
            color.usage != (RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled) ||
            acquired.queue != device_.graphicsIdentity || requiredFinal.queue != device_.graphicsIdentity ||
            acquired.layout != RenderGraphTextureLayout::ColorAttachment ||
            requiredFinal.layout != RenderGraphTextureLayout::ColorAttachment)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        if (auto prepared = resources_.PrepareMeshes(meshes); prepared.HasError())
            return prepared;
        if (auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        const auto support = ResolveMultiviewCapabilities(frontend_.Capabilities().support);
        if (!support.multipass || !support.indexedShaders || support.maximumViews < 2)
            return Result<void>::Failure(
                MakeError(RendererErrors::ViewportInvalidScene, "The selected renderer has no admitted two-view indexed shader path."));

        // PERF: All graph vectors, optional-read scratch, native depth storage and views are cold allocations.
        // Partial preparation remains in this owner; no normal acquisition compiles or grows this storage.
        xrGraph_ = std::make_shared<XrGraph>();
        auto &storage = *xrGraph_;
        storage.extent = color.extent;
        auto createdDepth = frontend_.CreateTexture(RenderMemoryScopes::ViewportResources, {.extent = color.extent,
                                                                                            .format = RenderTextureFormat::Depth32Float,
                                                                                            .layerCount = 2,
                                                                                            .usage = RenderTextureUsage::RenderAttachment});
        if (createdDepth.HasError())
            return Result<void>::Failure(createdDepth.ErrorValue());
        storage.depthTexture = createdDepth.Value().handle;
        if (auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        if (auto ready = frontend_.ResourceOperationResult(createdDepth.Value().operation); ready.HasError())
            return ready;
        for (std::uint32_t layer = 0; layer < storage.depthViews.size(); ++layer) {
            auto view = frontend_.CreateTextureView({.texture = storage.depthTexture,
                                                     .format = RenderTextureFormat::Depth32Float,
                                                     .aspect = RenderTextureAspect::Depth,
                                                     .baseLayer = layer,
                                                     .layerCount = 1});
            if (view.HasError())
                return Result<void>::Failure(view.ErrorValue());
            storage.depthViews[layer] = view.Value().handle;
            if (auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
                return Result<void>::Failure(processed.ErrorValue());
            if (auto ready = frontend_.ResourceOperationResult(view.Value().operation); ready.HasError())
                return ready;
        }
        constexpr auto maximumReads = RenderMaterialPass::MaximumDraws * 3;
        auto created = RenderGraphBuilder::Create(
            {.maxPasses = 1, .maxResources = maximumReads + 2, .maxUsages = maximumReads + 2, .maxDependencies = 1});
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        auto builder = std::move(created).Value();
        auto pass = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
        if (pass.HasError())
            return Result<void>::Failure(pass.ErrorValue());
        storage.pass = pass.Value();
        auto external = Detail::PreparedRenderGraphTemplate::DeclareTexture(builder, color, acquired, requiredFinal,
                                                                            frontend_.Capabilities().support.deviceIncarnation);
        if (external.HasError())
            return Result<void>::Failure(external.ErrorValue());
        storage.color = external.Value();
        auto depth = builder.ImportTexture(storage.depthTexture, RenderGraphResourceClass::Persistent);
        if (depth.HasError())
            return Result<void>::Failure(depth.ErrorValue());
        storage.depth = depth.Value();
        if (auto use =
                builder.AddUsage({storage.pass, storage.color.resource, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
            use.HasError())
            return use;
        if (auto use =
                builder.AddUsage({storage.pass, storage.depth, RenderGraphAccess::Write, RenderGraphUsageKind::DepthStencilAttachment});
            use.HasError())
            return use;
        std::vector<RenderGraphImportedState> initial;
        initial.reserve(maximumReads + 1);
        initial.push_back({storage.depth, {}, RenderGraphInitialContents::Discarded});
        storage.reads.reserve(maximumReads);
        storage.geometry.reserve(meshes.size());

        struct Input final {
            RenderBufferHandle handle;
            RenderGraphResourceId resource;
            RenderGraphUsageKind role;
        };

        std::vector<Input> inputs;
        inputs.reserve(maximumReads);
        const auto input = [&](RenderBufferHandle handle, RenderGraphUsageKind role) -> Result<RenderGraphResourceId> {
            for (const auto &existing : inputs)
                if (existing.handle == handle)
                    return existing.role == role ? Result<RenderGraphResourceId>::Success(existing.resource)
                                                 : Result<RenderGraphResourceId>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
            auto imported = builder.ImportBuffer(handle, RenderGraphResourceClass::Persistent);
            if (imported.HasError())
                return imported;
            if (auto use = builder.AddUsage({storage.pass, imported.Value(), RenderGraphAccess::Read, role}); use.HasError())
                return Result<RenderGraphResourceId>::Failure(use.ErrorValue());
            initial.push_back({imported.Value(),
                               {RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::NotApplicable, device_.graphicsIdentity}});
            inputs.push_back({handle, imported.Value(), role});
            storage.reads.push_back({imported.Value(), role});
            return imported;
        };
        for (const auto &source : meshes) {
            const auto mesh = resources_.FindMesh(source.handle.id.value);
            if (!mesh)
                return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleMeshResource));
            auto vertices = input(mesh->descriptor.vertexBuffer, RenderGraphUsageKind::VertexBuffer);
            if (vertices.HasError())
                return Result<void>::Failure(vertices.ErrorValue());
            auto indices = input(mesh->descriptor.indexBuffer, RenderGraphUsageKind::IndexBuffer);
            if (indices.HasError())
                return Result<void>::Failure(indices.ErrorValue());
            storage.geometry.push_back({source.handle.id.value, source.handle, mesh->mesh, vertices.Value(), indices.Value()});
        }
        std::ranges::sort(storage.geometry, {}, &XrGraph::Geometry::source);
        for (std::size_t draw = 0; draw < storage.parameters.size(); ++draw) {
            auto parameters = input(xrDrawSlots_[0][draw].buffer, RenderGraphUsageKind::UniformBuffer);
            if (parameters.HasError())
                return Result<void>::Failure(parameters.ErrorValue());
            storage.parameters[draw] = parameters.Value();
        }
        auto graph = builder.Finalize();
        if (graph.HasError())
            return Result<void>::Failure(graph.ErrorValue());
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, device_.graphicsIdentity}};
        auto prepared = Detail::PreparedRenderGraphTemplate::Prepare(std::move(graph).Value(), std::array{storage.color}, initial, queues,
                                                                     1, storage.reads);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        storage.topology.emplace(std::move(prepared).Value());
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::BindXrFrame */
    Result<void> EditorViewportRendererVulkan::BindXrFrame(RenderTextureHandle color,
                                                           const std::array<RenderTextureViewHandle, 2> &eyeViews,
                                                           const MultiviewPublication &publication,
                                                           std::span<const MultiviewSceneView> views,
                                                           std::span<const RenderStaticMeshInstance> instances) {
        if (owner_ == std::this_thread::get_id() && xrGraph_) {
            xrGraph_->bound = nullptr;
            xrGraph_->mirror.ClearAcquisition();
        }
        if (owner_ != std::this_thread::get_id() || !initialized_ || !xrGraph_ || !xrGraph_->topology || !publication.IsValid() ||
            views.size() != 2 || instances.size() > RenderMaterialPass::MaximumDraws)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        auto &storage = *xrGraph_;
        const auto &support = frontend_.Capabilities().support;
        if (support.deviceIncarnation != publication.deviceIncarnation || support.capabilityRevision != publication.capabilityRevision)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));

        // Temporary fixed values do not touch a potentially queued slot. Only successful canonical Bind
        // below proves the sole instance has retired, permitting mutation of its retained payload storage.
        std::array<MultiviewSceneInstance, RenderMaterialPass::MaximumDraws> values{};
        std::array<const XrGraph::Geometry *, RenderMaterialPass::MaximumDraws> meshes{};
        for (std::size_t index = 0; index < instances.size(); ++index) {
            const auto &source = instances[index];
            auto mesh = std::ranges::lower_bound(storage.geometry, source.mesh.id.value, {}, &XrGraph::Geometry::source);
            if (!source.IsValid() || source.material != CoreDefaultMaterial || mesh == storage.geometry.end() ||
                mesh->sourceGeneration != source.mesh || !xrDrawSlots_[0][index].material)
                return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleMeshResource));
            meshes[index] = &*mesh;
            values[index] = {mesh->mesh, xrDrawSlots_[0][index].material->Id(), source.localToWorld, source.localBounds};
        }
        auto extracted = MultiviewSceneSnapshot::Extract(publication, publication, views, std::span{values}.first(instances.size()),
                                                         ResolveMultiviewCapabilities(support), MultiviewExecutionPolicy::Multipass);
        if (extracted.HasError())
            return Result<void>::Failure(extracted.ErrorValue());
        constexpr std::size_t maximumReads = RenderMaterialPass::MaximumDraws * 3;
        std::array<Detail::RenderGraphOptionalRead, maximumReads> reads{};
        std::array<bool, maximumReads + 2> selected{};
        std::size_t readCount{};
        const auto select = [&](RenderGraphResourceId resource, RenderGraphUsageKind role) {
            // Every ID comes from this immutable cold superset, not from the caller's scene.
            const auto index = resource.value - 1;
            if (!selected[index]) {
                selected[index] = true;
                reads[readCount++] = {resource, role};
            }
        };
        for (const auto &draw : extracted.Value().Draws()) {
            const auto index = draw.instanceIndex;
            select(meshes[index]->vertices, RenderGraphUsageKind::VertexBuffer);
            select(meshes[index]->indices, RenderGraphUsageKind::IndexBuffer);
            select(storage.parameters[index], RenderGraphUsageKind::UniformBuffer);
        }
        const std::array bindings{Detail::RenderGraphExternalTextureBinding{storage.color.resource, color}};
        auto bound = storage.topology->Bind(frontend_, 0, bindings, std::span{reads}.first(readCount));
        if (bound.HasError())
            return Result<void>::Failure(bound.ErrorValue());
        storage.bound = nullptr;
        for (auto view : eyeViews) {
            const auto state = frontend_.ResourceState(view);
            if (state.HasError())
                return Result<void>::Failure(state.ErrorValue());
            if (state.Value() != RenderResourceState::Ready)
                return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        }
        storage.payload->value.emplace(std::move(extracted).Value());
        storage.scene = std::shared_ptr<const MultiviewSceneSnapshot>{storage.payload, &*storage.payload->value};
        storage.drawCount = storage.scene->Draws().size();
        for (std::size_t draw = 0; draw < storage.drawCount; ++draw) {
            const auto instance = storage.scene->Draws()[draw].instanceIndex;
            storage.draws[draw] = {instance, meshes[instance]->vertices, meshes[instance]->indices, storage.parameters[instance],
                                   xrDrawSlots_[0][instance].material};
        }
        for (std::uint32_t view = 0; view < storage.targets.size(); ++view)
            storage.targets[view] = {storage.color.resource,
                                     storage.depth,
                                     eyeViews[view],
                                     storage.depthViews[view],
                                     {0, 0, storage.extent.width, storage.extent.height},
                                     view,
                                     view};
        storage.bound = bound.Value();
        if (!storage.mirror.Bind(publication.frame, eyeViews[0])) {
            storage.bound = nullptr;
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        }
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
    void EditorViewportRendererVulkan::RevokeXrMirror() noexcept {
        if (owner_ == std::this_thread::get_id() && xrGraph_)
            xrGraph_->mirror.Revoke();
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
            if (packed.Value() != xrShader_.uniformBytes)
                return fail(MakeError(RendererErrors::ViewportInvalidScene));
            auto uploaded = bridge_.Port()->WriteFrameBuffer(frame.Token(), xrDrawSlots_[0][instance].instance,
                                                             std::span{scratch}.first(packed.Value()));
            if (uploaded.HasError())
                return fail(uploaded.ErrorValue());
        }
        auto reserved = frame.ReserveSubmission(RenderQueueRole::Graphics);
        if (reserved.HasError())
            return fail(reserved.ErrorValue());
        const RenderGraphQueueSubmission submission{reserved.Value(), {0, 1}};
        const std::array workloads{
            RenderGraphPassWorkload{storage.pass, RenderGraphMultiview{storage.scene, storage.scene->Publication(), storage.targets,
                                                                       std::span{storage.draws}.first(storage.drawCount)}}};
        recordedEver_ = true;
        auto executed = frame.ExecuteSubmittedGraph(*bound, workloads, std::span{&submission, 1});
        if (executed.HasError())
            frame.Cancel();
        else
            storage.submitted = reserved.Value().signal;
        return executed;
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
        return observed.HasError() ? Result<void>::Failure(observed.ErrorValue()) : Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::RetireXrGraph */
    Result<void> EditorViewportRendererVulkan::RetireXrGraph() {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        if (!xrGraph_)
            return Result<void>::Success();
        for (auto &view : xrGraph_->depthViews) {
            if (!view.IsValid())
                continue;
            auto released = frontend_.ReleaseTextureView(view);
            if (released.HasError())
                return released;
            view = {};
        }
        if (xrGraph_->depthTexture.IsValid()) {
            auto released = frontend_.ReleaseTexture(xrGraph_->depthTexture);
            if (released.HasError())
                return released;
            xrGraph_->depthTexture = {};
        }
        xrGraph_->mirror.Retire();
        xrGraph_.reset();
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::ReleaseXrGraph */
    void EditorViewportRendererVulkan::ReleaseXrGraph() noexcept {
        if (!xrGraph_)
            return;
        for (auto view : xrGraph_->depthViews)
            if (view.IsValid())
                static_cast<void>(frontend_.ReleaseTextureView(view));
        if (xrGraph_->depthTexture.IsValid())
            static_cast<void>(frontend_.ReleaseTexture(xrGraph_->depthTexture));
        xrGraph_.reset();
    }
}  // namespace Horo::Editor
