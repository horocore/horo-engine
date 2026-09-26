#include "Horo/Network/NetworkErrors.h"

namespace Horo::Network::NetworkErrors {
    namespace {
        const ErrorDomainId NetworkDomain{"horo.network"};
    }

    const ErrorCodeDescriptor NetworkTargetManifestInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.target_manifest.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Network target manifest is malformed or conflicts with its declared capabilities.",
        .remediationHint = "Regenerate a supported version-one manifest from the target's actual role, provider and protocol plan.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor NetworkTargetManifestCapacityExceeded{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.target_manifest.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Network target manifest exceeds its finite byte or provider bound.",
        .remediationHint = "Reduce the provider list or manifest size and regenerate the package.",
        .retryable = false,
        .userActionable = true,
    };
}  // namespace Horo::Network::NetworkErrors
