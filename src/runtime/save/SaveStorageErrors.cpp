#include "Horo/Runtime/Save/SaveErrors.h"

namespace Horo::Runtime::SaveErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.save"};
        constexpr auto kError = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor NamespaceInvalid{kDomain, ErrorCode{"save.namespace.invalid"}, kError,
                                               "A save namespace identity or revision is invalid.",
                                               "Supply complete non-zero typed namespace identities and revisions."};
    const ErrorCodeDescriptor NamespaceUnavailable{kDomain, ErrorCode{"save.namespace.unavailable"}, kError,
                                                   "No save namespace is available for the requested operation.",
                                                   "Bind an available user/profile or server-world namespace first."};
    const ErrorCodeDescriptor NamespaceStale{kDomain, ErrorCode{"save.namespace.stale"}, kError,
                                             "The captured save namespace binding is stale.",
                                             "Reject the operation and acquire the current namespace binding."};
    const ErrorCodeDescriptor SlotMetadataInvalid{kDomain, ErrorCode{"save.slot.metadata_invalid"}, kError,
                                                  "Trusted save-slot publication metadata is invalid.",
                                                  "Supply complete typed catalog facts from a committed publication."};
    const ErrorCodeDescriptor SlotMetadataLimitExceeded{kDomain, ErrorCode{"save.slot.metadata_limit_exceeded"}, kError,
                                                        "Trusted save-slot publication metadata exceeds an admission bound.",
                                                        "Reduce bounded provenance metadata or revise the trusted product limit."};
    const ErrorCodeDescriptor SlotDisplayMetadataInvalid{kDomain, ErrorCode{"save.slot.display_metadata_invalid"}, kError,
                                                         "Save-slot presentation metadata is invalid.",
                                                         "Use optional bounded well-formed UTF-8 presentation text."};
    const ErrorCodeDescriptor SlotGenerationConflict{kDomain, ErrorCode{"save.slot.generation_conflict"}, kError,
                                                     "A save-slot replacement does not advance the same logical slot.",
                                                     "Keep the slot identity and allocate a new publication generation."};
    const ErrorCodeDescriptor SlotIndexInvalid{kDomain, ErrorCode{"save.slot_index.invalid"}, kError,
                                               "The save-slot index operation or revision is invalid.",
                                               "Use the current index schema, a non-zero revision, and a fresh rebuild operation."};
    const ErrorCodeDescriptor SlotIndexCorrupt{kDomain, ErrorCode{"save.slot_index.corrupt"}, kError,
                                               "The derived save-slot index is corrupt.",
                                               "Rebuild the index from bounded validation of committed slot artifacts."};
    const ErrorCodeDescriptor SlotIndexLimitExceeded{kDomain, ErrorCode{"save.slot_index.limit_exceeded"}, kError,
                                                     "The save-slot index exceeds a trusted rebuild bound.",
                                                     "Increase an explicit product limit or remove unexpected storage artifacts."};
    const ErrorCodeDescriptor SlotIndexAllocationFailed{kDomain,
                                                        ErrorCode{"save.slot_index.allocation_failed"},
                                                        kError,
                                                        "Private save-slot index reconstruction storage could not be allocated.",
                                                        "Keep the previous index and retry after reducing memory pressure.",
                                                        true};
    const ErrorCodeDescriptor StorageOperationInvalid{kDomain, ErrorCode{"save.storage.operation_invalid"}, kError,
                                                      "A local save storage operation is invalid.",
                                                      "Supply a valid typed namespace, slot, payload, and operation bound."};
    const ErrorCodeDescriptor StorageCapabilityUnsupported{kDomain, ErrorCode{"save.storage.capability_unsupported"}, kError,
                                                           "The local save storage provider does not support this operation.",
                                                           "Select a provider that explicitly advertises the required capability."};
    const ErrorCodeDescriptor StorageResultInvalid{kDomain, ErrorCode{"save.storage.result_invalid"}, kError,
                                                   "The local save storage provider returned a contradictory result.",
                                                   "Fix the provider to return the exact bounded immutable result type requested."};
    const ErrorCodeDescriptor StorageAllocationFailed{kDomain,
                                                      ErrorCode{"save.storage.allocation_failed"},
                                                      kError,
                                                      "Local save storage operation state could not be allocated.",
                                                      "Release retained operation data and retry later.",
                                                      true};
    const ErrorCodeDescriptor SlotCommitInvalid{kDomain, ErrorCode{"save.slot_commit.invalid"}, kError,
                                                "A slot-generation commit record is invalid.",
                                                "Use one valid leased slot, a new generation, and complete finalized archive metadata."};
    const ErrorCodeDescriptor SlotCommitGenerationStale{kDomain, ErrorCode{"save.slot_commit.generation_stale"}, kError,
                                                        "The expected base slot publication no longer matches the selected generation.",
                                                        "Refresh the slot publication and explicitly retry from the current base."};
    const ErrorCodeDescriptor SlotCommitOutcomeUnknown{kDomain,
                                                       ErrorCode{"save.slot_commit.outcome_unknown"},
                                                       kError,
                                                       "Atomic slot publication has an unknown outcome.",
                                                       "Keep the slot lease and reconcile the durable journal against catalog evidence.",
                                                       true};
    const ErrorCodeDescriptor SlotCommitRecoveryFailed{kDomain,
                                                       ErrorCode{"save.slot_commit.recovery_failed"},
                                                       kError,
                                                       "Slot-generation recovery could not converge safely.",
                                                       "Quarantine slot mutation and preserve the journal for bounded operator recovery.",
                                                       true};
    const ErrorCodeDescriptor SlotRecoveryInvalid{kDomain, ErrorCode{"save.slot_recovery.invalid"}, kError,
                                                  "Last-known-good recovery evidence or policy is invalid.",
                                                  "Preserve the current evidence and correct the bounded recovery input."};
    const ErrorCodeDescriptor SlotRecoveryLimitExceeded{kDomain,
                                                        ErrorCode{"save.slot_recovery.limit_exceeded"},
                                                        kError,
                                                        "Last-known-good recovery evidence exceeds a trusted retention bound.",
                                                        "Preserve the evidence and revise the explicit recovery retention policy.",
                                                        false,
                                                        true};
    const ErrorCodeDescriptor SlotRecoveryAllocationFailed{kDomain,
                                                           ErrorCode{"save.slot_recovery.allocation_failed"},
                                                           kError,
                                                           "Last-known-good recovery bookkeeping could not be allocated.",
                                                           "Keep the current generation and retry recovery after reducing memory pressure.",
                                                           true};
    const ErrorCodeDescriptor StoragePolicyInvalid{kDomain, ErrorCode{"save.storage.policy_invalid"}, kError,
                                                   "Save storage capacity or recovery policy evidence is invalid.",
                                                   "Supply bounded ordered capacity evidence and a finite retry policy."};
    const ErrorCodeDescriptor StorageDiskFull{kDomain,
                                              ErrorCode{"save.storage.disk_full"},
                                              kError,
                                              "Physical storage lacks space for the save transaction peak.",
                                              "Free space or explicitly remove a presented reclaimable generation.",
                                              false,
                                              true};
    const ErrorCodeDescriptor StorageQuotaExceeded{kDomain,
                                                   ErrorCode{"save.storage.quota_exceeded"},
                                                   kError,
                                                   "The storage provider quota cannot admit the save transaction peak.",
                                                   "Free provider quota or select storage with sufficient capacity.",
                                                   false,
                                                   true};
    const ErrorCodeDescriptor StoragePermissionDenied{kDomain,
                                                      ErrorCode{"save.storage.permission_denied"},
                                                      kError,
                                                      "The storage authority denied the save operation.",
                                                      "Correct storage permissions before retrying.",
                                                      false,
                                                      true};
    const ErrorCodeDescriptor StorageReadOnly{kDomain,
                                              ErrorCode{"save.storage.read_only"},
                                              kError,
                                              "The selected save storage is read-only.",
                                              "Select writable storage or change the platform storage state.",
                                              false,
                                              true};
    const ErrorCodeDescriptor StorageVolumeUnavailable{kDomain,
                                                       ErrorCode{"save.storage.volume_unavailable"},
                                                       kError,
                                                       "The selected save storage volume is unavailable.",
                                                       "Reconnect or remount the storage volume before retrying.",
                                                       false,
                                                       true};
    const ErrorCodeDescriptor StorageTransientIo{kDomain,
                                                 ErrorCode{"save.storage.transient_io"},
                                                 kError,
                                                 "A transient save storage I/O operation failed.",
                                                 "Retry only within the admitted finite backoff budget.",
                                                 true};
    const ErrorCodeDescriptor StoragePermanentIo{kDomain,
                                                 ErrorCode{"save.storage.permanent_io"},
                                                 kError,
                                                 "A permanent save storage I/O operation failed.",
                                                 "Do not retry until external state or the request changes.",
                                                 false,
                                                 true};
}  // namespace Horo::Runtime::SaveErrors
