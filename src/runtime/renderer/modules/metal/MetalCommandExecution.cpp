#include "MetalCommandExecution.h"

namespace Horo::Render::Detail {
    namespace {
        /** @brief Verifies that a concrete operation exactly accounts for its declared resource uses. */
        [[nodiscard]] bool WorkloadMatches(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                           const RenderGraphWorkload &workload) {
            const auto uses = request.graph.Usages().subspan(pass.usages.offset, pass.usages.count);
            if (const auto *color = std::get_if<RenderGraphColorAttachment>(&workload)) {
                return pass.kind == RenderPassKind::Graphics && color->operations.IsValid() &&
                       color->operations.loadOperation != AttachmentLoadOperation::DontCare &&
                       color->operations.storeOperation == AttachmentStoreOperation::Store && uses.size() == 1 &&
                       uses[0].resource == color->texture && uses[0].kind == RenderGraphUsageKind::ColorAttachment &&
                       uses[0].access != RenderGraphAccess::Read &&
                       (color->operations.loadOperation != AttachmentLoadOperation::Load || uses[0].access == RenderGraphAccess::ReadWrite);
            }
            if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&workload)) {
                if (pass.kind != RenderPassKind::Copy || uses.size() != 2 || copy->byteCount == 0 || copy->source == copy->destination) {
                    return false;
                }
                bool source = false;
                bool destination = false;
                for (const auto &use : uses) {
                    source |= use.resource == copy->source && use.kind == RenderGraphUsageKind::CopySource &&
                              use.access == RenderGraphAccess::Read;
                    destination |= use.resource == copy->destination && use.kind == RenderGraphUsageKind::CopyDestination &&
                                   use.access == RenderGraphAccess::Write;
                }
                return source && destination;
            }
            if (const auto *primary = std::get_if<PrimaryOutputAttachment>(&workload)) {
                return pass.kind == RenderPassKind::Graphics && uses.empty() && primary->IsValid();
            }
            return pass.kind == RenderPassKind::Graphics && uses.empty();
        }

        /** @brief Checks the resolved native binding envelope without materializing or publishing resources. */
        [[nodiscard]] Result<void> ValidateResidentBindings(const RenderGraphExecutionRequest &request) {
            for (std::size_t index = 0; index < request.graph.Resources().size(); ++index) {
                const auto &resource = request.graph.Resources()[index];
                const auto &instance = request.resources[index];
                if (resource.resourceClass == RenderGraphResourceClass::Transient)
                    return Result<void>::Failure(
                        MakeError(MetalBackendErrors::UnsupportedGraphExecution,
                                  "Materialize transient resources through the frontend allocation plan before execution."));
                if (instance.resource != resource.id || instance.instance == 0)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
            }
            return Result<void>::Success();
        }

        /** @brief Checks one compiled operation's queue, kind and exact declared resource uses. */
        [[nodiscard]] Result<void> ValidatePassBinding(const RenderGraphExecutionRequest &request, const std::size_t index) {
            const auto &pass = request.graph.Passes()[index];
            const auto &binding = request.workloads[index];
            if (pass.kind == RenderPassKind::Compute)
                return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedPassKind));
            if (pass.queue != request.graph.Passes().front().queue)
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::UnsupportedGraphExecution, "Metal graph execution admits exactly one effective queue."));
            if (!pass.queue.IsValid() || binding.pass != pass.pass || !WorkloadMatches(request, pass, binding.workload))
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::InvalidExecutionPlan,
                              "Graph operation, resource uses, order or effective queue do not match the compiled pass."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateMetalRenderGraph */
    Result<void> ValidateMetalRenderGraph(const IMetalRuntime &runtime, const RenderGraphExecutionRequest &request) {
        const auto &graph = request.graph;
        if (!graph.Owner().IsValid() || request.workloads.size() != graph.Passes().size() ||
            request.resources.size() != graph.Resources().size()) {
            return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
        }
        if (!graph.ReleaseTransfers().empty() || !graph.AcquireTransfers().empty()) {
            return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution,
                                                   "Metal graph ownership transfers require compiled submission timelines."));
        }
        if (const auto valid = ValidateResidentBindings(request); valid.HasError())
            return valid;
        for (std::size_t index = 0; index < request.workloads.size(); ++index) {
            const auto &binding = request.workloads[index];
            if (const auto valid = ValidatePassBinding(request, index); valid.HasError())
                return valid;
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
            if (const auto encoded = runtime.ExecuteGraphWorkload(binding.workload, request.resources); encoded.HasError()) {
                runtime.AbortFrame();
                return encoded;
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
