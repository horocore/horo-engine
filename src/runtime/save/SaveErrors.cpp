#include "Horo/Runtime/Save/SaveErrors.h"

namespace Horo::Runtime::SaveErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.save"};
        constexpr auto kError = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor IdentityInvalid{kDomain, ErrorCode{"save.identity.invalid"}, kError,
                                              "A required save identity is missing or reserved.",
                                              "Supply a non-zero identity allocated by the owning authority."};
    const ErrorCodeDescriptor IdentityMalformed{kDomain, ErrorCode{"save.identity.malformed"}, kError,
                                                "A save identity is not in canonical form.",
                                                "Use the exact lowercase UUID representation."};
    const ErrorCodeDescriptor IdentityDuplicate{kDomain, ErrorCode{"save.identity.duplicate"}, kError,
                                                "A save identity occurs more than once.", "Provide each identity exactly once."};
    const ErrorCodeDescriptor ParticipantIdInvalid{kDomain, ErrorCode{"save.participant_id.invalid"}, kError,
                                                   "A save participant identity is not canonical.",
                                                   "Use a bounded lowercase dotted identity with letter-led segments."};
    const ErrorCodeDescriptor VersionInvalid{kDomain, ErrorCode{"save.version.invalid"}, kError,
                                             "A save version uses the reserved zero value.",
                                             "Supply an explicitly declared non-zero version."};
    const ErrorCodeDescriptor VersionUnsupportedNewer{kDomain, ErrorCode{"save.version.unsupported_newer"}, kError,
                                                      "The save version is newer than this reader supports.",
                                                      "Use a compatible reader or an explicit supported migration."};
    const ErrorCodeDescriptor ParticipantDescriptorInvalid{kDomain, ErrorCode{"save.participant.descriptor_invalid"}, kError,
                                                           "A save participant descriptor is invalid.",
                                                           "Correct its identity, version, roles, dependencies, ownership, and bounds."};
    const ErrorCodeDescriptor ParticipantAdapterMissing{kDomain, ErrorCode{"save.participant.adapter_missing"}, kError,
                                                        "A save participant adapter lease is missing.",
                                                        "Bind an owned adapter during explicit host composition."};
    const ErrorCodeDescriptor ParticipantDuplicate{kDomain, ErrorCode{"save.participant.duplicate"}, kError,
                                                   "A save participant identity is already registered.",
                                                   "Register each semantic participant identity exactly once."};
    const ErrorCodeDescriptor ParticipantRecordOwnershipDuplicate{kDomain, ErrorCode{"save.participant.record_ownership_duplicate"}, kError,
                                                                  "A canonical save record has more than one semantic owner.",
                                                                  "Assign each record identity to exactly one participant."};
    const ErrorCodeDescriptor ParticipantRegistryClosed{kDomain, ErrorCode{"save.participant.registry_closed"}, kError,
                                                        "The save participant registry is closed.",
                                                        "Register or rebind participants before the owning lifecycle closes."};
    const ErrorCodeDescriptor ParticipantRegistryCapacityExceeded{kDomain, ErrorCode{"save.participant.registry_capacity_exceeded"}, kError,
                                                                  "The save participant registry reached its bounded capacity.",
                                                                  "Reduce participant count or revise the explicit product limit."};
    const ErrorCodeDescriptor ParticipantRegistryAllocationFailed{kDomain,
                                                                  ErrorCode{"save.participant.registry_allocation_failed"},
                                                                  kError,
                                                                  "Save participant registry storage could not be allocated.",
                                                                  "Release memory and retry composition before admitting save work.",
                                                                  true};
    const ErrorCodeDescriptor ParticipantDependencyMissing{kDomain, ErrorCode{"save.participant.dependency_missing"}, kError,
                                                           "A required save participant dependency is absent.",
                                                           "Register every declared dependency before publishing a snapshot."};
    const ErrorCodeDescriptor ParticipantDependencyCycle{kDomain, ErrorCode{"save.participant.dependency_cycle"}, kError,
                                                         "Save participant dependencies contain a cycle.",
                                                         "Remove the cycle so semantic ownership has an acyclic dependency graph."};
    const ErrorCodeDescriptor ParticipantDependencyPhaseIncompatible{kDomain, ErrorCode{"save.participant.dependency_phase_incompatible"},
                                                                     kError,
                                                                     "A save participant dependency names an unsupported operation phase.",
                                                                     "Give both participants the required phase capability or correct the "
                                                                     "typed dependency phase."};
    const ErrorCodeDescriptor ParticipantRegistryGenerationExhausted{kDomain, ErrorCode{"save.participant.registry_generation_exhausted"},
                                                                     ErrorSeverity::Critical,
                                                                     "The save participant registry generation is exhausted.",
                                                                     "Stop the owning runtime instead of reusing a registry generation."};
    const ErrorCodeDescriptor CaptureContextInvalid{kDomain, ErrorCode{"save.capture.context_invalid"}, kError,
                                                    "Runtime save capture evidence or bounds are invalid.",
                                                    "Capture at one issued safe point with finite qualified limits."};
    const ErrorCodeDescriptor CaptureRegistryStale{kDomain, ErrorCode{"save.capture.registry_stale"}, kError,
                                                   "Runtime save capture addressed a different participant registry generation.",
                                                   "Restart capture with the exact currently pinned registry snapshot."};
    const ErrorCodeDescriptor CaptureRecordInvalid{kDomain, ErrorCode{"save.capture.record_invalid"}, kError,
                                                   "A canonical capture record contradicts its participant registration.",
                                                   "Use a registered capture owner, exact schema, and owned record identity."};
    const ErrorCodeDescriptor CaptureBudgetExceeded{kDomain, ErrorCode{"save.capture.budget_exceeded"}, kError,
                                                    "Runtime save capture would exceed a declared participant or operation bound.",
                                                    "Reject or defer capture before allocating detached payload storage."};
    const ErrorCodeDescriptor CaptureRecordDuplicate{kDomain, ErrorCode{"save.capture.record_duplicate"}, kError,
                                                     "A canonical record occurs more than once in one runtime save capture.",
                                                     "Supply each participant-owned record exactly once."};
    const ErrorCodeDescriptor CaptureIncomplete{kDomain, ErrorCode{"save.capture.incomplete"}, kError,
                                                "Runtime save capture is missing required participant records.",
                                                "Capture every owned record for required or participating optional owners at one epoch."};
    const ErrorCodeDescriptor CaptureAlreadySealed{kDomain, ErrorCode{"save.capture.already_sealed"}, kError,
                                                   "The runtime save capture builder is sealed, spent, or moved from.",
                                                   "Create a new owner-safe-point builder for another capture."};
    const ErrorCodeDescriptor CaptureAllocationFailed{kDomain,
                                                      ErrorCode{"save.capture.allocation_failed"},
                                                      kError,
                                                      "Host-owned runtime save capture storage could not be allocated.",
                                                      "Release admitted capture memory and retry at a later safe point.",
                                                      true};
    const ErrorCodeDescriptor
        CaptureAdapterContractInvalid{kDomain, ErrorCode{"save.capture.adapter_contract_invalid"}, kError,
                                      "A runtime save participant violated its scoped capture contract.",
                                      "Fix the adapter to honor its exact context, sink failures, and omission disposition."};
    const ErrorCodeDescriptor RestoreContextInvalid{kDomain, ErrorCode{"save.restore.context_invalid"}, kError,
                                                    "Staged restore operation or generation evidence is invalid.",
                                                    "Restart restore with the current load operation, registry, session, and Scene."};
    const ErrorCodeDescriptor RestoreParticipantInvalid{kDomain, ErrorCode{"save.restore.participant_invalid"}, kError,
                                                        "Detached restore input contradicts its participant registration.",
                                                        "Supply each registered restore participant once with its exact schema and scope."};
    const ErrorCodeDescriptor
        RestoreParticipantIncomplete{kDomain, ErrorCode{"save.restore.participant_incomplete"}, kError,
                                     "Staged restore is missing required participant state.",
                                     "Decode every required participant and required restore dependency before staging."};
    const ErrorCodeDescriptor
        RestoreAdapterContractInvalid{kDomain, ErrorCode{"save.restore.adapter_contract_invalid"}, kError,
                                      "A restore participant violated its inactive candidate contract.",
                                      "Keep fallible work private and expose prepared state before aggregate activation."};
    const ErrorCodeDescriptor RestoreAllocationFailed{kDomain,
                                                      ErrorCode{"save.restore.allocation_failed"},
                                                      kError,
                                                      "Bounded staged restore bookkeeping could not be allocated.",
                                                      "Release admitted restore memory and retry at a later lifecycle boundary.",
                                                      true};
    const ErrorCodeDescriptor RestoreTransitionInvalid{kDomain, ErrorCode{"save.restore.transition_invalid"}, kError,
                                                       "A staged restore transaction transition is invalid.",
                                                       "Prepare once, then activate or roll back the unpublished candidate bundle."};
    const ErrorCodeDescriptor RestoreActivationStale{kDomain, ErrorCode{"save.restore.activation_stale"}, kError,
                                                     "The runtime session or Scene changed before restore activation.",
                                                     "Discard the candidate bundle and restart against the current runtime generation."};
    const ErrorCodeDescriptor ArchiveHeaderInvalid{kDomain, ErrorCode{"save.archive.header_invalid"}, kError,
                                                   "Save archive header metadata is invalid.",
                                                   "Use the exact bounded canonical header schema and required fields."};
    const ErrorCodeDescriptor ArchiveManifestInvalid{kDomain, ErrorCode{"save.archive.manifest_invalid"}, kError,
                                                     "Save archive manifest metadata is invalid.",
                                                     "Use unique stable-sorted participant and chunk identities with valid schemas."};
    const ErrorCodeDescriptor ArchiveMetadataLimitExceeded{kDomain, ErrorCode{"save.archive.metadata_limit_exceeded"}, kError,
                                                           "Save archive metadata exceeds an admission bound.",
                                                           "Reduce metadata size or revise the trusted product limits."};
    const ErrorCodeDescriptor ArchiveDirectoryInvalid{kDomain, ErrorCode{"save.archive.directory_invalid"}, kError,
                                                      "Save archive chunk directory framing is invalid.",
                                                      "Use bounded, contiguous, aligned, uniquely owned chunk records."};
    const ErrorCodeDescriptor ArchiveFramingLimitExceeded{kDomain, ErrorCode{"save.archive.framing_limit_exceeded"}, kError,
                                                          "Save archive framing exceeds an admission bound.",
                                                          "Reduce entry or payload size or revise trusted product limits."};
    const ErrorCodeDescriptor ArchivePayloadTruncated{kDomain, ErrorCode{"save.archive.payload_truncated"}, kError,
                                                      "Save archive payload length contradicts its validated directory.",
                                                      "Provide the exact complete payload region before selecting a chunk."};
    const ErrorCodeDescriptor ArchiveChunkHashMismatch{kDomain, ErrorCode{"save.archive.chunk_hash_mismatch"}, kError,
                                                       "Decoded save chunk bytes do not match their declared digest.",
                                                       "Reject the archive and retain it for corruption diagnostics."};
    const ErrorCodeDescriptor CanonicalCodecInvalid{kDomain, ErrorCode{"save.canonical_codec.invalid"}, kError,
                                                    "A canonical save codec argument is invalid.",
                                                    "Supply a valid schema argument or caller-owned value."};
    const ErrorCodeDescriptor CanonicalCodecCorrupt{kDomain, ErrorCode{"save.canonical_codec.corrupt"}, kError,
                                                    "Canonical save value wire bytes are corrupt.",
                                                    "Reject malformed, truncated, noncanonical, or trailing value bytes."};
    const ErrorCodeDescriptor CanonicalCodecLimitExceeded{kDomain, ErrorCode{"save.canonical_codec.limit_exceeded"}, kError,
                                                          "A canonical save value exceeds an admission bound.",
                                                          "Reduce the value or revise the trusted participant codec limits."};
    const ErrorCodeDescriptor CanonicalCodecDuplicate{kDomain, ErrorCode{"save.canonical_codec.duplicate"}, kError,
                                                      "A canonical collection contains a duplicate encoded identity.",
                                                      "Provide unique map keys, set values, and record field identities."};
    const ErrorCodeDescriptor CanonicalCodecNonFinite{kDomain, ErrorCode{"save.canonical_codec.non_finite"}, kError,
                                                      "A canonical floating-point value is not finite.",
                                                      "Map special values explicitly in the versioned participant adapter."};
    const ErrorCodeDescriptor CanonicalCodecUtf8Invalid{kDomain, ErrorCode{"save.canonical_codec.utf8_invalid"}, kError,
                                                        "Canonical string bytes are not valid UTF-8 scalar values.",
                                                        "Supply validated UTF-8 without implicit codec normalization."};
    const ErrorCodeDescriptor CanonicalCodecConfigurationInvalid{kDomain, ErrorCode{"save.canonical_codec.configuration_invalid"}, kError,
                                                                 "Canonical codec limits are invalid.",
                                                                 "Provide finite non-zero internally coherent trusted limits."};
    const ErrorCodeDescriptor CanonicalCodecAllocationFailed{kDomain, ErrorCode{"save.canonical_codec.allocation_failed"}, kError,
                                                             "Canonical codec storage allocation failed.",
                                                             "Reduce admitted data or memory pressure before retrying."};
    const ErrorCodeDescriptor ReferenceInvalid{kDomain, ErrorCode{"save.reference.invalid"}, kError, "A durable save reference is invalid.",
                                               "Use a declared stable identity form without paths or runtime handles."};
    const ErrorCodeDescriptor ReferenceCorrupt{kDomain, ErrorCode{"save.reference.corrupt"}, kError,
                                               "Durable save reference wire bytes are corrupt.",
                                               "Reject the unknown tag or invalid stable reference payload."};
    const ErrorCodeDescriptor ReferenceResolutionInvalid{kDomain, ErrorCode{"save.reference.resolution_invalid"}, kError,
                                                         "A durable save reference resolution is contradictory.",
                                                         "Retain the original target and provide a replacement only for remapping."};
    const ErrorCodeDescriptor SaveRootConfigurationInvalid{kDomain, ErrorCode{"save.root.configuration_invalid"}, kError,
                                                           "The platform save-root configuration is invalid.",
                                                           "Provide the required absolute platform state directory."};
    const ErrorCodeDescriptor SaveRootPlatformUnsupported{kDomain, ErrorCode{"save.root.platform_unsupported"}, kError,
                                                          "The selected save-root platform convention is unsupported.",
                                                          "Compose a Windows, macOS, Linux, or explicit test resolver."};
    const ErrorCodeDescriptor SaveRootUnavailable{kDomain,
                                                  ErrorCode{"save.root.unavailable"},
                                                  kError,
                                                  "The product save root is unavailable.",
                                                  "Check user-state storage availability and directory permissions.",
                                                  true,
                                                  true};
    const ErrorCodeDescriptor SaveRootContainmentViolation{kDomain, ErrorCode{"save.root.containment_violation"}, kError,
                                                           "The product save root failed containment validation.",
                                                           "Remove redirected or unexpected entries beneath the approved state root."};
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
    const ErrorCodeDescriptor OperationInvalid{kDomain, ErrorCode{"save.operation.invalid"}, kError,
                                               "An asynchronous save operation descriptor or handle is invalid.",
                                               "Use a non-zero application operation identity and finite callback capacity."};
    const ErrorCodeDescriptor OperationAllocationFailed{kDomain,
                                                        ErrorCode{"save.operation.allocation_failed"},
                                                        kError,
                                                        "Asynchronous save operation state could not be allocated.",
                                                        "Release admitted operation memory and retry later.",
                                                        true};
    const ErrorCodeDescriptor OperationTransitionInvalid{kDomain, ErrorCode{"save.operation.transition_invalid"}, kError,
                                                         "An asynchronous save operation transition is invalid.",
                                                         "Publish bounded monotonic progress and enter the commit gate before success."};
    const ErrorCodeDescriptor
        OperationCallbackCapacityExceeded{kDomain, ErrorCode{"save.operation.callback_capacity_exceeded"}, kError,
                                          "The asynchronous save operation cannot retain another completion callback.",
                                          "Use the admitted callback capacity or poll the immutable operation snapshot."};
    const ErrorCodeDescriptor OperationCallbackInvalid{kDomain, ErrorCode{"save.operation.callback_invalid"}, kError,
                                                       "An asynchronous save operation completion callback is empty.",
                                                       "Register a callable completion observer."};
    const ErrorCodeDescriptor OperationCancelled{kDomain, ErrorCode{"save.operation.cancelled"}, kError,
                                                 "The asynchronous save operation was cancelled before commit.",
                                                 "Retry only while the owning session and request remain valid."};
    const ErrorCodeDescriptor OperationDeadlineExceeded{kDomain,
                                                        ErrorCode{"save.operation.deadline_exceeded"},
                                                        kError,
                                                        "The asynchronous save operation deadline elapsed before commit.",
                                                        "Retry with an appropriate admitted deadline if the operation remains valid.",
                                                        true};
    const ErrorCodeDescriptor OperationAbandoned{kDomain, ErrorCode{"save.operation.abandoned"}, kError,
                                                 "The asynchronous save operation producer ended without a terminal result.",
                                                 "Keep the producer alive until it completes, fails, or observes cancellation."};
    const ErrorCodeDescriptor OperationInProgress{kDomain,
                                                  ErrorCode{"save.operation.in_progress"},
                                                  kError,
                                                  "An incompatible save-domain operation is already admitted.",
                                                  "Wait for the reported operation or select an explicit queue/coalescing policy.",
                                                  true};
    const ErrorCodeDescriptor ArbiterInvalid{kDomain, ErrorCode{"save.arbiter.invalid"}, kError,
                                             "A save operation arbiter limit, request, or transition is invalid.",
                                             "Supply bounded limits, a valid typed target, and an allowed lifecycle transition."};
    const ErrorCodeDescriptor ArbiterCapacityExceeded{kDomain,
                                                      ErrorCode{"save.arbiter.capacity_exceeded"},
                                                      kError,
                                                      "The save operation arbiter cannot retain another request.",
                                                      "Acknowledge terminal operations or revise the explicit session capacity.",
                                                      true};
    const ErrorCodeDescriptor LifecycleInvalid{kDomain, ErrorCode{"save.lifecycle.invalid"}, kError,
                                               "Runtime Save lifecycle evidence is invalid.",
                                               "Supply a non-zero operation and exact runtime, scene, and registry generations."};
    const ErrorCodeDescriptor ThreadAffinityViolation{kDomain, ErrorCode{"save.lifecycle.thread_affinity_violation"}, kError,
                                                      "Runtime Save lifecycle work ran on a non-owner thread.",
                                                      "Route admission, transitions, and safe-point work to the runtime owner thread."};
    const ErrorCodeDescriptor GenerationStale{kDomain, ErrorCode{"save.lifecycle.generation_stale"}, kError,
                                              "Runtime Save work addresses a stale runtime, scene, or registry generation.",
                                              "Discard the completion or restart work against the exact active generations."};
    const ErrorCodeDescriptor SafePointInvalid{kDomain, ErrorCode{"save.lifecycle.safe_point_invalid"}, kError,
                                               "Simulation-owned save work was requested outside its lifecycle commit safe point.",
                                               "Run capture and restore publication inside CommitDeferredLifecycleChanges."};
    const ErrorCodeDescriptor LifecycleSuspended{kDomain, ErrorCode{"save.lifecycle.suspended"}, kError,
                                                 "Runtime Save safe-point work is deferred while the runtime is suspended.",
                                                 "Keep the operation queued and retry after the runtime resumes."};
    const ErrorCodeDescriptor LifecycleUnavailable{kDomain, ErrorCode{"save.lifecycle.unavailable"}, kError,
                                                   "Runtime Save lifecycle admission is closed.",
                                                   "Do not submit new work after host shutdown begins."};
    const ErrorCodeDescriptor LifecycleCapacityExceeded{kDomain, ErrorCode{"save.lifecycle.capacity_exceeded"}, kError,
                                                        "Runtime Save lifecycle operation capacity is exhausted.",
                                                        "Retire acknowledged operations or revise the explicit session bound."};
    const ErrorCodeDescriptor CompletionInvalid{kDomain, ErrorCode{"save.lifecycle.completion_invalid"}, kError,
                                                "A Runtime Save worker completion contradicts its fenced operation.",
                                                "Publish exactly one well-formed completion for detached work."};
    const ErrorCodeDescriptor
        LifecycleCallbackFailed{kDomain, ErrorCode{"save.lifecycle.callback_failed"}, kError,
                                "A Runtime Save safe-point callback threw unexpectedly.",
                                "Return expected failures through Result and keep safe-point callbacks non-throwing."};
    const ErrorCodeDescriptor LifecycleReentrant{kDomain, ErrorCode{"save.lifecycle.reentrant"}, kError,
                                                 "Runtime Save lifecycle mutation re-entered an active safe-point drain.",
                                                 "Queue follow-up work and apply it after the current safe point returns."};
    const ErrorCodeDescriptor PolicyInvalid{kDomain,
                                            ErrorCode{"save.policy.invalid"},
                                            kError,
                                            "The project save policy is contradictory or invalid.",
                                            "Correct the named save mode in project settings and recook the project.",
                                            false,
                                            true};
    const ErrorCodeDescriptor
        PolicyCapabilityUnsupported{kDomain,
                                    ErrorCode{"save.policy.capability_unsupported"},
                                    kError,
                                    "The project save policy requires an unavailable runtime capability.",
                                    "Choose a supported composition or declare an explicit safe fallback for the named mode.",
                                    false,
                                    true};
    const ErrorCodeDescriptor CompositionUnsupported{kDomain, ErrorCode{"save.composition.unsupported"}, kError,
                                                     "The selected save composition does not support persistence.",
                                                     "Select a save-capable product composition before admission."};
    const ErrorCodeDescriptor CompositionInvalid{kDomain, ErrorCode{"save.composition.invalid"}, kError,
                                                 "A deterministic save composition input is invalid.",
                                                 "Supply finite positive limits and a valid typed operation request."};
    const ErrorCodeDescriptor CompositionCapacityExceeded{kDomain, ErrorCode{"save.composition.capacity_exceeded"}, kError,
                                                          "The deterministic save composition reached a declared bound.",
                                                          "Retire test state or increase the explicit test-only capacity."};
    const ErrorCodeDescriptor CompositionCancelled{kDomain, ErrorCode{"save.composition.cancelled"}, kError,
                                                   "The deterministic save operation was cancelled before publication.",
                                                   "Retry only when the owning operation remains valid."};
    const ErrorCodeDescriptor CompositionInjectedFailure{kDomain, ErrorCode{"save.composition.injected_failure"}, kError,
                                                         "The deterministic save operation reached its injected failure.",
                                                         "Remove the deliberate test fault before retrying."};
    const ErrorCodeDescriptor CompositionObjectMissing{kDomain, ErrorCode{"save.composition.object_missing"}, kError,
                                                       "No deterministic save object exists at the requested address.",
                                                       "Store the object before loading or removing it."};
    const ErrorCodeDescriptor DiagnosticInvalid{kDomain, ErrorCode{"save.diagnostic.invalid"}, kError,
                                                "Runtime Save diagnostic evidence is invalid.",
                                                "Supply canonical bounded typed evidence from the owning operation."};
    const ErrorCodeDescriptor DiagnosticUnsupported{kDomain, ErrorCode{"save.diagnostic.unsupported"}, kError,
                                                    "Runtime Save cannot classify the diagnostic source error.",
                                                    "Map the source to an explicitly declared Runtime Save error first."};
    const ErrorCodeDescriptor DiagnosticCorrelationStale{kDomain, ErrorCode{"save.diagnostic.correlation_stale"}, kError,
                                                         "Runtime Save diagnostic generation evidence is stale.",
                                                         "Use the exact retained owner generations for this observation."};
}  // namespace Horo::Runtime::SaveErrors
