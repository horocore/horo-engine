#pragma once

/**
 * @file SaveErrors.h
 * @brief Stable errors for runtime-save value validation and bounded snapshot construction.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Runtime::SaveErrors {
    /** @brief A persistent save identity was missing or used the reserved all-zero value. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief Persistent identity text or bytes were not in the canonical representation. */
    extern const ErrorCodeDescriptor IdentityMalformed;
    /** @brief A collection contained the same persistent identity more than once. */
    extern const ErrorCodeDescriptor IdentityDuplicate;
    /** @brief A participant type identity was empty, oversized, or noncanonical. */
    extern const ErrorCodeDescriptor ParticipantIdInvalid;
    /** @brief A schema or format version used the reserved zero value. */
    extern const ErrorCodeDescriptor VersionInvalid;
    /** @brief Input requires a newer schema or format version than the reader supports. */
    extern const ErrorCodeDescriptor VersionUnsupportedNewer;
    /** @brief A save participant descriptor is incomplete or internally contradictory. */
    extern const ErrorCodeDescriptor ParticipantDescriptorInvalid;
    /** @brief A participant registration omitted its owned adapter lease. */
    extern const ErrorCodeDescriptor ParticipantAdapterMissing;
    /** @brief The registry already contains the participant identity. */
    extern const ErrorCodeDescriptor ParticipantDuplicate;
    /** @brief Two participants claim ownership of the same canonical record identity. */
    extern const ErrorCodeDescriptor ParticipantRecordOwnershipDuplicate;
    /** @brief Participant registration or publication was attempted after registry shutdown. */
    extern const ErrorCodeDescriptor ParticipantRegistryClosed;
    /** @brief The bounded participant registry has no remaining capacity. */
    extern const ErrorCodeDescriptor ParticipantRegistryCapacityExceeded;
    /** @brief Participant registry binding or immutable snapshot storage could not be allocated. */
    extern const ErrorCodeDescriptor ParticipantRegistryAllocationFailed;
    /** @brief A participant declares a dependency absent from the registry snapshot. */
    extern const ErrorCodeDescriptor ParticipantDependencyMissing;
    /** @brief Participant dependencies contain a cycle. */
    extern const ErrorCodeDescriptor ParticipantDependencyCycle;
    /** @brief A dependency names a capture/restore phase unsupported by either participant. */
    extern const ErrorCodeDescriptor ParticipantDependencyPhaseIncompatible;
    /** @brief The registry generation cannot advance without reusing a value. */
    extern const ErrorCodeDescriptor ParticipantRegistryGenerationExhausted;
    /** @brief Capture evidence or operation bounds are missing, zero, or outside qualified maxima. */
    extern const ErrorCodeDescriptor CaptureContextInvalid;
    /** @brief The capture request does not address the exact pinned participant registry generation. */
    extern const ErrorCodeDescriptor CaptureRegistryStale;
    /** @brief A captured record has an unknown owner, mismatched schema, role, or record identity. */
    extern const ErrorCodeDescriptor CaptureRecordInvalid;
    /** @brief A participant or operation-wide capture payload/count bound would be exceeded. */
    extern const ErrorCodeDescriptor CaptureBudgetExceeded;
    /** @brief One stable canonical record was supplied more than once to a capture. */
    extern const ErrorCodeDescriptor CaptureRecordDuplicate;
    /** @brief Required or partially supplied participant records are missing at snapshot seal. */
    extern const ErrorCodeDescriptor CaptureIncomplete;
    /** @brief A sealed capture builder was reused after immutable publication. */
    extern const ErrorCodeDescriptor CaptureAlreadySealed;
    /** @brief Host-owned capture bookkeeping or copied payload storage could not be allocated. */
    extern const ErrorCodeDescriptor CaptureAllocationFailed;
    /** @brief A participant violated the scoped sink, omission, or one-shot capture contract. */
    extern const ErrorCodeDescriptor CaptureAdapterContractInvalid;
    /** @brief Restore operation, generation, registry, or capacity evidence is invalid or stale. */
    extern const ErrorCodeDescriptor RestoreContextInvalid;
    /** @brief Detached restore input is missing, duplicated, unknown, or mismatched with its registry binding. */
    extern const ErrorCodeDescriptor RestoreParticipantInvalid;
    /** @brief One required participant or required prepared dependency is absent. */
    extern const ErrorCodeDescriptor RestoreParticipantIncomplete;
    /** @brief A restore receipt violated its inactive-candidate or prepared-state contract. */
    extern const ErrorCodeDescriptor RestoreAdapterContractInvalid;
    /** @brief Restore bookkeeping or bounded trace storage could not be allocated. */
    extern const ErrorCodeDescriptor RestoreAllocationFailed;
    /** @brief A restore transaction method was invoked from an incompatible ownership state. */
    extern const ErrorCodeDescriptor RestoreTransitionInvalid;
    /** @brief Session or Scene generation changed before aggregate restore activation. */
    extern const ErrorCodeDescriptor RestoreActivationStale;
    /** @brief Save header JSON was malformed, noncanonical, or had an invalid exact shape. */
    extern const ErrorCodeDescriptor ArchiveHeaderInvalid;
    /** @brief Save manifest JSON was malformed, noncanonical, duplicated, or out of order. */
    extern const ErrorCodeDescriptor ArchiveManifestInvalid;
    /** @brief Save metadata exceeded an explicit byte, string, participant, or chunk bound. */
    extern const ErrorCodeDescriptor ArchiveMetadataLimitExceeded;
    /** @brief A chunk directory has unsafe bounds, ordering, ownership, alignment, or correspondence. */
    extern const ErrorCodeDescriptor ArchiveDirectoryInvalid;
    /** @brief A chunk directory or payload exceeds a trusted framing admission bound. */
    extern const ErrorCodeDescriptor ArchiveFramingLimitExceeded;
    /** @brief The stored payload is shorter or longer than its validated directory declares. */
    extern const ErrorCodeDescriptor ArchivePayloadTruncated;
    /** @brief A selected decoded chunk does not match its manifest checksum. */
    extern const ErrorCodeDescriptor ArchiveChunkHashMismatch;
    /** @brief A caller supplied an invalid value or schema argument to the canonical encoder. */
    extern const ErrorCodeDescriptor CanonicalCodecInvalid;
    /** @brief Untrusted canonical wire bytes are malformed, noncanonical, truncated, or contain trailing data. */
    extern const ErrorCodeDescriptor CanonicalCodecCorrupt;
    /** @brief Canonical value input or output exceeds an explicit codec bound. */
    extern const ErrorCodeDescriptor CanonicalCodecLimitExceeded;
    /** @brief A canonical map, set, or record contains a duplicate encoded identity. */
    extern const ErrorCodeDescriptor CanonicalCodecDuplicate;
    /** @brief A canonical floating-point value is non-finite. */
    extern const ErrorCodeDescriptor CanonicalCodecNonFinite;
    /** @brief Canonical string bytes are not a valid UTF-8 scalar sequence. */
    extern const ErrorCodeDescriptor CanonicalCodecUtf8Invalid;
    /** @brief Trusted canonical codec limits are zero or internally contradictory. */
    extern const ErrorCodeDescriptor CanonicalCodecConfigurationInvalid;
    /** @brief Canonical codec owned storage could not be allocated within admitted bounds. */
    extern const ErrorCodeDescriptor CanonicalCodecAllocationFailed;
    /** @brief A durable reference contains an invalid stable identity or unknown form. */
    extern const ErrorCodeDescriptor ReferenceInvalid;
    /** @brief Durable reference wire bytes contain an unknown tag or invalid stable payload. */
    extern const ErrorCodeDescriptor ReferenceCorrupt;
    /** @brief A resolved, missing, remapped, or deferred reference result is contradictory. */
    extern const ErrorCodeDescriptor ReferenceResolutionInvalid;
    /** @brief Product save-root inputs are missing, relative, or structurally invalid. */
    extern const ErrorCodeDescriptor SaveRootConfigurationInvalid;
    /** @brief The selected save-root platform convention is not supported. */
    extern const ErrorCodeDescriptor SaveRootPlatformUnsupported;
    /** @brief The product save root could not be created, inspected, or canonicalized. */
    extern const ErrorCodeDescriptor SaveRootUnavailable;
    /** @brief A save-root component redirected or resolved outside its approved parent. */
    extern const ErrorCodeDescriptor SaveRootContainmentViolation;
    /** @brief A save namespace omitted a required typed identity or revision. */
    extern const ErrorCodeDescriptor NamespaceInvalid;
    /** @brief No user/profile namespace is currently available for admission. */
    extern const ErrorCodeDescriptor NamespaceUnavailable;
    /** @brief A captured namespace or binding revision no longer matches the active binding. */
    extern const ErrorCodeDescriptor NamespaceStale;
    /** @brief Trusted slot publication metadata is incomplete or uses an unknown typed value. */
    extern const ErrorCodeDescriptor SlotMetadataInvalid;
    /** @brief Trusted slot publication metadata exceeded an explicit admission bound. */
    extern const ErrorCodeDescriptor SlotMetadataLimitExceeded;
    /** @brief Caller-owned slot presentation is oversized or is not well-formed UTF-8. */
    extern const ErrorCodeDescriptor SlotDisplayMetadataInvalid;
    /** @brief A replacement changed logical slot identity or reused the committed generation. */
    extern const ErrorCodeDescriptor SlotGenerationConflict;
    /** @brief A slot-index schema, revision, state transition, or rebuild argument is invalid. */
    extern const ErrorCodeDescriptor SlotIndexInvalid;
    /** @brief A decoded slot index is malformed, unordered, duplicated, or contains invalid metadata. */
    extern const ErrorCodeDescriptor SlotIndexCorrupt;
    /** @brief Slot-index entries, observations, or diagnostics exceed a trusted operation bound. */
    extern const ErrorCodeDescriptor SlotIndexLimitExceeded;
    /** @brief Private slot-index candidate or diagnostic storage could not be allocated. */
    extern const ErrorCodeDescriptor SlotIndexAllocationFailed;
    /** @brief A local storage operation request, address, payload, or configured limit is invalid. */
    extern const ErrorCodeDescriptor StorageOperationInvalid;
    /** @brief The selected storage provider does not implement the requested operation. */
    extern const ErrorCodeDescriptor StorageCapabilityUnsupported;
    /** @brief A storage provider returned a value that contradicts the requested operation. */
    extern const ErrorCodeDescriptor StorageResultInvalid;
    /** @brief Storage operation admission state could not be allocated. */
    extern const ErrorCodeDescriptor StorageAllocationFailed;
    /** @brief An asynchronous save operation descriptor or handle is invalid. */
    extern const ErrorCodeDescriptor OperationInvalid;
    /** @brief Asynchronous save operation state or callback storage could not be allocated. */
    extern const ErrorCodeDescriptor OperationAllocationFailed;
    /** @brief An asynchronous save operation progress or terminal transition is invalid. */
    extern const ErrorCodeDescriptor OperationTransitionInvalid;
    /** @brief The bounded completion callback capacity is exhausted. */
    extern const ErrorCodeDescriptor OperationCallbackCapacityExceeded;
    /** @brief A completion callback is empty and cannot be registered. */
    extern const ErrorCodeDescriptor OperationCallbackInvalid;
    /** @brief Cooperative cancellation won before the operation commit gate. */
    extern const ErrorCodeDescriptor OperationCancelled;
    /** @brief The operation deadline elapsed before the commit gate. */
    extern const ErrorCodeDescriptor OperationDeadlineExceeded;
    /** @brief The operation producer was released without publishing a terminal result. */
    extern const ErrorCodeDescriptor OperationAbandoned;
    /** @brief A save lifecycle coordinator, operation descriptor, or generation is malformed. */
    extern const ErrorCodeDescriptor LifecycleInvalid;
    /** @brief Save lifecycle mutation or safe-point execution was attempted from a non-owner thread. */
    extern const ErrorCodeDescriptor ThreadAffinityViolation;
    /** @brief Save work addresses a runtime, scene, or registry generation that is no longer current. */
    extern const ErrorCodeDescriptor GenerationStale;
    /** @brief Save simulation work was requested outside CommitDeferredLifecycleChanges. */
    extern const ErrorCodeDescriptor SafePointInvalid;
    /** @brief Save safe-point work is deferred while the runtime is suspended. */
    extern const ErrorCodeDescriptor LifecycleSuspended;
    /** @brief Save lifecycle admission or mutation is closed during host teardown. */
    extern const ErrorCodeDescriptor LifecycleUnavailable;
    /** @brief The bounded save lifecycle operation store has no remaining capacity. */
    extern const ErrorCodeDescriptor LifecycleCapacityExceeded;
    /** @brief A detached worker completion is malformed or contradicts operation state. */
    extern const ErrorCodeDescriptor CompletionInvalid;
    /** @brief A host safe-point callback unexpectedly threw instead of returning a typed failure. */
    extern const ErrorCodeDescriptor LifecycleCallbackFailed;
    /** @brief Lifecycle mutation attempted to re-enter an active save safe-point drain. */
    extern const ErrorCodeDescriptor LifecycleReentrant;
    /** @brief Project-authored save mode policy is contradictory, malformed, or outside portable limits. */
    extern const ErrorCodeDescriptor PolicyInvalid;
    /** @brief Enabled project policy requires a runtime capability without an admitted fallback. */
    extern const ErrorCodeDescriptor PolicyCapabilityUnsupported;
    /** @brief The selected save composition explicitly does not support persistence. */
    extern const ErrorCodeDescriptor CompositionUnsupported;
    /** @brief Deterministic composition limits or a submitted request are malformed. */
    extern const ErrorCodeDescriptor CompositionInvalid;
    /** @brief The deterministic composition cannot retain another operation, object, or payload. */
    extern const ErrorCodeDescriptor CompositionCapacityExceeded;
    /** @brief Cancellation won before the deterministic operation's in-memory commit point. */
    extern const ErrorCodeDescriptor CompositionCancelled;
    /** @brief A deterministic test fault failed the operation before publication. */
    extern const ErrorCodeDescriptor CompositionInjectedFailure;
    /** @brief A deterministic load or remove addressed no committed object. */
    extern const ErrorCodeDescriptor CompositionObjectMissing;
    /** @brief Save diagnostic input is malformed, unordered, contradictory, or over capacity. */
    extern const ErrorCodeDescriptor DiagnosticInvalid;
    /** @brief A diagnostic source error is foreign, unknown, or not declared by Runtime Save. */
    extern const ErrorCodeDescriptor DiagnosticUnsupported;
    /** @brief Diagnostic correlation does not match the expected active save generations. */
    extern const ErrorCodeDescriptor DiagnosticCorrelationStale;
}  // namespace Horo::Runtime::SaveErrors
