#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

#include <array>

namespace Horo::Render {
    namespace {
        /** @brief Requires each operation to account for exactly its retained semantic uses. */
        [[nodiscard]] bool MatchesWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                           const RenderGraphWorkload &workload) {
            using enum RenderGraphAccess;
            const auto uses = request.graph.Usages().subspan(pass.usages.offset, pass.usages.count);
            if (const auto *color = std::get_if<RenderGraphColorAttachment>(&workload)) {
                return pass.kind == RenderPassKind::Graphics && color->operations.IsValid() &&
                       color->operations.loadOperation != AttachmentLoadOperation::DontCare &&
                       color->operations.storeOperation == AttachmentStoreOperation::Store && uses.size() == 1 &&
                       uses[0].resource == color->texture && uses[0].kind == RenderGraphUsageKind::ColorAttachment &&
                       uses[0].access != Read &&
                       (color->operations.loadOperation != AttachmentLoadOperation::Load || uses[0].access == ReadWrite);
            }
            if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&workload)) {
                if (pass.kind != RenderPassKind::Copy || uses.size() != 2 || copy->byteCount == 0 || copy->source == copy->destination)
                    return false;
                bool source = false;
                bool destination = false;
                for (const auto &use : uses) {
                    source |= use.resource == copy->source && use.kind == RenderGraphUsageKind::CopySource && use.access == Read;
                    destination |=
                        use.resource == copy->destination && use.kind == RenderGraphUsageKind::CopyDestination && use.access == Write;
                }
                return source && destination;
            }
            if (const auto *primary = std::get_if<PrimaryOutputAttachment>(&workload))
                return pass.kind == RenderPassKind::Graphics && uses.empty() && primary->IsValid();
            return pass.kind == RenderPassKind::Graphics && uses.empty();
        }
    }  // namespace

    /** @copydoc ValidateRenderGraphExecutionRequest */
    Result<void> ValidateRenderGraphExecutionRequest(const RenderGraphExecutionRequest &request) {
        const auto &graph = request.graph;
        const auto malformed = [] {
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        };
        if (!graph.Owner().IsValid() || graph.Resources().size() > RenderGraphLimits::HardMaxResources ||
            graph.Passes().size() > RenderGraphLimits::HardMaxPasses || request.workloads.size() != graph.Passes().size() ||
            request.resources.size() != graph.Resources().size())
            return malformed();
        if (!graph.ReleaseTransfers().empty() || !graph.AcquireTransfers().empty())
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload,
                                                   "Single-queue workload admission does not implement ownership transfers."));
        std::array<bool, RenderGraphLimits::HardMaxResources> used{};
        for (std::size_t index = 0; index < graph.Passes().size(); ++index) {
            const auto &pass = graph.Passes()[index];
            if (!pass.usages.IsValidFor(graph.Usages().size()) || !pass.queue.IsValid() || pass.queue != graph.Passes().front().queue ||
                request.workloads[index].pass != pass.pass || !MatchesWorkload(request, pass, request.workloads[index].workload))
                return malformed();
            for (const auto &use : graph.Usages().subspan(pass.usages.offset, pass.usages.count)) {
                if (use.resource.owner != graph.Owner() || use.resource.value == 0 || use.resource.value > graph.Resources().size())
                    return malformed();
                used[use.resource.value - 1U] = true;
            }
        }
        for (std::size_t index = 0; index < graph.Resources().size(); ++index) {
            const auto &resource = graph.Resources()[index];
            if (request.resources[index].resource != resource.id)
                return malformed();
            if (resource.resourceClass == RenderGraphResourceClass::Transient) {
                if (!request.transientResourcesAdmitted)
                    return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
                if (!used[index])
                    continue;
            }
            if (request.resources[index].instance == 0)
                return malformed();
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render
