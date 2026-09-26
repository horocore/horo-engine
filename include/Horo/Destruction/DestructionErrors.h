#pragma once

/**
 * @file DestructionErrors.h
 * @brief Stable destruction contract failures independent of physics and rendering backends.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Destruction::DestructionErrors {
    /** @brief A destruction identity contains a reserved or incomplete representation. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief An identity belongs to a different asset, world, or destructible owner. */
    extern const ErrorCodeDescriptor IdentityUnknown;
    /** @brief An identity belongs to a retired runtime generation. */
    extern const ErrorCodeDescriptor StaleGeneration;
    /** @brief An identity names a replaced fracture content version. */
    extern const ErrorCodeDescriptor StaleContent;
    /** @brief An identity names a semantic state revision that is no longer current. */
    extern const ErrorCodeDescriptor StaleRevision;
    /** @brief A generation cannot advance without reusing a previously issued value. */
    extern const ErrorCodeDescriptor GenerationExhausted;
    /** @brief A revision cannot advance without reusing a previously issued value. */
    extern const ErrorCodeDescriptor RevisionExhausted;
    /** @brief Canonical serialized destruction identity bytes are malformed. */
    extern const ErrorCodeDescriptor SerializedIdentityInvalid;
    /** @brief A destructible descriptor contains malformed or contradictory policy. */
    extern const ErrorCodeDescriptor DescriptorInvalid;
    /** @brief A feature tier value is unknown to this descriptor contract. */
    extern const ErrorCodeDescriptor TierInvalid;
    /** @brief A required feature is unavailable in the exact selected tier. */
    extern const ErrorCodeDescriptor FeatureUnsatisfied;
    /** @brief Runtime geometry generation was requested from the core pre-cooked contract. */
    extern const ErrorCodeDescriptor RuntimeGeometryUnsupported;
    /** @brief A descriptor limit is zero, contradictory, above its tier, or above an engine ceiling. */
    extern const ErrorCodeDescriptor LimitProfileInvalid;
    /** @brief Exact cooked counts or peak cost exceed the descriptor's admitted limits. */
    extern const ErrorCodeDescriptor LimitExceeded;
    /** @brief A descriptor belongs to a replaced immutable configuration publication. */
    extern const ErrorCodeDescriptor StaleConfiguration;
    /** @brief A state snapshot or detached successor violates the canonical state-machine invariants. */
    extern const ErrorCodeDescriptor StateInvalid;
    /** @brief A damage command contains a zero, negative, or non-finite amount. */
    extern const ErrorCodeDescriptor InvalidDamage;
    /** @brief A new damage command arrived before the configured fixed-tick cooldown elapsed. */
    extern const ErrorCodeDescriptor DamageCooldownActive;
    /** @brief A reused command identity carries different revision, kind, or payload semantics. */
    extern const ErrorCodeDescriptor DuplicateCommand;
    /** @brief A command attempted to mutate the terminal Destroyed state. */
    extern const ErrorCodeDescriptor StateTerminal;
    /** @brief Detached transition work was cancelled before owner-safe publication. */
    extern const ErrorCodeDescriptor CancelledBeforeCommit;
    /** @brief The state owner has closed mutation and replacement admission for shutdown. */
    extern const ErrorCodeDescriptor ShutdownInProgress;
    /** @brief A damage/fracture command contains malformed identity, schema, enum, vector, or tick data. */
    extern const ErrorCodeDescriptor CommandInvalid;
    /** @brief A finite command payload exceeds the explicitly admitted per-command limits. */
    extern const ErrorCodeDescriptor CommandLimitExceeded;
    /** @brief The submitted authority grant is invalid, stale, or lacks the required typed capability. */
    extern const ErrorCodeDescriptor CommandAuthorityDenied;
    /** @brief The descriptor or immutable capability snapshot does not support the requested command kind. */
    extern const ErrorCodeDescriptor CommandUnsupported;
    /** @brief A durable command terminal result contains an impossible outcome/reason/revision combination. */
    extern const ErrorCodeDescriptor CommandResultInvalid;
    /** @brief Registry limits, records, query filters, or capability projections are malformed. */
    extern const ErrorCodeDescriptor RegistryInvalid;
    /** @brief A registry already contains the exact target or authored owner. */
    extern const ErrorCodeDescriptor RegistryDuplicate;
    /** @brief A registry or query would exceed its explicit finite capacity. */
    extern const ErrorCodeDescriptor RegistryCapacityExceeded;
    /** @brief A destruction product composition has malformed schema, profile, facts, or lifecycle evidence. */
    extern const ErrorCodeDescriptor CompositionInvalid;
    /** @brief An exact product profile requires a capability the host reported unavailable. */
    extern const ErrorCodeDescriptor CompositionCapabilityUnavailable;
    /** @brief Work references a replaced immutable destruction composition revision. */
    extern const ErrorCodeDescriptor CompositionStale;
}  // namespace Horo::Destruction::DestructionErrors
