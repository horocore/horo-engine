#pragma once

/**
 * @file PCGErrors.h
 * @brief Stable procedural-generation contract failures independent of runtime and target backends.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::PCG::PCGErrors {
    /** @brief A PCG identity contains a reserved value. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief A PCG identity belongs to a different graph or logical owner. */
    extern const ErrorCodeDescriptor IdentityUnknown;
    /** @brief A PCG identity belongs to a retired graph revision or execution. */
    extern const ErrorCodeDescriptor IdentityStale;
    /** @brief A graph revision cannot advance without reusing a previously issued value. */
    extern const ErrorCodeDescriptor RevisionExhausted;
    /** @brief Canonical serialized PCG identity bytes contain a reserved value. */
    extern const ErrorCodeDescriptor SerializedIdentityInvalid;
    /** @brief A point schema or attribute key is malformed or contains an unknown type. */
    extern const ErrorCodeDescriptor PointSchemaInvalid;
    /** @brief Two schema fields or storage columns use the same canonical key. */
    extern const ErrorCodeDescriptor PointAttributeDuplicate;
    /** @brief A storage column does not exist in the captured schema. */
    extern const ErrorCodeDescriptor PointAttributeUnknown;
    /** @brief A storage column has a different type than its schema field. */
    extern const ErrorCodeDescriptor PointAttributeTypeMismatch;
    /** @brief Point core or column data violates its finite/value/length contract. */
    extern const ErrorCodeDescriptor PointDataInvalid;
    /** @brief A tier point, attribute, payload, or memory ceiling was exceeded. */
    extern const ErrorCodeDescriptor PointCapacityExceeded;
    /** @brief Checked PCG size arithmetic overflowed or used invalid alignment. */
    extern const ErrorCodeDescriptor PointSizeOverflow;
    /** @brief A spatial snapshot, descriptor, transform, bound, or provenance value is malformed. */
    extern const ErrorCodeDescriptor SpatialInputInvalid;
    /** @brief The declared coordinate convention or precision is unsupported. */
    extern const ErrorCodeDescriptor SpatialCoordinatesUnsupported;
    /** @brief Required spatial coverage is missing or partial and therefore cannot prove an empty result. */
    extern const ErrorCodeDescriptor SpatialCoverageUnavailable;
    /** @brief A spatial element, grid, control-point, or byte ceiling was exceeded. */
    extern const ErrorCodeDescriptor SpatialCapacityExceeded;
    /** @brief A captured spatial revision or origin epoch is no longer current. */
    extern const ErrorCodeDescriptor SpatialSnapshotStale;
    /** @brief A replacement changes provider/source identity or fails to advance revision and snapshot identity. */
    extern const ErrorCodeDescriptor SpatialReplacementInvalid;
    /** @brief A PCG registry configuration, graph, or node-runtime descriptor is malformed. */
    extern const ErrorCodeDescriptor RegistryDescriptorInvalid;
    /** @brief A stable graph or node-runtime identity is already registered. */
    extern const ErrorCodeDescriptor RegistryDuplicate;
    /** @brief A bounded graph or node-runtime registry is full. */
    extern const ErrorCodeDescriptor RegistryCapacityExceeded;
    /** @brief A registry operation was attempted after composition shutdown. */
    extern const ErrorCodeDescriptor RegistryClosed;
    /** @brief A registry publication generation cannot advance without wrapping. */
    extern const ErrorCodeDescriptor RegistryGenerationExhausted;
    /** @brief A process-local registry handle is malformed. */
    extern const ErrorCodeDescriptor RegistryHandleInvalid;
    /** @brief A process-local registry handle belongs to another immutable snapshot generation. */
    extern const ErrorCodeDescriptor RegistryHandleStale;
    /** @brief The exact capability requested by a graph or runtime is not projected by the host. */
    extern const ErrorCodeDescriptor UnsupportedCapability;
    /** @brief An exact node runtime required by a registered graph is unavailable. */
    extern const ErrorCodeDescriptor RuntimeUnavailable;
    /** @brief A graph source envelope, field, value, or topology endpoint is malformed. */
    extern const ErrorCodeDescriptor GraphSourceMalformed;
    /** @brief A stable graph, node, pin, edge, exposed-input, or key identity is duplicated. */
    extern const ErrorCodeDescriptor GraphSourceDuplicate;
    /** @brief The graph source schema cannot be consumed by this implementation. */
    extern const ErrorCodeDescriptor GraphSourceVersionUnsupported;
    /** @brief The graph source exceeds a caller-selected or compiled safety ceiling. */
    extern const ErrorCodeDescriptor GraphSourceCapacityExceeded;
    /** @brief The graph has an illegal edge, pin association, cardinality, or cycle. */
    extern const ErrorCodeDescriptor GraphTopologyInvalid;
    /** @brief A node type is unavailable under the selected unknown-node policy. */
    extern const ErrorCodeDescriptor GraphNodeTypeUnknown;
    /** @brief A graph source migration was required but unavailable or invalid. */
    extern const ErrorCodeDescriptor GraphMigrationFailed;
    /** @brief A replacement changes graph identity or fails to advance its durable revision. */
    extern const ErrorCodeDescriptor GraphReplacementInvalid;
    /** @brief Graph-source work was rejected because cancellation or shutdown closed admission. */
    extern const ErrorCodeDescriptor GraphLifecycleUnavailable;
    /** @brief Pre-compile graph validation rejected source, registry, capability, or runtime evidence. */
    extern const ErrorCodeDescriptor GraphValidationFailed;
    /** @brief Pre-compile graph validation exceeded a finite work or diagnostic ceiling. */
    extern const ErrorCodeDescriptor GraphValidationCapacityExceeded;
    /** @brief A generation plan, output operation, receipt, or provenance tuple is malformed. */
    extern const ErrorCodeDescriptor GenerationPlanInvalid;
    /** @brief A generation plan exceeds its finite operation, dependency, or resource envelope. */
    extern const ErrorCodeDescriptor GenerationPlanCapacityExceeded;
    /** @brief A target generation or retained ownership record is no longer current. */
    extern const ErrorCodeDescriptor GenerationPlanStale;
    /** @brief An update or removal lacks an exact target-owned provenance authorization. */
    extern const ErrorCodeDescriptor GenerationOwnershipMismatch;
    /** @brief Generation-plan admission is closed by cancellation or shutdown. */
    extern const ErrorCodeDescriptor GenerationPlanLifecycleUnavailable;
    /** @brief A provenance stamp, identity, digest, or output is malformed. */
    extern const ErrorCodeDescriptor ProvenanceInvalid;
    /** @brief Provenance contains duplicate input, provider, or output identities. */
    extern const ErrorCodeDescriptor ProvenanceDuplicate;
    /** @brief A bounded provenance input or output ceiling was exceeded. */
    extern const ErrorCodeDescriptor ProvenanceCapacityExceeded;
    /** @brief A numeric tier, profile, or non-deterministic input cannot satisfy the requested promise. */
    extern const ErrorCodeDescriptor ProvenanceTierUnsupported;
    /** @brief Authoritative provenance changed since capture. */
    extern const ErrorCodeDescriptor ProvenanceStale;
    /** @brief Provenance admission is closed by cancellation or shutdown. */
    extern const ErrorCodeDescriptor ProvenanceLifecycleUnavailable;
    /** @brief An async operation, exact fence, scope, or callback is malformed. */
    extern const ErrorCodeDescriptor AsyncInvalid;
    /** @brief The async operation or registered scene/cell/graph scope is unknown. */
    extern const ErrorCodeDescriptor AsyncUnknown;
    /** @brief Async admission or publication is closed by invalidation or shutdown. */
    extern const ErrorCodeDescriptor AsyncClosed;
    /** @brief Captured source/content/authority evidence is no longer current. */
    extern const ErrorCodeDescriptor AsyncStale;
    /** @brief Async operation or scope retention exceeds its finite ceiling. */
    extern const ErrorCodeDescriptor AsyncCapacityExceeded;
    /** @brief An async owner-lane method was called from another thread. */
    extern const ErrorCodeDescriptor AsyncWrongThread;
    /** @brief The operation cannot retire while worker, child, or owner work remains. */
    extern const ErrorCodeDescriptor AsyncNotReady;
    /** @brief An owner-lane publication callback threw an exception. */
    extern const ErrorCodeDescriptor AsyncPublicationFailed;
    /** @brief A never-reused async operation identity cannot advance without wraparound. */
    extern const ErrorCodeDescriptor AsyncGenerationExhausted;
}  // namespace Horo::PCG::PCGErrors
