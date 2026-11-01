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
        auto pending = std::move(builder).Value();
        const auto pass = pending.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
        if (pass.HasError())
            return Result<void>::Failure(pass.ErrorValue());
        const auto color = pending.ImportTexture(resources_.ColorTexture(), RenderGraphResourceClass::Persistent);
        if (color.HasError())
            return Result<void>::Failure(color.ErrorValue());
        if (const auto used =
                pending.AddUsage({pass.Value(), color.Value(), RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
            used.HasError())
            return used;
        const auto graph = pending.Finalize();
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

    Result<std::uint32_t> EditorViewportRendererVulkan::PrepareViewportSlot(Render::FrameToken frame,
                                                                            const Render::StaticMeshPassDescriptor &pass) {
        using namespace Render;
        using Output = Result<std::uint32_t>;
        if (!pass.IsValid() || !IsReady() || pass.target != resources_.Target() ||
            pass.extent != FramebufferExtent{resources_.AllocatedExtent().width, resources_.AllocatedExtent().height})
            return Output::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        const auto slot = bridge_.Port()->FrameSlot(frame);
        if (slot.HasError())
            return Output::Failure(slot.ErrorValue());
        if (slot.Value().count != device_.frames || slot.Value().index >= device_.frames ||
            pass.scene.instances.size() > preparedDraws_.size())
            return Output::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        return Output::Success(slot.Value().index);
    }

    Result<void> EditorViewportRendererVulkan::PrepareShadowFrame(Render::FrameToken frame, std::uint32_t slot,
                                                                  const Render::StaticMeshPassDescriptor &pass) {
        auto shadows = Render::BuildDirectionalShadowFrame(pass.scene, static_cast<float>(pass.extent.width) / pass.extent.height,
                                                           Math::ClipDepthRange::ZeroToOne, resources_.ActiveDirectionalShadowProfile());
        if (shadows.HasError())
            return Result<void>::Failure(shadows.ErrorValue());
        preparedShadow_ = shadows.Value();
        if (!preparedShadow_)
            return Result<void>::Success();
        return PrepareShadowDraws(frame, slot, pass.scene, *preparedShadow_);
    }

    Result<std::size_t> EditorViewportRendererVulkan::PrepareViewportOverlay(Render::FrameToken frame, std::uint32_t slot,
                                                                             const Render::StaticMeshPassDescriptor &pass) {
        const auto overlay = PrepareOverlay(frame, slots_[slot].overlay, pass);
        if (overlay.HasError())
            return Result<std::size_t>::Failure(overlay.ErrorValue());
        const auto count = pass.scene.instances.size() + (overlay.Value() ? 1 : 0);
        if (count > preparedDraws_.size())
            return Result<std::size_t>::Failure(
                MakeError(RendererErrors::ViewportInvalidScene, "Viewport mesh and overlay draws exceed material capacity."));
        if (overlay.Value())
            preparedDraws_[pass.scene.instances.size()] = {slots_[slot].overlay.mesh, slots_[slot].overlay.uniforms.material};
        return Result<std::size_t>::Success(count);
    }

    Result<void> EditorViewportRendererVulkan::PrepareViewportDraws(Render::FrameToken frame, std::uint32_t slot,
                                                                    const Render::StaticMeshPassDescriptor &pass) {
        const auto *shadow = preparedShadow_ ? &*preparedShadow_ : nullptr;
        for (std::size_t index = 0; index < pass.scene.instances.size(); ++index) {
            const auto &instance = pass.scene.instances[index];
            const auto mesh = resources_.FindMesh(instance.mesh.id.value);
            if (!mesh)
                return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleMeshResource));
            const auto &draw = slots_[slot].draws[index];
            if (const auto written =
                    ProgramResources::WriteDraw(bridge_, shader_, resources_.AllocatedExtent(), frame, draw, instance, pass.scene, shadow);
                written.HasError())
                return written;
            preparedDraws_[index] = {mesh->mesh, draw.material};
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::ExecuteViewport */
    Result<void> EditorViewportRendererVulkan::ExecuteViewport(Render::RenderFrameScope &frame,
                                                               const Render::StaticMeshPassDescriptor &pass) {
        const auto slot = PrepareViewportSlot(frame.Token(), pass);
        if (slot.HasError())
            return Result<void>::Failure(slot.ErrorValue());
        if (auto shadows = PrepareShadowFrame(frame.Token(), slot.Value(), pass); shadows.HasError())
            return shadows;
        const auto count = PrepareViewportOverlay(frame.Token(), slot.Value(), pass);
        if (count.HasError())
            return Result<void>::Failure(count.ErrorValue());
        if (count.Value() == 0)
            return ExecuteEmptyViewport(frame, pass.clearColor);
        if (auto draws = PrepareViewportDraws(frame.Token(), slot.Value(), pass); draws.HasError())
            return draws;
        return ExecuteShadowedViewport(frame, pass, count.Value(), preparedShadow_ ? &*preparedShadow_ : nullptr);
    }
}  // namespace Horo::Editor
