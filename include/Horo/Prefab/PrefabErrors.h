#pragma once

/**
 * @file PrefabErrors.h
 * @brief Stable prefab identity, authoring document and dependency graph errors.
 */

#include "Horo/Foundation/ErrorCode.h"

#include <span>

namespace Horo::Prefab::PrefabErrors {
    /** @brief A required prefab identity is invalid. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief A prefab-local address contains an invalid or excessive scope. */
    extern const ErrorCodeDescriptor AddressInvalid;
    /** @brief A prefab reference does not use a valid Asset Registry identity. */
    extern const ErrorCodeDescriptor ReferenceInvalid;
    /** @brief A prefab project limit policy is empty, contradictory or exceeds an engine hard ceiling. */
    extern const ErrorCodeDescriptor LimitProfileInvalid;
    /** @brief A prefab expansion operation exhausted its captured derived work budget. */
    extern const ErrorCodeDescriptor WorkBudgetExceeded;
    /** @brief A prefab authoring document has invalid top-level metadata or mode. */
    extern const ErrorCodeDescriptor DocumentInvalid;
    /** @brief A prefab hierarchy is disconnected, cyclic, unordered, or ambiguously rooted. */
    extern const ErrorCodeDescriptor HierarchyInvalid;
    /** @brief A prefab document exceeds its object-count bound. */
    extern const ErrorCodeDescriptor ObjectCountExceeded;
    /** @brief A prefab document exceeds its direct nested-placement count bound. */
    extern const ErrorCodeDescriptor NestedPlacementCountExceeded;
    /** @brief A prefab document exceeds its Asset Registry dependency count bound. */
    extern const ErrorCodeDescriptor ReferenceCountExceeded;
    /** @brief A prefab hierarchy exceeds its maximum root-inclusive depth. */
    extern const ErrorCodeDescriptor HierarchyDepthExceeded;
    /** @brief One prefab object exceeds the combined component and behavior count bound. */
    extern const ErrorCodeDescriptor ComponentCountExceeded;
    /** @brief A prefab document exceeds its bounded dynamic payload bytes. */
    extern const ErrorCodeDescriptor PayloadTooLarge;
    /** @brief Optional prefab composition data violates concrete or variant invariants. */
    extern const ErrorCodeDescriptor CompositionInvalid;
    /** @brief A prefab dependency graph candidate contains duplicate or inconsistent source data. */
    extern const ErrorCodeDescriptor DependencyGraphInvalid;
    /** @brief A dependency required by the prefab graph is absent from its pinned inputs. */
    extern const ErrorCodeDescriptor DependencyUnavailable;
    /** @brief A dependency's registered asset type conflicts with its prefab graph role. */
    extern const ErrorCodeDescriptor DependencyTypeMismatch;
    /** @brief A prefab dependency edge was authored against a different source revision. */
    extern const ErrorCodeDescriptor DependencyRevisionMismatch;
    /** @brief One dependency identity carries incompatible type or source-revision evidence. */
    extern const ErrorCodeDescriptor DependencyConflict;
    /** @brief The requested dependency conflict policy is not supported by this contract version. */
    extern const ErrorCodeDescriptor DependencyConflictPolicyUnsupported;
    /** @brief A complete dependency closure exceeds its caller-owned hard capacity. */
    extern const ErrorCodeDescriptor DependencyClosureCapacityExceeded;
    /** @brief A completed prefab resolution no longer matches the authoritative publication context. */
    extern const ErrorCodeDescriptor ResolutionStale;
    /** @brief A generated prefab scene identity collides with an authored or expanded identity. */
    extern const ErrorCodeDescriptor IdentityCollision;
    /** @brief A typed prefab reference cannot be rewritten without ambiguity or data loss. */
    extern const ErrorCodeDescriptor ReferenceRewriteInvalid;

