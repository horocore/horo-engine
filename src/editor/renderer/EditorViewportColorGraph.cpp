#include "EditorViewportColorGraph.h"

#include "EditorViewportColorInternal.h"

#include <algorithm>

namespace Horo::Editor {
    using namespace Render;
    using namespace ViewportColorDetail;

    /** @copydoc EditorViewportColorGraph::Buffer */
    RenderGraphResourceId EditorViewportColorGraph::Buffer(RenderBufferHandle handle) {
        for (std::size_t i = 0; i < bufferCount_; ++i)
            if (buffers_[i].first == handle)
                return buffers_[i].second;
        Require(bufferCount_ < buffers_.size());
        const auto resource = Take(builder_->ImportBuffer(handle, RenderGraphResourceClass::Persistent));
        bufferInitial_[bufferCount_] = pipeline_->BufferInitialState(handle, queue_);
        buffers_[bufferCount_++] = {handle, resource};
        initial_->push_back({resource, bufferInitial_[bufferCount_ - 1], RenderGraphInitialContents::Preserved});
        return resource;
    }

    /** @copydoc EditorViewportColorGraph::Table */
    RenderGraphResourceId EditorViewportColorGraph::Table(EditorViewportColorPipeline &pipeline, std::size_t index) {
        const auto resource = Take(builder_->ImportTexture(pipeline.TableTexture(index), RenderGraphResourceClass::Persistent));
        tableInitial_ = pipeline.TableInitialState(queue_);
        initial_->push_back({resource, tableInitial_, RenderGraphInitialContents::Preserved});
        return resource;
    }

    /** @copydoc EditorViewportColorGraph::Use */
    void EditorViewportColorGraph::Use(RenderGraphPassRef pass, RenderGraphResourceId resource, RenderGraphAccess access,
                                       RenderGraphUsageKind kind) {
        Take(builder_->AddUsage({pass, resource, access, kind}));
    }

    /** @copydoc EditorViewportColorGraph::RasterTexture */
    RenderGraphResourceId EditorViewportColorGraph::RasterTexture(EditorViewportColorPipeline &pipeline, std::size_t role,
                                                                  const MaterialResourceBinding &binding) {
        if (role == 1)
            return display_;
        if (binding.binding.value == 3 || binding.binding.value == 4)
            return Table(pipeline, binding.binding.value - 3);
        if (binding.binding.value == 8)
            return Table(pipeline, 2);
        return scene_;
    }

    /** @copydoc EditorViewportColorGraph::RasterInputs */
    void EditorViewportColorGraph::RasterInputs(EditorViewportColorPipeline &pipeline, std::size_t role, RenderGraphPassRef pass) {
        using enum Horo::Render::RenderGraphAccess;
        using enum Horo::Render::RenderGraphUsageKind;
        const auto geometry = pipeline.GeometryBuffers();
        const auto vertex = Buffer(geometry[0]);
        const auto index = Buffer(geometry[1]);
        Use(pass, vertex, Read, VertexBuffer);
        Use(pass, index, Read, IndexBuffer);
        draws_[role][0] = {pipeline.Mesh(), vertex, index, pipeline.Material(role)};
        std::size_t count{};
        for (const auto &binding : pipeline.Material(role)->Descriptor().resources) {
            if (const auto *buffer = std::get_if<MaterialBufferBinding>(&binding.value)) {
                const bool storage = binding.binding.value == 7;
                const auto id = Buffer(buffer->buffer);
                Use(pass, id, Read, storage ? Storage : UniformBuffer);
                reads_[role][count++] = {id, buffer->buffer, {}};
            }
            if (const auto *view = std::get_if<RenderTextureViewHandle>(&binding.value)) {
                const auto id = RasterTexture(pipeline, role, binding);
                Use(pass, id, Read, Sampled);
                reads_[role][count++] = {id, {}, *view};
            }
        }
        auto &raster = std::get<RenderGraphMaterialRaster>(workloads_[role + 1].workload);
        raster.draws = draws_[role];
        raster.reads = std::span{reads_[role]}.first(count);
    }

    /** @copydoc EditorViewportColorGraph::Raster */
    void EditorViewportColorGraph::Raster(EditorViewportColorPipeline &pipeline, std::size_t role, RenderGraphPassRef pass,
                                          RenderGraphResourceId color, RenderTextureViewHandle view) {
        Use(pass, color, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment);
        workloads_[role + 1] = {pass, RenderGraphMaterialRaster{.color = color,
                                                                .colorView = view,
                                                                .extent = extent_,
                                                                .colorOperations = {.loadOperation = AttachmentLoadOperation::Clear},
                                                                .depthClear = std::nullopt}};
        RasterInputs(pipeline, role, pass);
    }

