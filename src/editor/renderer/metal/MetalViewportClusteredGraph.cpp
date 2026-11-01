#include "Horo/Runtime/Render/RenderGraphSynchronization.h"
#include "MetalViewportClusteredFrame.h"
#include "RenderFrontendResourceAccess.h"

#include <algorithm>

namespace Horo::Editor {
    using namespace Render;

    struct MetalViewportGraphAuthor final {
        MetalViewportGraphAuthor(RenderGraphBuilder &source, const RenderQueueId graphics) : builder(source), queue(graphics) {}

        RenderGraphBuilder &builder;
        RenderQueueId queue;
        std::vector<std::pair<RenderBufferHandle, RenderGraphResourceId>> buffers;
        std::vector<RenderGraphImportedState> initial;
        std::optional<Error> error;
        RenderGraphResourceId color, depth, atlas;
        std::array<RenderGraphResourceId, 4> lightBuffers;
        std::array<RenderBufferHandle, 4> bufferHandles;
        std::array<RenderGraphPassRef, MetalViewportClusteredFrame::MaximumPasses> passes;

        template <typename T> T Take(Result<T> result) {
            if (result.HasError()) {
                if (!error)
                    error = result.ErrorValue();
                return {};
            }
            return std::move(result).Value();
        }

        void Take(Result<void> result) {
            if (result.HasError() && !error)
                error = result.ErrorValue();
        }

        RenderGraphResourceId Buffer(const RenderBufferHandle handle, const RenderGraphSynchronizationOperation operation) {
            const auto found = std::ranges::find(buffers, handle, &std::pair<RenderBufferHandle, RenderGraphResourceId>::first);
            if (found != buffers.end())
                return found->second;
            const auto id = Take(builder.ImportBuffer(handle, RenderGraphResourceClass::Persistent));
            buffers.emplace_back(handle, id);
            const auto scope = operation == RenderGraphSynchronizationOperation::GraphicsInputBuffer ? RenderGraphPipelineScope::Graphics
                                                                                                     : RenderGraphPipelineScope::External;
            initial.push_back({id,
                               {RenderGraphSynchronizationAccess::Read, operation, scope, RenderGraphTextureLayout::NotApplicable, queue},
                               RenderGraphInitialContents::Preserved});
            return id;
        }

        RenderGraphResourceId Texture(const RenderTextureHandle handle) {
            const auto id = Take(builder.ImportTexture(handle, RenderGraphResourceClass::Persistent));
            initial.push_back({id, {}, RenderGraphInitialContents::Discarded});
            return id;
        }

        void Use(const RenderGraphPassRef pass, const RenderGraphResourceId resource, const RenderGraphAccess access,
                 const RenderGraphUsageKind kind) {
            Take(builder.AddUsage({pass, resource, access, kind}));
        }

        void AuthorPassesAndCulling() {
            for (std::size_t index = 0; index < this->lightBuffers.size(); ++index)
                this->lightBuffers[index] = Buffer(this->bufferHandles[index], RenderGraphSynchronizationOperation::Storage);
            auto &passes = this->passes;
            for (std::size_t index = 0; index < passes.size(); ++index) {
                passes[index] =
                    Take(builder.AddPass(index == 1 ? RenderPassKind::Compute : RenderPassKind::Graphics, RenderQueueRole::Graphics));
                if (index != 0)
                    Take(builder.AddDependency({passes[index - 1], passes[index], RenderGraphDependencyKind::ExecutionOrder}));
            }
            const auto culling = passes[1];
            for (std::size_t index = 0; index < this->lightBuffers.size(); ++index)
                Use(culling, this->lightBuffers[index], index < 2 ? RenderGraphAccess::Read : RenderGraphAccess::Write,
                    RenderGraphUsageKind::Storage);
        }
    };

