#include "Horo/Network/NetworkErrors.h"

namespace Horo::Network::NetworkErrors {
    namespace {
        const ErrorDomainId NetworkDomain{"horo.network"};
    }

    const ErrorCodeDescriptor RpcDescriptorInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.rpc.descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "RPC declaration metadata is malformed or unsafe.",
        .remediationHint = "Use a stable identity, a safe direction/target/permission combination, and finite parameter and rate bounds.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor RpcDescriptorConflict{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.rpc.descriptor_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "RPC or parameter identities conflict.",
        .remediationHint = "Allocate each RPC and parameter identity once and preserve retired identities as tombstones.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor RpcDescriptorIncompatible{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.rpc.descriptor_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "RPC declaration evolution is incompatible.",
        .remediationHint = "Preserve accepted routing and parameter semantics or publish an explicitly incompatible major version.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor RpcUnknown{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.rpc.unknown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested RPC declaration is absent.",
        .remediationHint = "Dispatch only an exact identity from the pinned immutable declaration generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor RpcCapacityExceeded{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.rpc.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "RPC declaration construction exceeded its finite capacity.",
        .remediationHint = "Reduce declaration, parameter, owner, payload, or default-data bounds.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor RpcParameterUnsupported{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.rpc.parameter_unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An RPC parameter has no accepted typed codec.",
        .remediationHint = "Register an exact owner, semantic value type, codec, and sufficient finite bounds before activation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor RpcPermissionUnsupported{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.rpc.permission_unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The declared RPC caller policy has no admitted implementation.",
        .remediationHint = "Compose a host-approved caller-policy capability before enabling this RPC.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor RpcPermissionDenied{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.rpc.permission_denied"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The admitted caller is not permitted to invoke this RPC.",
        .remediationHint = "Use the current server-issued direction, role and object ownership grant.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor RpcRateLimited{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.rpc.rate_limited"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The caller or global RPC work budget is exhausted.",
        .remediationHint = "Wait for the host clock budget to refill; never retry an accepted logical command.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor RpcParameterInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.rpc.parameter_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A canonical typed RPC value violates Gameplay's declared constraint.",
        .remediationHint = "Submit finite values inside the owner-composed typed parameter range.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor RpcGameplayFailed{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.rpc.gameplay_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An authorized Gameplay RPC invocation failed.",
        .remediationHint = "Inspect the retained Gameplay cause; the logical command is terminal and must not be replayed.",
        .retryable = false,
        .userActionable = false,
    };
}  // namespace Horo::Network::NetworkErrors
