#include "Horo/PlatformServices/PlatformOfflineQueueStorage.h"

namespace Horo::PlatformOfflineQueue::PlatformOfflineQueueErrors {
    namespace {
        const ErrorDomainId Domain{"horo.platform.offline"};

        [[nodiscard]] ErrorCodeDescriptor Descriptor(const std::string_view code, const std::string_view summary,
                                                     const std::string_view remediation, const bool retryable = false) {
            return {.domain = Domain,
                    .code = ErrorCode{std::string{code}},
                    .defaultSeverity = ErrorSeverity::Error,
                    .summary = summary,
                    .remediationHint = remediation,
                    .retryable = retryable,
                    .userActionable = false};
        }
    }  // namespace

    const ErrorCodeDescriptor InvalidConfiguration =
        Descriptor("platform.offline.invalid_configuration", "Offline queue storage configuration is invalid.",
                   "Use finite nonzero bounds and an explicit product state root.");
    const ErrorCodeDescriptor InvalidPartition =
        Descriptor("platform.offline.invalid_partition", "Offline queue partition identity is invalid.",
                   "Use the protected nonzero same-binding partition assigned by the identity owner.");
    const ErrorCodeDescriptor InvalidRecord =
        Descriptor("platform.offline.invalid_record", "Offline queue record is malformed.",
                   "Provide a complete provider-neutral record with a valid lifecycle and operation.");
    const ErrorCodeDescriptor UnsupportedVersion =
        Descriptor("platform.offline.version_unsupported", "Offline queue schema version is unsupported.",
                   "Migrate the queue document before opening it.");
    const ErrorCodeDescriptor PayloadTooLarge =
        Descriptor("platform.offline.payload_too_large", "Offline queue payload exceeds its bounded limit.",
                   "Reduce the canonical intent payload before durable admission.");
    const ErrorCodeDescriptor CapacityExceeded = Descriptor("platform.offline.capacity_exceeded", "Offline queue capacity is exhausted.",
                                                            "Retire or compact legal terminal records before admitting more work.", true);
    const ErrorCodeDescriptor Corrupt = Descriptor("platform.offline.corrupt", "Offline queue storage is corrupt or truncated.",
                                                   "Quarantine and recover the last valid document; do not recreate it as an empty queue.");
    const ErrorCodeDescriptor DurableUnavailable =
        Descriptor("platform.offline.durable_unavailable", "Offline queue storage is unavailable.",
                   "Retry after filesystem access, permission or capacity is restored.", true);
    const ErrorCodeDescriptor StorageUnknown =
        Descriptor("platform.offline.storage_unknown", "Offline queue publication outcome is unknown.",
                   "Reconcile the published document before retrying durable admission.");
    const ErrorCodeDescriptor IdentityConflict =
        Descriptor("platform.offline.identity_conflict", "Offline queue identity is reused with conflicting data.",
                   "Preserve one canonical payload for each durable intent identity.");
}  // namespace Horo::PlatformOfflineQueue::PlatformOfflineQueueErrors
