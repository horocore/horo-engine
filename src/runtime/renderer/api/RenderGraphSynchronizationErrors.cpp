#include "Horo/Runtime/Render/RenderGraphSynchronizationErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::RenderGraphSynchronizationErrors {
    namespace {
        const ErrorDomainId Domain{"render.graph.synchronization"};
    }

    const ErrorCodeDescriptor AllocationFailed =
        Detail::MakeErrorDescriptor(Domain, "render.graph.synchronization.allocation_failed", ErrorSeverity::Error,
                                    "Synchronization plan storage allocation failed.",
                                    "Reduce graph limits or release host memory before retrying.", true);
    const ErrorCodeDescriptor DuplicateInitialState =
        Detail::MakeErrorDescriptor(Domain, "render.graph.synchronization.initial_state_duplicate", ErrorSeverity::Error,
                                    "An imported resource has more than one initial state.",
                                    "Provide exactly one initial state for each imported resource generation.");
    const ErrorCodeDescriptor InvalidInitialState =
        Detail::MakeErrorDescriptor(Domain, "render.graph.synchronization.initial_state_invalid", ErrorSeverity::Error,
                                    "An imported resource initial state is malformed or incompatible.",
                                    "Provide a valid state, effective queue, and layout compatible with the imported resource kind.");
    const ErrorCodeDescriptor InvalidExternalRelease =
        Detail::MakeErrorDescriptor(Domain, "render.graph.synchronization.external_release_invalid", ErrorSeverity::Error,
                                    "An external release obligation is malformed, duplicate or has no retained producer.",
                                    "Name each used external resource once with its exact required external state and queue.");
    const ErrorCodeDescriptor InvalidQueueTopology =
        Detail::MakeErrorDescriptor(Domain, "render.graph.synchronization.queue_topology_invalid", ErrorSeverity::Error,
                                    "The effective queue topology is incomplete or ambiguous.",
                                    "Provide exactly one valid assignment for every queue role used by retained passes.");
    const ErrorCodeDescriptor InvalidSchedule =
        Detail::MakeErrorDescriptor(Domain, "render.graph.synchronization.schedule_invalid", ErrorSeverity::Error,
                                    "The schedule is malformed or belongs to another graph.",
                                    "Synthesize only from the intact schedule compiled from this graph.");
    const ErrorCodeDescriptor MissingInitialState =
        Detail::MakeErrorDescriptor(Domain, "render.graph.synchronization.initial_state_missing", ErrorSeverity::Error,
                                    "An imported resource has no exact initial state evidence.",
                                    "Provide generation-scoped initial state evidence for every imported resource used by the schedule.");
    const ErrorCodeDescriptor UnexpectedInitialState =
        Detail::MakeErrorDescriptor(Domain, "render.graph.synchronization.initial_state_unexpected", ErrorSeverity::Error,
                                    "Initial state evidence names a transient or unknown resource.",
                                    "Provide initial state evidence only for resources imported by this graph.");
    const ErrorCodeDescriptor UndefinedRead =
        Detail::MakeErrorDescriptor(Domain, "render.graph.synchronization.undefined_read", ErrorSeverity::Error,
                                    "A synchronization use reads undefined transient contents.",
                                    "Write the whole transient resource in an earlier scheduled pass before reading it.");
    const ErrorCodeDescriptor UnsupportedState =
        Detail::MakeErrorDescriptor(Domain, "render.graph.synchronization.state_unsupported", ErrorSeverity::Error,
                                    "A resource use cannot be represented by the backend-neutral synchronization model.",
                                    "Use a declared access, operation, pass scope, texture layout, and effective queue combination.");
}  // namespace Horo::Render::RenderGraphSynchronizationErrors
