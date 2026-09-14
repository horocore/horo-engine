#include "Horo/Network/NetworkErrors.h"

namespace Horo::Network::NetworkErrors {
    namespace {
        const ErrorDomainId NetworkDomain{"horo.network"};
    }

    const ErrorCodeDescriptor ReplicationSerializerInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.serializer_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Replication serializer metadata or value representation is malformed.",
        .remediationHint = "Use valid typed identities, canonical module ownership, finite bounds, and an explicit quantization policy.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationSerializerConflict{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.serializer_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Replication serializers claim the same stable contract identity.",
        .remediationHint = "Register exactly one serializer for each owner, semantic value type, and codec identity tuple.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationSerializerUnknown{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.serializer_unknown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A replication field has no exact serializer binding.",
        .remediationHint =
            "Contribute the declaring owner's matching value-type and codec adapter before publishing the schema generation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationSerializerCapacityExceeded{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.serializer_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Replication serializer work exceeded a finite declared bound.",
        .remediationHint = "Reduce adapter count, element count, or encoded bytes to the schema and host registry limits.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ReplicationSerializerIncompatible{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.serializer_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Replication value tags are incompatible with the declared field.",
        .remediationHint = "Decode only the exact negotiated semantic value type and codec identity for this schema field.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ReplicationSerializerValueInvalid{
        .domain = NetworkDomain,
        .code = ErrorCode{"network.replication.serializer_value_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A replication runtime value or canonical payload is invalid for its typed codec.",
        .remediationHint = "Provide the declared value kind, finite numbers, canonical byte form, and values within the field bounds.",
        .retryable = false,
        .userActionable = true,
    };
}  // namespace Horo::Network::NetworkErrors
