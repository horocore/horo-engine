#include "RenderFrameGraphResources.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "RenderGraphResourceLeasePool.h"
#include "RenderResourceRegistry.h"

namespace Horo::Render::Detail {
    /** @copydoc ResolveFrameGraphResources */
    Result<ResolvedFrameGraphResources> ResolveFrameGraphResources(const CompiledRenderGraphExecution &graph,
                                                                   const RenderResourceRegistry &registry) {
        using ResolveResult = Result<ResolvedFrameGraphResources>;
        if (!graph.Owner().IsValid() || graph.Resources().size() > RenderGraphLimits::HardMaxResources)
            return ResolveResult::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        ResolvedFrameGraphResources resolved;
        for (const RenderGraphResource &resource : graph.Resources()) {
            const auto identity = ResolveGraphResidentIdentity(resource.binding);
            if (identity.HasError())
                return ResolveResult::Failure(identity.ErrorValue());
            const auto instance = registry.BackendInstance(identity.Value().resourceClass, identity.Value().identity);
            if (instance.HasError())
                return ResolveResult::Failure(instance.ErrorValue());
            resolved.instances[resolved.count++] = {resource.id, instance.Value()};
        }
        return ResolveResult::Success(std::move(resolved));
    }
}  // namespace Horo::Render::Detail
