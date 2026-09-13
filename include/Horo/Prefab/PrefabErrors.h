#pragma once

/**
 * @file PrefabErrors.h
 * @brief Stable prefab identity, authoring document and dependency graph errors.
 */

#include "Horo/Foundation/ErrorCode.h"

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
}  // namespace Horo::Prefab::PrefabErrors