    /** @brief A referenced prefab source is absent from the admitted source snapshot. */
    extern const ErrorCodeDescriptor MissingPrefabSource;
    /** @brief A referenced prefab source revision is unavailable or not pinned. */
    extern const ErrorCodeDescriptor SourceRevisionUnavailable;
    /** @brief A prefab source schema cannot be interpreted by the active contract. */
    extern const ErrorCodeDescriptor UnsupportedPrefabSchema;
    /** @brief A nested placement or variant placement is malformed. */
    extern const ErrorCodeDescriptor InvalidPlacement;
    /** @brief A concrete or variant source declares more than one variant parent. */
    extern const ErrorCodeDescriptor MultipleVariantParents;
    /** @brief Combined nested and variant composition contains a cycle. */
    extern const ErrorCodeDescriptor CyclicComposition;
    /** @brief Variant inheritance exceeds the project bound. */
    extern const ErrorCodeDescriptor VariantDepthExceeded;
    /** @brief Nested composition exceeds the project bound. */
    extern const ErrorCodeDescriptor CompositionDepthExceeded;
    /** @brief Combined composition edges exceed the project bound. */
    extern const ErrorCodeDescriptor CompositionEdgeLimitExceeded;

    /** @brief A prefab override record is malformed or targets an unsupported operation. */
    extern const ErrorCodeDescriptor OverrideInvalid;
    /** @brief A prefab override conflicts with another admitted layer or precondition. */
    extern const ErrorCodeDescriptor OverrideConflict;
    /** @brief A prefab override target cannot be resolved without losing authored intent. */
    extern const ErrorCodeDescriptor OverrideOrphan;
    /** @brief A prefab override was authored against a source revision that is no longer active. */
    extern const ErrorCodeDescriptor OverrideSourceStale;

    /** @brief Prefab cook input is incomplete, inconsistent, or not canonical. */
    extern const ErrorCodeDescriptor CookInputInvalid;
    /** @brief A cooked prefab artifact is malformed or fails integrity validation. */
    extern const ErrorCodeDescriptor CookArtifactInvalid;
    /** @brief A cooked prefab payload exceeds the admitted bound. */
    extern const ErrorCodeDescriptor CookPayloadTooLarge;

    /** @brief A prefab source must pass the project migration pipeline before this operation. */
    extern const ErrorCodeDescriptor MigrationRequired;
    /** @brief A prefab source migration failed before publication. */
    extern const ErrorCodeDescriptor MigrationFailed;

    /** @brief The requested runtime prefab is absent from the cooked catalog. */
    extern const ErrorCodeDescriptor AssetNotFound;
    /** @brief A direct runtime prefab operation requires an asset that is not resident. */
    extern const ErrorCodeDescriptor AssetNotLoaded;
    /** @brief The cooked prefab format is not supported by this runtime. */
    extern const ErrorCodeDescriptor UnsupportedCookedVersion;
    /** @brief A cooked prefab payload is corrupt or fails its integrity envelope. */
    extern const ErrorCodeDescriptor CorruptedPayload;
    /** @brief A cooked prefab references an unavailable gameplay component type. */
    extern const ErrorCodeDescriptor ComponentTypeUnregistered;
    /** @brief Runtime staging could not allocate a required component payload. */
    extern const ErrorCodeDescriptor ComponentAllocationFailed;
    /** @brief A runtime spawn request repeats an asset in its inherited lineage. */
    extern const ErrorCodeDescriptor SpawnRecursionDetected;
    /** @brief A runtime spawn lineage exceeds the configured depth bound. */
    extern const ErrorCodeDescriptor SpawnDepthExceeded;
    /** @brief Runtime spawn admission rejected a bounded request before mutation. */
    extern const ErrorCodeDescriptor AdmissionRejected;
    /** @brief A runtime prefab operation was cancelled before publication. */
    extern const ErrorCodeDescriptor Cancelled;
    /** @brief The target runtime scene is unavailable or has been replaced. */
    extern const ErrorCodeDescriptor SceneUnavailable;
    /** @brief A runtime spawn parent is stale, missing, or belongs to another scene. */
    extern const ErrorCodeDescriptor InvalidParent;
    /** @brief Runtime entity identity storage is exhausted. */
    extern const ErrorCodeDescriptor EntityAllocationExhausted;

    /** @brief A structured prefab diagnostic input or retained context is malformed. */
    extern const ErrorCodeDescriptor DiagnosticInvalid;
    /** @brief A diagnostic category or originating error is outside the prefab contract. */
    extern const ErrorCodeDescriptor DiagnosticUnsupported;
    /** @brief A diagnostic cause or dependency chain exceeded its explicit bound. */
    extern const ErrorCodeDescriptor DiagnosticBudgetExceeded;

    /** @brief Returns the complete canonical prefab error descriptor set. @return Stable process-lifetime descriptor view. */
    [[nodiscard]] std::span<const ErrorCodeDescriptor *const> Descriptors() noexcept;
}  // namespace Horo::Prefab::PrefabErrors
