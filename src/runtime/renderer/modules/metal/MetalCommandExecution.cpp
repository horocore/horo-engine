#include "MetalCommandExecution.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

namespace Horo::Render::Detail {
    /** @copydoc ValidateMetalRenderGraph */
    Result<void> ValidateMetalRenderGraph(const IMetalRuntime &runtime, const RenderGraphExecutionRequest &request) {
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
        for (const auto &resource : graph.Resources()) {
            if (resource.resourceClass == RenderGraphResourceClass::Transient && !request.transientResourcesAdmitted)
                return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution,
                                                       "Prepare transient resources through the frontend before execution."));
        }
        for (std::size_t index = 0; index < graph.Passes().size(); ++index) {
            const auto &pass = graph.Passes()[index];
            if (pass.kind == RenderPassKind::Compute && !std::holds_alternative<RenderGraphLightCulling>(request.workloads[index].workload))
                return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedPassKind));
            if (pass.queue != graph.Passes().front().queue)
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::UnsupportedGraphExecution, "Metal graph execution admits exactly one effective queue."));
        }
        if (!request.graph.ExternalReleases().empty())
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
        if (const auto valid = ValidateRenderGraphExecutionRequest(request); valid.HasError())
            return Result<void>::Failure(
                MakeError(MetalBackendErrors::InvalidExecutionPlan, "Graph operation, bindings or declared resource uses are invalid."));
        for (const auto &binding : request.workloads) {
            if (const auto *vfx = std::get_if<RenderGraphVfx>(&binding.workload)) {
                if (const auto valid = runtime.ValidateVfxWorkload(*vfx, request); valid.HasError())
                    return valid;
                continue;
            }
            if (std::holds_alternative<RenderGraphMultiview>(binding.workload))
                return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution));
            if (const auto valid = runtime.ValidateGraphWorkload(binding.workload, request.resources); valid.HasError()) {
                return valid;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc ExecuteMetalRenderGraph */
    Result<void> ExecuteMetalRenderGraph(IMetalRuntime &runtime, const RenderGraphExecutionRequest &request) {
        if (const auto valid = ValidateMetalRenderGraph(runtime, request); valid.HasError()) {
            return valid;
        }
        for (const auto &binding : request.workloads) {
            if (const auto *vfx = std::get_if<RenderGraphVfx>(&binding.workload)) {
                if (const auto encoded = runtime.ExecuteVfxWorkload(*vfx, request); encoded.HasError()) {
                    runtime.AbortFrame();
                    return encoded;
                }
                continue;
            }
            if (const auto encoded = runtime.ExecuteGraphWorkload(binding.workload, request.resources); encoded.HasError()) {
                runtime.AbortFrame();
                return encoded;
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
