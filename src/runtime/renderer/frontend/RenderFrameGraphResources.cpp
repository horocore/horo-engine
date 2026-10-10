#include "RenderFrameGraphResources.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "RenderGraphResourceLeasePool.h"
#include "RenderGraphTransientResourcePool.h"
#include "RenderResourceRegistry.h"

namespace Horo::Render::Detail {
    /** @copydoc ResolveFrameGraphResources */
    Result<ResolvedFrameGraphResources> ResolveFrameGraphResources(const CompiledRenderGraphExecution &graph,
                                                                   const RenderResourceRegistry &registry,
                                                                   const RenderGraphTransientResourceSet *transient) {
        using ResolveResult = Result<ResolvedFrameGraphResources>;
        if (!graph.Owner().IsValid() || graph.Resources().size() > RenderGraphLimits::HardMaxResources)
            return ResolveResult::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        ResolvedFrameGraphResources resolved;
        for (const RenderGraphResource &resource : graph.Resources()) {
            const auto *binding = &resource.binding;
            if (resource.resourceClass == RenderGraphResourceClass::Transient) {
                if (transient == nullptr || resource.id.value == 0 || resource.id.value > transient->bindings.size())
                    return ResolveResult::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
                const auto index = resource.id.value - 1U;
                if (transient->lifetimes[index].disposition == RenderGraphLifetimeDisposition::Unused) {
                    resolved.instances[resolved.count++] = {resource.id, 0};
                    continue;
                }
                binding = &transient->bindings[index];
            }
            const auto identity = ResolveGraphResidentIdentity(*binding);
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
