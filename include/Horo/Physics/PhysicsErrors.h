#pragma once

/** @file PhysicsErrors.h
 * @brief Stable Horo physics error identities, never native solver codes or messages.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Physics::PhysicsErrors {
    /** @brief Missing or invalid published-world identity. */
    extern const ErrorCodeDescriptor WorldInvalid;
    /** @brief Missing world, invalid slot index or zero slot generation. */
    extern const ErrorCodeDescriptor HandleMalformed;
    /** @brief A well-formed handle belongs to a different world generation. */
    extern const ErrorCodeDescriptor HandleWorldMismatch;
    /** @brief The owning registry found an absent, retired or replaced slot generation. */
    extern const ErrorCodeDescriptor HandleStale;
    /** @brief A registry cannot issue another generation without reusing an old identity. */
    extern const ErrorCodeDescriptor GenerationExhausted;
    /** @brief Required Physics functionality is absent from the admitted composition. */
    extern const ErrorCodeDescriptor CapabilityUnavailable;
    /** @brief An operation is not supported by the selected qualified Physics profile. */
    extern const ErrorCodeDescriptor OperationUnsupported;
    /** @brief The world lifecycle phase cannot admit the requested operation. */
    extern const ErrorCodeDescriptor InvalidState;
    /** @brief A mutable Physics operation was attempted outside its owning thread. */
    extern const ErrorCodeDescriptor ThreadAffinityViolation;
    /** @brief Solver child work exceeded its fixed-tick deadline and was cooperatively cancelled. */
    extern const ErrorCodeDescriptor SolverDeadlineExceeded;
    /** @brief Malformed or unsupported-version descriptor metadata. */
    extern const ErrorCodeDescriptor DescriptorInvalid;
    /** @brief A command lacks complete canonical identity/order evidence or duplicates an admitted key. */
    extern const ErrorCodeDescriptor CommandOrderInvalid;
    /** @brief A seed policy, named stream or explicit consumption tuple is malformed or unsupported. */
    extern const ErrorCodeDescriptor SeedPolicyInvalid;
    /** @brief Unknown or unsupported numerical/solver profile. */
    extern const ErrorCodeDescriptor ProfileUnsupported;
    /** @brief Requested capacity exceeds the bounded profile or lacks its required budget. */
    extern const ErrorCodeDescriptor CapacityExceeded;
    /** @brief Admission evidence changed after the caller captured its capability revision. */
    extern const ErrorCodeDescriptor CapabilityStale;
    /** @brief A query targets a retired scene or filter/broadphase snapshot generation. */
    extern const ErrorCodeDescriptor QuerySnapshotStale;
    /** @brief Candidate or process initialization failed after releasing acquired resources. */
    extern const ErrorCodeDescriptor InitializationFailed;
    /** @brief Convex or mesh source geometry is malformed before solver cooking. */
    extern const ErrorCodeDescriptor ShapeCookSourceInvalid;
    /** @brief Shape cooking exceeded an explicit source, output or payload limit. */
    extern const ErrorCodeDescriptor ShapeCookLimitExceeded;
    /** @brief Shape cooking was cooperatively cancelled before publication. */
    extern const ErrorCodeDescriptor ShapeCookCancelled;
    /** @brief A cooked Physics shape artifact is malformed or fails integrity validation. */
    extern const ErrorCodeDescriptor ShapeArtifactInvalid;
}  // namespace Horo::Physics::PhysicsErrors
