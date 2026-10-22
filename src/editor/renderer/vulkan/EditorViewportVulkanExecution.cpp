#include "EditorViewportRendererVulkan.h"
#include "editor/renderer/EditorRendererErrors.h"

#include <algorithm>

namespace Horo::Editor {
    /** @copydoc EditorViewportRendererVulkan::CreateEmptyGraph */
    Result<void> EditorViewportRendererVulkan::CreateEmptyGraph() {
        using namespace Render;
        auto builder = RenderGraphBuilder::Create({.maxPasses = 1, .maxResources = 1, .maxUsages = 1, .maxDependencies = 1});
        if (builder.HasError())
            return Result<void>::Failure(builder.ErrorValue());
        const auto pass = builder.Value().AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
        if (pass.HasError())
            return Result<void>::Failure(pass.ErrorValue());
        const auto color = builder.Value().ImportTexture(resources_.ColorTexture(), RenderGraphResourceClass::Persistent);
        if (color.HasError())
            return Result<void>::Failure(color.ErrorValue());
        if (const auto used =
                builder.Value().AddUsage({pass.Value(), color.Value(), RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
            used.HasError())
            return used;
        const auto graph = builder.Value().Finalize();
        if (graph.HasError())
            return Result<void>::Failure(graph.ErrorValue());
        const auto schedule = CompileRenderGraph(graph.Value());
        if (schedule.HasError())
            return Result<void>::Failure(schedule.ErrorValue());
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, device_.graphicsIdentity}};
        const std::array initial{RenderGraphImportedState{color.Value(), {}, RenderGraphInitialContents::Discarded}};
        const auto synchronization = SynthesizeRenderGraphSynchronization(graph.Value(), schedule.Value(), queues, initial);
        if (synchronization.HasError())
            return Result<void>::Failure(synchronization.ErrorValue());
        auto compiled = CompileRenderGraphExecution(graph.Value(), schedule.Value(), synchronization.Value(), queues);
        if (compiled.HasError())
            return Result<void>::Failure(compiled.ErrorValue());
        const std::array workloads{RenderGraphPassWorkload{pass.Value(), RenderGraphColorAttachment{color.Value(), {}}}};
        emptyGraph_.emplace(EmptyGraph{std::move(compiled).Value(), workloads});
        emptyGraphTexture_ = resources_.ColorTexture();
        return Result<void>::Success();
    }

    /** @copydoc IEditorViewportRenderer::ExecutePreparedFrame */
    Result<void> EditorViewportRendererVulkan::ExecutePreparedFrame(Render::RenderFrameScope &frame,
                                                                    const std::span<const Render::RenderPassDescriptor> passes) {
        using namespace Render;
        const auto fail = [&frame](const Error &error) {
            frame.Cancel();
            return Result<void>::Failure(error);
        };
        if (owner_ != std::this_thread::get_id() || !initialized_ || !bridge_.Port() || !frame.Token().IsValid() || passes.size() > 2)
            return fail(MakeError(RendererErrors::ViewportNotInitialized));
        const StaticMeshPassDescriptor *viewport{};
        const PrimaryOutputAttachment *output{};
        for (const auto &pass : passes) {
            if (pass.kind != RenderPassKind::Graphics || (!pass.staticMesh && !pass.primaryOutput) ||
                (pass.staticMesh && pass.primaryOutput))
                return fail(MakeError(RendererErrors::ViewportInvalidScene));
            if (pass.staticMesh) {
                if (viewport)
                    return fail(MakeError(RendererErrors::ViewportInvalidScene));
                viewport = &*pass.staticMesh;
            }
            if (pass.primaryOutput) {
                if (output)
                    return fail(MakeError(RendererErrors::ViewportInvalidScene));
                output = &*pass.primaryOutput;
            }
        }
        if (!output)
            return fail(MakeError(RendererErrors::ViewportInvalidScene));
        if (const auto prepared = bridge_.Port()->PreparePrimaryOutput(frame.Token(), *output); prepared.HasError())
            return fail(prepared.ErrorValue());
        if (!viewport)
            return frame.Execute({});
        const auto executed = ExecuteViewport(frame, *viewport);
        if (executed.HasError())
            frame.Cancel();
        return executed;
    }

    /** @copydoc EditorViewportRendererVulkan::ExecuteEmptyViewport */
    Result<void> EditorViewportRendererVulkan::ExecuteEmptyViewport(Render::RenderFrameScope &frame, const Render::ClearColor clearColor) {
        using namespace Render;
        if (!emptyGraph_)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        std::get<RenderGraphColorAttachment>(emptyGraph_->workloads[0].workload).operations.clearColor = clearColor;
        const auto reserved = frame.ReserveSubmission(RenderQueueRole::Graphics);
        if (reserved.HasError())
            return Result<void>::Failure(reserved.ErrorValue());
        const RenderGraphQueueSubmission submission{reserved.Value(), {0, 1}};
        recordedEver_ = true;
        return frame.ExecuteSubmittedGraph(emptyGraph_->graph, emptyGraph_->workloads, std::span{&submission, 1});
    }

    /** @copydoc EditorViewportRendererVulkan::ExecuteViewport */
    Result<void> EditorViewportRendererVulkan::ExecuteViewport(Render::RenderFrameScope &frame,
                                                               const Render::StaticMeshPassDescriptor &pass) {
        using namespace Render;
        if (!pass.IsValid() || !IsReady() || pass.target != resources_.Target() ||
            pass.extent != FramebufferExtent{resources_.AllocatedExtent().width, resources_.AllocatedExtent().height})
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        if (requestedGrid_.visible || requestedLightVisualizer_.selectedLight ||
            std::ranges::any_of(pass.scene.lights,
                                [](const auto &light) {
            return light.kind != RenderLightKind::Directional;
        }) ||
            pass.scene.lights.size() > 1)
            return Result<void>::Failure(
                MakeError(RendererErrors::ViewportCapabilityUnavailable,
                          "This unqualified Vulkan viewport profile currently admits static meshes with at most one directional light; "
                          "grid and selected-light overlays require their cooked pipeline."));
        const auto slot = bridge_.Port()->FrameSlot(frame.Token());
        if (slot.HasError())
            return Result<void>::Failure(slot.ErrorValue());
        if (slot.Value().count != device_.frames || slot.Value().index >= device_.frames ||
            pass.scene.instances.size() > preparedDraws_.size())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        if (pass.scene.instances.empty())
            return ExecuteEmptyViewport(frame, pass.clearColor);
        for (std::size_t index = 0; index < pass.scene.instances.size(); ++index) {
            const auto &instance = pass.scene.instances[index];
            const auto mesh = resources_.FindMesh(instance.mesh.id.value);
            if (!mesh)
                return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleMeshResource));
            const auto &draw = slots_[slot.Value().index].draws[index];
            if (const auto written = WriteDraw(frame.Token(), draw, instance, pass.scene); written.HasError())
                return written;
            preparedDraws_[index] = {mesh->mesh, draw.material};
        }
        const auto reserved = frame.ReserveSubmission(RenderQueueRole::Graphics);
        if (reserved.HasError())
            return Result<void>::Failure(reserved.ErrorValue());
        recordedEver_ = true;
        return frame.ExecuteMaterialPass({pass.target,
                                          pass.extent,
                                          {.clearColor = pass.clearColor},
                                          1.0F,
                                          std::span{preparedDraws_}.first(pass.scene.instances.size()),
                                          reserved.Value()});
    }
}  // namespace Horo::Editor
