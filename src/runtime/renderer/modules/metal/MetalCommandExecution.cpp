#include "MetalCommandExecution.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

namespace Horo::Render::Detail {
    namespace {
        Result<void> ValidateGraphEnvelope(const RenderGraphExecutionRequest &request) {
            if (!request.submissions.empty())
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::UnsupportedGraphExecution, "Metal explicit graph submissions are not implemented."));
            const auto &graph = request.graph;
            if (!graph.Owner().IsValid() || request.workloads.size() != graph.Passes().size() ||
                request.resources.size() != graph.Resources().size()) {
                return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
            }
            if (!graph.ReleaseTransfers().empty() || !graph.AcquireTransfers().empty()) {
                return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution,
                                                       "Metal graph ownership transfers require compiled submission timelines."));
            }
            if (!request.graph.ExternalReleases().empty())
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
            return Result<void>::Success();
        }

        Result<void> ValidateGraphPasses(const RenderGraphExecutionRequest &request) {
            for (const auto &resource : request.graph.Resources()) {
                if (resource.resourceClass == RenderGraphResourceClass::Transient && !request.transientResourcesAdmitted)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution,
                                                           "Prepare transient resources through the frontend before execution."));
            }
            for (std::size_t index = 0; index < request.graph.Passes().size(); ++index) {
                const auto &pass = request.graph.Passes()[index];
                if (pass.kind == RenderPassKind::Compute &&
                    !std::holds_alternative<RenderGraphLightCulling>(request.workloads[index].workload))
                    return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedPassKind));
                if (pass.queue != request.graph.Passes().front().queue)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution,
                                                           "Metal graph execution admits exactly one effective queue."));
            }
            return Result<void>::Success();
        }

        Result<void> ValidateNativeWorkload(const IMetalRuntime &runtime, const RenderGraphWorkload &workload,
                                            const RenderGraphExecutionRequest &request) {
            if (const auto *raster = std::get_if<RenderGraphMaterialRaster>(&workload))
                return runtime.ValidateMaterialRaster(*raster, request);
            if (const auto *vfx = std::get_if<RenderGraphVfx>(&workload))
                return runtime.ValidateVfxWorkload(*vfx, request);
            if (std::holds_alternative<RenderGraphMultiview>(workload))
                return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution));
            return runtime.ValidateGraphWorkload(workload, request.resources);
        }

        Result<void> ExecuteNativeWorkload(IMetalRuntime &runtime, const RenderGraphWorkload &workload,
                                           const RenderGraphExecutionRequest &request) {
            if (const auto *raster = std::get_if<RenderGraphMaterialRaster>(&workload))
                return runtime.ExecuteMaterialRaster(*raster, request);
            if (const auto *vfx = std::get_if<RenderGraphVfx>(&workload))
                return runtime.ExecuteVfxWorkload(*vfx, request);
            return runtime.ExecuteGraphWorkload(workload, request.resources);
        }
    }  // namespace

    /** @copydoc ValidateMetalRenderGraph */
    Result<void> ValidateMetalRenderGraph(const IMetalRuntime &runtime, const RenderGraphExecutionRequest &request) {
        if (const auto envelope = ValidateGraphEnvelope(request); envelope.HasError())
            return envelope;
        if (const auto passes = ValidateGraphPasses(request); passes.HasError())
            return passes;
        if (const auto valid = ValidateRenderGraphExecutionRequest(request); valid.HasError())
            return Result<void>::Failure(
                MakeError(MetalBackendErrors::InvalidExecutionPlan, "Graph operation, bindings or declared resource uses are invalid."));
        for (const auto &binding : request.workloads)
            if (const auto valid = ValidateNativeWorkload(runtime, binding.workload, request); valid.HasError())
                return valid;
        return Result<void>::Success();
    }

    /** @copydoc ExecuteMetalRenderGraph */
    Result<void> ExecuteMetalRenderGraph(IMetalRuntime &runtime, const RenderGraphExecutionRequest &request) {
        if (const auto valid = ValidateMetalRenderGraph(runtime, request); valid.HasError()) {
            return valid;
        }
        for (const auto &binding : request.workloads) {
            if (const auto encoded = ExecuteNativeWorkload(runtime, binding.workload, request); encoded.HasError()) {
                runtime.AbortFrame();
                return encoded;
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