    /** @copydoc EditorViewportColorGraph::AppendScene */
    Result<RenderGraphPassRef> EditorViewportColorGraph::AppendScene(RenderGraphBuilder &builder, EditorViewportColorPipeline &pipeline,
                                                                     RenderQueueId queue, std::vector<RenderGraphImportedState> &initial,
                                                                     RenderGraphResourceId scene, RenderGraphResourceId display,
                                                                     RenderTextureViewHandle displayView, RenderGraphPassRef after,
                                                                     FramebufferExtent extent) {
        try {
            builder_ = &builder;
            pipeline_ = &pipeline;
            initial_ = &initial;
            queue_ = queue;
            scene_ = scene;
            display_ = display;
            extent_ = extent;
            bufferCount_ = 0;
            const auto meter = Take(builder.AddPass(RenderPassKind::Compute, RenderQueueRole::Graphics));
            const auto buffers = pipeline.MeterBuffers();
            const auto &previous = pipeline.Previous();
            const auto prior = previous.buffer.IsValid() ? previous.buffer : buffers[1];
            RenderGraphExposureGpu work{scene,
                                        Buffer(buffers[0]),
                                        Buffer(prior),
                                        Buffer(buffers[2]),
                                        extent,
                                        pipeline.Dispatch(),
                                        previous,
                                        pipeline.Produced(),
                                        !previous.buffer.IsValid(),
                                        pipeline.Kernel()};
            Use(meter, work.sceneColor, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled);
            Use(meter, work.histogram, RenderGraphAccess::ReadWrite, RenderGraphUsageKind::Storage);
            Use(meter, work.previousExposure, work.resetPrevious ? RenderGraphAccess::ReadWrite : RenderGraphAccess::Read,
                RenderGraphUsageKind::Storage);
            Use(meter, work.outputExposure, RenderGraphAccess::Write, RenderGraphUsageKind::Storage);
            workloads_[0] = {meter, work};
            Take(builder.AddDependency({after, meter, RenderGraphDependencyKind::ExecutionOrder}));
            const auto output = Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
            Take(builder.AddDependency({meter, output, RenderGraphDependencyKind::ExecutionOrder}));
            Raster(pipeline, 0, output, display, displayView);
            return Result<RenderGraphPassRef>::Success(output);
        } catch (const Failure &failure) {
            return Result<RenderGraphPassRef>::Failure(failure.error);
        }
    }

    /** @copydoc EditorViewportColorGraph::AppendEncode */
    Result<RenderGraphPassRef> EditorViewportColorGraph::AppendEncode(RenderGraphBuilder &builder, EditorViewportColorPipeline &pipeline,
                                                                      RenderGraphResourceId output, RenderTextureViewHandle outputView,
                                                                      RenderGraphPassRef after) {
        try {
            const auto pass = Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
            Take(builder.AddDependency({after, pass, RenderGraphDependencyKind::ExecutionOrder}));
            Raster(pipeline, 1, pass, output, outputView);
            return Result<RenderGraphPassRef>::Success(pass);
        } catch (const Failure &failure) {
            return Result<RenderGraphPassRef>::Failure(failure.error);
        }
    }

    /** @copydoc EditorViewportColorGraph::Refresh */
    void EditorViewportColorGraph::Refresh(EditorViewportColorPipeline &pipeline) noexcept {
        using enum Horo::Render::RenderGraphSynchronizationAccess;
        using enum Horo::Render::RenderGraphPipelineScope;
        using enum Horo::Render::RenderGraphTextureLayout;
        auto &work = std::get<RenderGraphExposureGpu>(workloads_[0].workload);
        work.dispatch = pipeline.Dispatch();
        work.previous = pipeline.Previous();
        work.produced = pipeline.Produced();
        work.resetPrevious = !work.previous.buffer.IsValid();
        for (std::size_t role = 0; role < 2; ++role)
            draws_[role][0].material = pipeline.Material(role);
        for (std::size_t i = 0; i < bufferCount_; ++i) {
            auto state =
                RenderGraphLogicalState{Read, RenderGraphSynchronizationOperation::GraphicsInputBuffer, Graphics, NotApplicable, queue_};
            if (buffers_[i].second == work.histogram)
                state = {ReadWrite, RenderGraphSynchronizationOperation::Storage, Compute, NotApplicable, queue_};
            if (buffers_[i].second == work.outputExposure)
                state = {Write, RenderGraphSynchronizationOperation::Storage, Compute, NotApplicable, queue_};
            if (buffers_[i].second == work.previousExposure)
                state = {Read, RenderGraphSynchronizationOperation::Storage, Graphics, NotApplicable, queue_};
            pipeline.StageBufferState(buffers_[i].first, state);
        }
    }

    /** @copydoc EditorViewportColorGraph::MatchesInitialStates */
    bool EditorViewportColorGraph::MatchesInitialStates(const EditorViewportColorPipeline &pipeline) const noexcept {
        if (tableInitial_ != pipeline.TableInitialState(queue_))
            return false;
        for (std::size_t i = 0; i < bufferCount_; ++i)
            if (bufferInitial_[i] != pipeline.BufferInitialState(buffers_[i].first, queue_))
                return false;
        return true;
    }
}  // namespace Horo::Editor
