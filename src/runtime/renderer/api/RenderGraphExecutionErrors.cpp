#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::RenderGraphExecutionErrors {
    namespace {
        const ErrorDomainId Domain{"render.graph.execution"};
    }

    const ErrorCodeDescriptor UnsupportedWorkload =
        Detail::MakeErrorDescriptor(Domain, "render.graph.execution.workload_unsupported", ErrorSeverity::Error,
                                    "The selected backend cannot execute this graph workload or resource class.",
                                    "Use an admitted workload and resolved resident resources; no backend fallback is performed.");
    const ErrorCodeDescriptor VfxExecutionUnavailable =
        Detail::MakeErrorDescriptor(Domain, "render.graph.execution.vfx_unavailable", ErrorSeverity::Error,
                                    "Typed VFX graph scheduling is available, but native particle encoding is not implemented.",
                                    "Provide a qualified particle encoder before submitting VFX workloads; no empty-pass substitution is "
                                    "permitted.");
    const ErrorCodeDescriptor VfxDrawBudgetExceeded =
        Detail::MakeErrorDescriptor(Domain, "render.graph.execution.vfx_draw_budget_exceeded", ErrorSeverity::Error,
                                    "Complete VFX sorted draw runs exceed the remaining global draw-call allowance.",
                                    "Re-admit the scene/view budget or explicitly reduce cosmetic effects at the owner boundary.");
    const ErrorCodeDescriptor VfxSchedulingCapacity =
        Detail::MakeErrorDescriptor(Domain, "render.graph.execution.vfx_scheduling_capacity", ErrorSeverity::Error,
                                    "VFX view scheduling exceeds its prepared renderer capacity.",
                                    "Prepare aggregate view/order storage before activation; frame-hot growth is prohibited.");
    const ErrorCodeDescriptor VfxViewsRetained =
        Detail::MakeErrorDescriptor(Domain, "render.graph.execution.vfx_views_retained", ErrorSeverity::Warning,
                                    "All prepared VFX view slots remain leased by outstanding workloads.",
                                    "Release retired view/graph leases before explicitly retrying admission.");
    const ErrorCodeDescriptor AllocationFailed =
        Detail::MakeErrorDescriptor(Domain, "render.graph.execution.allocation_failed", ErrorSeverity::Error,
                                    "Compiled graph execution storage allocation failed.",
                                    "Reduce graph limits or release host memory before retrying.", true);
    const ErrorCodeDescriptor InvalidGraph =
        Detail::MakeErrorDescriptor(Domain, "render.graph.execution.graph_invalid", ErrorSeverity::Error,
                                    "The finalized graph is moved-from or structurally invalid.",
                                    "Compile execution only from the intact finalized graph returned by its builder.");
    const ErrorCodeDescriptor InvalidQueueTopology =
        Detail::MakeErrorDescriptor(Domain, "render.graph.execution.queue_topology_invalid", ErrorSeverity::Error,
                                    "The effective queue topology is incomplete or ambiguous.",
                                    "Provide exactly one valid assignment for every queue role used by retained passes.");
    const ErrorCodeDescriptor InvalidSchedule =
        Detail::MakeErrorDescriptor(Domain, "render.graph.execution.schedule_invalid", ErrorSeverity::Error,
                                    "The pass schedule is malformed or belongs to another graph.",
                                    "Use the intact schedule compiled from the same finalized graph.");
    const ErrorCodeDescriptor InvalidSynchronization =
        Detail::MakeErrorDescriptor(Domain, "render.graph.execution.synchronization_invalid", ErrorSeverity::Error,
                                    "The synchronization plan is malformed or belongs to another graph.",
                                    "Use the intact synchronization plan synthesized from this graph and schedule.");
}  // namespace Horo::Render::RenderGraphExecutionErrors