    namespace {
        void AddAttachmentUses(MetalViewportGraphAuthor &author, const RenderGraphPassRef pass, const RenderGraphMaterialRaster &raster) {
            if (!raster.depthOnly)
                author.Use(pass, raster.color,
                           raster.colorOperations.loadOperation == AttachmentLoadOperation::Clear ? RenderGraphAccess::Write
                                                                                                  : RenderGraphAccess::ReadWrite,
                           RenderGraphUsageKind::ColorAttachment);
            author.Use(pass, raster.depth, raster.depthClear ? RenderGraphAccess::Write : RenderGraphAccess::ReadWrite,
                       RenderGraphUsageKind::DepthStencilAttachment);
        }

        void AddUniqueUse(MetalViewportGraphAuthor &author, const RenderGraphPassRef pass, const RenderGraphResourceId resource,
                          const RenderGraphUsageKind kind, std::vector<RenderGraphResourceId> &used) {
            if (std::ranges::find(used, resource) != used.end())
                return;
            used.push_back(resource);
            author.Use(pass, resource, RenderGraphAccess::Read, kind);
        }

        RenderGraphMaterialRaster CreateViewportRaster(const EditorViewportResources &resources, const MetalViewportGraphAuthor &author,
                                                       const std::size_t pass, const StaticMeshPassDescriptor &descriptor) {
            RenderGraphMaterialRaster raster{.depth = author.depth,
                                             .depthView = resources.DepthTextureView(),
                                             .extent = descriptor.extent,
                                             .colorOperations = {.loadOperation = AttachmentLoadOperation::Load,
                                                                 .clearColor = descriptor.clearColor},
                                             .depthClear = std::nullopt,
                                             .depthOnly = true};
            if (pass == 0 || pass == 1)
                raster.depthClear = 1;
            if (pass >= 1 && pass <= 4) {
                const auto extent = resources.ShadowExtent();
                raster.depth = author.atlas;
                raster.depthView = resources.ShadowTextureView();
                raster.extent = {extent.width, extent.height};
            }
            if (pass >= 5) {
                raster.depthOnly = false;
                raster.color = author.color;
                raster.colorView = resources.ColorTextureView();
            }
            if (pass == 5)
                raster.colorOperations.loadOperation = AttachmentLoadOperation::Clear;
            return raster;
        }

        Result<CompiledRenderGraphExecution> CompileViewportGraph(MetalViewportGraphAuthor &author,
                                                                  const std::span<const RenderQueueAssignment> queues) {
            using Outcome = Result<CompiledRenderGraphExecution>;
            if (author.error)
                return Outcome::Failure(*author.error);
            const auto graph = author.builder.Finalize();
            if (graph.HasError())
                return Outcome::Failure(graph.ErrorValue());
            const auto schedule = CompileRenderGraph(graph.Value());
            if (schedule.HasError())
                return Outcome::Failure(schedule.ErrorValue());
            const auto synchronization = SynthesizeRenderGraphSynchronization(graph.Value(), schedule.Value(), queues, author.initial);
            if (synchronization.HasError())
                return Outcome::Failure(synchronization.ErrorValue());
            return CompileRenderGraphExecution(graph.Value(), schedule.Value(), synchronization.Value(), queues);
        }
    }  // namespace

