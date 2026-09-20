#pragma once

/**
 * @file NavigationErrors.h
 * @brief Stable Horo navigation error identities independent of provider-native failures.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Navigation::NavigationErrors {
    /** @brief A stable or runtime navigation identity uses its reserved invalid representation. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief A runtime navigation handle is malformed, foreign, or generation-stale. */
    extern const ErrorCodeDescriptor InvalidHandle;
    /** @brief A navigation generation cannot advance without reusing an issued identity. */
    extern const ErrorCodeDescriptor GenerationExhausted;
    /** @brief Capability evidence, requested query identity, quality, or finite limits are malformed. */
    extern const ErrorCodeDescriptor CapabilityDescriptorInvalid;
    /** @brief Provider capability evidence changed after the caller captured its revision. */
    extern const ErrorCodeDescriptor CapabilityStale;
    /** @brief The selected provider permanently does not implement the requested operation or quality. */
    extern const ErrorCodeDescriptor OperationUnsupported;
    /** @brief Known provider functionality is not currently available. */
    extern const ErrorCodeDescriptor CapabilityUnavailable;
    /** @brief Requested finite work or output bounds exceed the provider declaration. */
    extern const ErrorCodeDescriptor QueryLimitExceeded;
    /** @brief Queue, request-record, or admission capacity rejected work before a handle or job existed. */
    extern const ErrorCodeDescriptor AdmissionRejected;
    /** @brief An admitted navigation query was cancelled before owner-thread publication. */
    extern const ErrorCodeDescriptor QueryCancelled;
    /** @brief An admitted query result belongs to an old navigation topology generation. */
    extern const ErrorCodeDescriptor StaleSnapshot;
    /** @brief Required navigation coverage or active world data is unavailable. */
    extern const ErrorCodeDescriptor NoNavigationData;
    /** @brief Provider execution failed after admission; normalized Horo diagnostics carry actionable detail. */
    extern const ErrorCodeDescriptor ProviderFailed;
    /** @brief A terminal outcome or its bounded provenance/coverage evidence is malformed. */
    extern const ErrorCodeDescriptor OutcomeDescriptorInvalid;
    /** @brief An accepted request belongs to a revoked or replacement navigation-world incarnation. */
    extern const ErrorCodeDescriptor InvalidWorld;
    /** @brief An accepted operation exhausted a declared execution, scratch, result, or retry bound. */
    extern const ErrorCodeDescriptor CapacityExceeded;
    /** @brief An area identity, source, or finite non-negative traversal cost is malformed. */
    extern const ErrorCodeDescriptor AreaDescriptorInvalid;
    /** @brief A filter identity, source, referenced identity, or finite non-negative cost is malformed. */
    extern const ErrorCodeDescriptor FilterDescriptorInvalid;
    /** @brief Area, filter, or filter-override identities collide within one deterministic registry. */
    extern const ErrorCodeDescriptor DescriptorConflict;
    /** @brief An exact area identity is absent and cannot be replaced by a default area. */
    extern const ErrorCodeDescriptor AreaUnknown;
    /** @brief An exact filter identity is absent and cannot be replaced by a default filter. */
    extern const ErrorCodeDescriptor FilterUnknown;
    /** @brief A grounded-agent profile identity, name, dimension, or bake resolution value is invalid. */
    extern const ErrorCodeDescriptor AgentProfileInvalid;
    /** @brief Profile deletion would leave one or more stable authored references dangling. */
    extern const ErrorCodeDescriptor AgentProfileReferenced;
    /** @brief Navigation bake-source geometry, transform, topology, identity, or limits are malformed. */
    extern const ErrorCodeDescriptor SourceGeometryInvalid;
    /** @brief A geometry producer kind is outside the closed canonical source contract. */
    extern const ErrorCodeDescriptor SourceGeometryUnsupported;
    /** @brief Source geometry exceeds a qualified contribution, vertex, triangle, or owned-byte bound. */
    extern const ErrorCodeDescriptor SourceGeometryCapacityExceeded;
    /** @brief Captured source revision or digest evidence no longer matches authoritative geometry. */
    extern const ErrorCodeDescriptor SourceGeometryStale;
    /** @brief Bake-input revisions, identities, transforms, bounds, modes or resolved partitions are malformed. */
    extern const ErrorCodeDescriptor BakeInputInvalid;
    /** @brief A bake surface or modifier references an absent profile, source, surface, filter or area. */
    extern const ErrorCodeDescriptor BakeInputReferenceMissing;
    /** @brief Canonical bake-input capture exceeds a qualified count, work-unit or owned-byte bound. */
    extern const ErrorCodeDescriptor BakeInputCapacityExceeded;
    /** @brief A captured bake revision, request generation or source provenance no longer matches current authority. */
    extern const ErrorCodeDescriptor BakeInputStale;
    /** @brief The owning operation was cancelled before the bake-input publication barrier. */
    extern const ErrorCodeDescriptor BakeInputCancelled;
    /** @brief The owning operation failed before the bake-input publication barrier. */
    extern const ErrorCodeDescriptor BakeInputFailed;
    /** @brief Application shutdown closed bake-input publication admission. */
    extern const ErrorCodeDescriptor BakeInputShuttingDown;
    /** @brief A staged bake-job descriptor, work order, callback, or resource profile is malformed. */
    extern const ErrorCodeDescriptor BakeJobInvalid;
    /** @brief A staged bake cannot fit a declared concurrency, memory, temporary-storage, item, or work-unit budget. */
    extern const ErrorCodeDescriptor BakeJobBudgetExceeded;
    /** @brief The application operation store or process scheduler rejected a staged bake before execution. */
    extern const ErrorCodeDescriptor BakeJobAdmissionRejected;
    /** @brief A project profile identity, revision, capability requirement or finite capacity is malformed. */
    extern const ErrorCodeDescriptor ProjectProfileInvalid;
    /** @brief A profile candidate or preview preference does not target the current project revision. */
    extern const ErrorCodeDescriptor ProjectProfileStale;
    /** @brief A profile aggregate, provider or usage exceeds an authoritative finite capacity. */
    extern const ErrorCodeDescriptor ProjectProfileCapacityExceeded;
    /** @brief A NavMesh artifact header, coordinate frame, table, range, or semantic value is invalid. */
    extern const ErrorCodeDescriptor NavMeshArtifactInvalid;
    /** @brief A NavMesh artifact checksum, table partition, reference, or encoded payload is corrupt. */
    extern const ErrorCodeDescriptor NavMeshArtifactCorrupt;
    /** @brief A NavMesh artifact count, encoded size, decoded size, or owned storage exceeds a qualified bound. */
    extern const ErrorCodeDescriptor NavMeshArtifactCapacityExceeded;
    /** @brief A neutral or provider-private cooked format is not compatible with this consumer. */
    extern const ErrorCodeDescriptor UnsupportedCookedVersion;
    /** @brief An exact independently addressable NavMesh tile is absent. */
    extern const ErrorCodeDescriptor NavMeshTileUnknown;
    /** @brief No provider-private payload matches the exact requested provider fingerprint. */
    extern const ErrorCodeDescriptor NavMeshProviderPayloadUnavailable;
    /** @brief A provider-private payload exists but its version, endian, compression, or fingerprint is incompatible. */
    extern const ErrorCodeDescriptor NavMeshProviderPayloadIncompatible;
    /** @brief A Scene navigation surface, region, modifier, or link payload is malformed or exceeds authored bounds. */
    extern const ErrorCodeDescriptor SceneComponentInvalid;
    /** @brief Stable identities collide within a Scene navigation component domain. */
    extern const ErrorCodeDescriptor SceneComponentConflict;
    /** @brief A Scene navigation region, modifier, or link endpoint references an absent surface. */
    extern const ErrorCodeDescriptor SceneSurfaceMissing;
    /** @brief A grounded link selects a profile absent from one or both endpoint surfaces. */
    extern const ErrorCodeDescriptor SceneProfileMismatch;
    /** @brief A dynamic obstacle or modifier registry payload or safe-point fence is malformed. */
    extern const ErrorCodeDescriptor DynamicRegistryInvalid;
    /** @brief A dynamic obstacle or modifier identity is already present in the staged or active publication. */
    extern const ErrorCodeDescriptor DynamicRegistryConflict;
    /** @brief A dynamic obstacle or modifier update targets an old owner, source, or registry generation. */
    extern const ErrorCodeDescriptor DynamicRegistryStale;
    /** @brief A dynamic registry count, command, or snapshot bound was exceeded. */
    extern const ErrorCodeDescriptor DynamicRegistryCapacityExceeded;
    /** @brief A dynamic obstacle or modifier was updated more often than its selected profile permits. */
    extern const ErrorCodeDescriptor DynamicRegistryUpdateRateExceeded;
    /** @brief Dynamic registry mutation or snapshot admission is closed during teardown. */
    extern const ErrorCodeDescriptor DynamicRegistryShuttingDown;
    /** @brief The navigation source envelope has invalid framing or reserved fields. */
    extern const ErrorCodeDescriptor SourceEnvelopeInvalid;
    /** @brief A navigation source schema version is outside the explicitly supported range. */
    extern const ErrorCodeDescriptor SourceUnsupportedVersion;
    /** @brief Two authored or generated source records claim the same stable identity. */
    extern const ErrorCodeDescriptor SourceDuplicateIdentity;
    /** @brief The navigation source envelope checksum does not match its exact bytes. */
    extern const ErrorCodeDescriptor SourceChecksumMismatch;
    /** @brief A source record or envelope exceeds a qualified bounded parser limit. */
    extern const ErrorCodeDescriptor SourceSerializationCapacityExceeded;
    /** @brief An unknown authored record cannot be safely retained under the selected policy. */
    extern const ErrorCodeDescriptor SourceUnknownAuthoredRecord;
    /** @brief A generated payload was retained for inspection but quarantined from activation. */
    extern const ErrorCodeDescriptor GeneratedPayloadQuarantined;
    /** @brief No explicit migration edge exists for the requested source-schema transition. */
    extern const ErrorCodeDescriptor SourceMigrationMissing;
    /** @brief An explicit migration catalog contains an invalid or ambiguous edge. */
    extern const ErrorCodeDescriptor SourceMigrationInvalid;

    // Compatibility names keep the serialization vocabulary discoverable without duplicating error identities.
    inline const ErrorCodeDescriptor &SerializationDuplicateIdentity = SourceDuplicateIdentity;
    inline const ErrorCodeDescriptor &SerializationUnsupportedVersion = SourceUnsupportedVersion;
    inline const ErrorCodeDescriptor &SerializationChecksumMismatch = SourceChecksumMismatch;
    inline const ErrorCodeDescriptor &SerializationCapacityExceeded = SourceSerializationCapacityExceeded;
    inline const ErrorCodeDescriptor &SerializationUnknownAuthoredRecord = SourceUnknownAuthoredRecord;
    inline const ErrorCodeDescriptor &SerializationGeneratedPayloadQuarantined = GeneratedPayloadQuarantined;
}  // namespace Horo::Navigation::NavigationErrors
