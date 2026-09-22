#include "Horo/Extensions/ExtensionErrors.h"

namespace Horo::Extensions::ExtensionErrors {
    namespace {
        const ErrorDomainId ScriptBoundaryDomain{"horo.extensions"};
    }

    const ErrorCodeDescriptor ScriptValueInvalid{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_value_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script value or structured error is malformed.",
        .remediationHint = "Use only the declared scalar, collection, enum, struct, handle, and nullable value forms.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ScriptValueCapacityExceeded{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_value_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script value or codec operation exceeded a finite host bound.",
        .remediationHint = "Reduce value depth, collection size, string/byte payload, or codec work.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ScriptValueEncodingInvalid{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_value_encoding_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script value wire payload is malformed or incomplete.",
        .remediationHint = "Use the host-owned canonical script value codec and provide one complete payload.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ScriptValueTypeMismatch{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_value_type_mismatch"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script value does not match its declared language-neutral type.",
        .remediationHint = "Pass the exact declared primitive, enum, struct, array, map, or nullable value.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ScriptCallResultInvalid{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_call_result_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script call result or structured error is malformed.",
        .remediationHint = "Publish exactly one success or failure result with bounded declared payloads.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ScriptInvocationInvalid{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_invocation_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script invocation request or lifecycle transition is malformed.",
        .remediationHint = "Use a validated descriptor generation, declared function, bounded values, and finite policy.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ScriptInvocationThreadViolation{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_invocation_thread_violation"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script invocation crossed its declared owner-thread or safe-point boundary.",
        .remediationHint = "Submit and drain from the context owner thread; keep provider work behind the host ABI.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ScriptInvocationReentrant{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_invocation_reentrant"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script invocation attempted a forbidden recursive lifecycle transition.",
        .remediationHint = "Return to the declared safe point before submitting or delivering the nested call.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ScriptInvocationUnavailable{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_invocation_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested script invocation authority is unavailable or revoked.",
        .remediationHint = "Refresh the provider/context generation and recreate the binding.",
        .retryable = true,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ScriptInvocationProviderDuplicate{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_invocation_provider_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script invocation provider generation is already registered.",
        .remediationHint = "Register one exact provider generation and revoke it before replacement.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ScriptInvocationCapacityExceeded{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_invocation_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script invocation registry or its context/provider bound is full.",
        .remediationHint = "Drain completions, finish existing calls, or use an explicit larger host bound.",
        .retryable = true,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ScriptInvocationShutdown{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_invocation_shutdown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script invocation registry is shutting down.",
        .remediationHint = "Stop submitting work and recreate the binding after the host lifecycle restarts.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ScriptInvocationBackpressure{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_invocation_backpressure"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The script invocation completion queue applied its bounded backpressure policy.",
        .remediationHint = "Drain the context safe-point queue; progress may be coalesced but terminal completion remains reserved.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ScriptInvocationCancelled{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_invocation_cancelled"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The script invocation was cooperatively cancelled.",
        .remediationHint = "Observe the cancellation source and stop provider work at a bounded point.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ScriptInvocationTimeout{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_invocation_timeout"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The script invocation exceeded its finite timeout.",
        .remediationHint = "Complete work within the declared timeout or use an explicit larger policy bound.",
        .retryable = true,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ScriptInvocationProviderRevoked{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_invocation_provider_revoked"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The script provider generation was revoked before invocation completion.",
        .remediationHint = "Rebind against the current provider generation after reload or shutdown.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ScriptInvocationContextRevoked{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_invocation_context_revoked"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The script context was revoked before completion delivery.",
        .remediationHint = "Destroy runtime bindings before context teardown and do not retain pending VM objects.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ScriptInvocationAbandoned{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_invocation_abandoned"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script invocation producer was destroyed before publishing a terminal result.",
        .remediationHint = "Keep the host-owned controller alive until provider work publishes completion or failure.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ScriptHandleInvalid{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_handle_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The script opaque handle is malformed or owned by another context/generation.",
        .remediationHint = "Use the exact host-issued handle without changing its identity fields.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ScriptHandleRevoked{
        .domain = ScriptBoundaryDomain,
        .code = ErrorCode{"script_handle_revoked"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The script opaque handle was revoked with its provider or context generation.",
        .remediationHint = "Acquire a fresh handle from the current live binding.",
        .retryable = true,
        .userActionable = true,
    };
}  // namespace Horo::Extensions::ExtensionErrors