    Result<void> MetalViewportClusteredFrame::BuildGraph(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor) {
        if (const auto ready = PrepareGraphMaterials(slot, descriptor.scene); ready.HasError())
            return ready;
        for (std::uint32_t cascades = 0; cascades < slot.variants.size(); ++cascades)
            if (const auto built = BuildVariant(slot, slot.variants[cascades], cascades, descriptor); built.HasError())
                return built;
        slot.meshCount = descriptor.scene.instances.size();
        slot.color = resources_->ColorTexture();
        slot.depth = resources_->DepthTexture();
        slot.atlas = resources_->ShadowDepthTexture();
        slot.atlasGeneration = resources_->ShadowGeneration();
        for (std::size_t draw = 0; draw < slot.meshCount; ++draw)
            slot.meshes[draw] = resources_->FindMesh(descriptor.scene.instances[draw].mesh.id.value)->mesh;
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredFrame::PrepareGraphMaterials(Slot &slot, const RenderSceneView &scene) {
        if (const auto prepared = PrepareUniforms(slot); prepared.HasError())
            return prepared;
        if (const auto overlays = PrepareOverlays(slot); overlays.HasError())
            return overlays;
        for (std::size_t pass = 0; pass < 6; ++pass)
            for (std::size_t draw = 0; draw < scene.instances.size(); ++draw)
                if (const auto prepared = PrepareMaterial(slot, pass, draw); prepared.HasError())
                    return prepared;
        for (std::size_t draw = 0; draw < slot.overlays.size(); ++draw)
            if (const auto material = PrepareMaterial(slot, 6, draw); material.HasError())
                return material;
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredFrame::BuildVariant(Slot &slot, Slot::Variant &variant, const std::uint32_t cascadeCount,
                                                           const Render::StaticMeshPassDescriptor &descriptor) {
        using namespace Render;
        const auto queues = Detail::RenderFrontendResourceAccess::QueueAssignments(*frontend_);
        if (queues.HasError())
            return Result<void>::Failure(queues.ErrorValue());
        const auto graphics = std::ranges::find(queues.Value(), RenderQueueRole::Graphics, &RenderQueueAssignment::role);
        if (graphics == queues.Value().end())
            return Result<void>::Failure(MakeError(LightCullingErrors::Unsupported));
        auto created = RenderGraphBuilder::Create(
            {.maxPasses = MaximumPasses, .maxResources = 300, .maxUsages = 2500, .maxDependencies = MaximumPasses});
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        auto builder = std::move(created).Value();
        MetalViewportGraphAuthor author{builder, graphics->queue};
        author.color = author.Texture(resources_->ColorTexture());
        author.depth = author.Texture(resources_->DepthTexture());
        author.atlas = author.Texture(resources_->ShadowDepthTexture());
        author.bufferHandles = {slot.uploaded.buffers.lights, slot.uploaded.buffers.clusters, slot.uploaded.buffers.membership,
                                slot.uploaded.buffers.references};
        auto &lightBuffers = author.lightBuffers;
        author.AuthorPassesAndCulling();
        const auto &passes = author.passes;
        const auto culling = passes[1];
        variant.workloads[1] = {culling, RenderGraphLightCulling{lightBuffers[0], lightBuffers[1], lightBuffers[2], lightBuffers[3],
                                                                 slot.uploaded.dispatch, slot.uploaded.revision, programs_->Kernel()}};
        for (std::size_t pass = 0; pass < variant.draws.size(); ++pass)
            if (auto raster = BuildRaster(slot, variant, author, pass, cascadeCount, descriptor); raster.HasError())
                return raster;
        variant.workloads[8] = {passes[8], PrimaryOutputAttachment{}};
        auto graph = CompileViewportGraph(author, queues.Value());
        if (graph.HasError())
            return Result<void>::Failure(graph.ErrorValue());
        variant.graph.emplace(std::move(graph).Value());
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredFrame::BuildRaster(Slot &slot, Slot::Variant &variant, MetalViewportGraphAuthor &author,
                                                          const std::size_t pass, const std::uint32_t cascadeCount,
                                                          const Render::StaticMeshPassDescriptor &descriptor) {
        auto raster = CreateViewportRaster(*resources_, author, pass, descriptor);
        const bool isShadow = pass >= 1 && pass <= 4;
        const auto graphIndex = pass == 0 ? 0 : pass + 1;
        AddAttachmentUses(author, author.passes[graphIndex], raster);
        if (pass == 6) {
            if (auto overlay = BuildOverlayRaster(slot, variant, author, raster); overlay.HasError())
                return overlay;
        }
        if (pass < 6 && (!isShadow || pass <= cascadeCount)) {
            if (auto draws = BuildRasterDraws(slot, variant, author, pass, raster, descriptor.scene); draws.HasError())
                return draws;
        }
        variant.workloads[graphIndex] = {author.passes[graphIndex], raster};
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredFrame::BuildRasterDraws(Slot &slot, Slot::Variant &variant, MetalViewportGraphAuthor &author,
                                                               const std::size_t pass, RenderGraphMaterialRaster &raster,
                                                               const RenderSceneView &scene) {
        if (scene.instances.empty())
            return Result<void>::Success();
        const auto graphPass = author.passes[pass == 0 ? 0 : pass + 1];
        std::vector<RenderGraphResourceId> used;
        std::size_t readCount{};
        const auto uniform = slot.uniformBuffers[pass == 5 ? 0 : 2];
        const auto id = author.Buffer(uniform, RenderGraphSynchronizationOperation::GraphicsInputBuffer);
        AddUniqueUse(author, graphPass, id, RenderGraphUsageKind::UniformBuffer, used);
        variant.reads[pass][readCount++] = {id, uniform, {}};
        if (pass == 5) {
            author.Use(graphPass, author.atlas, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled);
            variant.reads[pass][readCount++] = {author.atlas, {}, resources_->ShadowTextureView()};
            for (const auto index : {0U, 2U, 3U}) {
                author.Use(graphPass, author.lightBuffers[index], RenderGraphAccess::Read, RenderGraphUsageKind::Storage);
                variant.reads[pass][readCount++] = {author.lightBuffers[index], author.bufferHandles[index], {}};
            }
        }
        for (std::size_t draw = 0; draw < scene.instances.size(); ++draw) {
            const auto mesh = resources_->FindMesh(scene.instances[draw].mesh.id.value);
            if (!mesh)
                return Result<void>::Failure(MakeError(LightCullingErrors::Pending));
            const auto vertices = author.Buffer(mesh->descriptor.vertexBuffer, RenderGraphSynchronizationOperation::GraphicsInputBuffer);
            const auto indices = author.Buffer(mesh->descriptor.indexBuffer, RenderGraphSynchronizationOperation::GraphicsInputBuffer);
            AddUniqueUse(author, graphPass, vertices, RenderGraphUsageKind::VertexBuffer, used);
            AddUniqueUse(author, graphPass, indices, RenderGraphUsageKind::IndexBuffer, used);
            variant.draws[pass][draw] = {mesh->mesh, vertices, indices, slot.uniforms[pass * MaximumDraws + draw].material};
        }
        raster.draws = std::span{variant.draws[pass]}.first(scene.instances.size());
        raster.reads = std::span{variant.reads[pass]}.first(readCount);
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredFrame::BuildOverlayRaster(Slot &slot, Slot::Variant &variant, MetalViewportGraphAuthor &author,
                                                                 RenderGraphMaterialRaster &raster) {
        const auto pass = author.passes[7];
        const auto uniform = author.Buffer(slot.uniformBuffers[1], RenderGraphSynchronizationOperation::GraphicsInputBuffer);
        author.Use(pass, uniform, RenderGraphAccess::Read, RenderGraphUsageKind::UniformBuffer);
        variant.reads[6][0] = {uniform, slot.uniformBuffers[1], {}};
        for (std::size_t draw = 0; draw < slot.overlays.size(); ++draw) {
            const auto &overlay = slot.overlays[draw];
            const auto vertices = author.Buffer(overlay.descriptor.vertexBuffer, RenderGraphSynchronizationOperation::GraphicsInputBuffer);
            const auto indices = author.Buffer(overlay.descriptor.indexBuffer, RenderGraphSynchronizationOperation::GraphicsInputBuffer);
            author.Use(pass, vertices, RenderGraphAccess::Read, RenderGraphUsageKind::VertexBuffer);
            author.Use(pass, indices, RenderGraphAccess::Read, RenderGraphUsageKind::IndexBuffer);
            variant.draws[6][draw] = {overlay.mesh, vertices, indices, slot.uniforms[6 * MaximumDraws + draw].material};
        }
        raster.draws = std::span{variant.draws[6]}.first(slot.overlays.size());
        raster.reads = std::span{variant.reads[6]}.first(1);
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
