#pragma once

/**
 * @file CinematicErrors.h
 * @brief Stable cinematic model errors independent of runtime and editor services.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Cinematic::CinematicErrors {
    /** @brief An identity contains a reserved stable value or generation. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief An identity does not name the requested authored object. */
    extern const ErrorCodeDescriptor IdentityUnknown;
    /** @brief An identity names an authored object with a retired generation. */
    extern const ErrorCodeDescriptor IdentityStale;
    /** @brief A generation cannot advance without wrapping into a reserved value. */
    extern const ErrorCodeDescriptor GenerationExhausted;
    /** @brief Canonical serialized identity bytes contain a reserved value. */
    extern const ErrorCodeDescriptor SerializedIdentityInvalid;
    /** @brief Sequence source bytes are malformed or contain an invalid field value. */
    extern const ErrorCodeDescriptor SequenceSchemaMalformed;
    /** @brief Sequence source contains a duplicate field, track, or dependency identity. */
    extern const ErrorCodeDescriptor SequenceSchemaDuplicate;
    /** @brief Sequence source uses a schema version that requires migration or a newer reader. */
    extern const ErrorCodeDescriptor SequenceSchemaVersionUnsupported;
    /** @brief Sequence source exceeds a compiled parser or schema safety ceiling. */
    extern const ErrorCodeDescriptor SequenceSchemaLimitExceeded;
    /** @brief Sequence content exceeds the selected cook tier. */
    extern const ErrorCodeDescriptor SequenceCookTierExceeded;
    /** @brief A referenced asset is absent from the exact cook snapshot. */
    extern const ErrorCodeDescriptor SequenceReferenceMissing;
    /** @brief A referenced asset move has not been reconciled into the exact cook snapshot. */
    extern const ErrorCodeDescriptor SequenceReferenceMoved;
    /** @brief A referenced asset exists but cannot be loaded for cooking. */
    extern const ErrorCodeDescriptor SequenceReferenceUnloadable;
    /** @brief A referenced asset does not have the domain type declared by its track. */
    extern const ErrorCodeDescriptor SequenceReferenceTypeMismatch;
    /** @brief The reachable sub-sequence graph contains a cycle. */
    extern const ErrorCodeDescriptor SequenceReferenceCycle;
    /** @brief Scalar curve keys, ordering, or policies are malformed. */
    extern const ErrorCodeDescriptor CurveMalformed;
    /** @brief Scalar curve key or tangent data contains a non-finite value. */
    extern const ErrorCodeDescriptor CurveNonFinite;
    /** @brief Scalar curve time tangents cannot form a valid bounded segment. */
    extern const ErrorCodeDescriptor CurveTangentInvalid;
    /** @brief Scalar curve input exceeds the compiled sampling capacity. */
    extern const ErrorCodeDescriptor CurveLimitExceeded;
    /** @brief Transform-track version is not directly compatible with this evaluator. */
    extern const ErrorCodeDescriptor TransformVersionUnsupported;
    /** @brief Transform track or scene generation no longer matches the active binding snapshot. */
    extern const ErrorCodeDescriptor TransformBindingStale;
    /** @brief A required transform binding or parent is absent from the activation snapshot. */
    extern const ErrorCodeDescriptor TransformBindingMissing;
    /** @brief Transform binding topology contains a parent cycle. */
    extern const ErrorCodeDescriptor TransformHierarchyCycle;
    /** @brief Transform-track data or its binding relationship is malformed. */
    extern const ErrorCodeDescriptor TransformMalformed;
    /** @brief Transform evaluation exceeds a compiled input or caller output bound. */
    extern const ErrorCodeDescriptor TransformLimitExceeded;
    /** @brief Sampled transform channels cannot form a finite scene transform. */
    extern const ErrorCodeDescriptor TransformSampleInvalid;
    /** @brief Property-track version is not directly compatible with this evaluator. */
    extern const ErrorCodeDescriptor PropertyVersionUnsupported;
    /** @brief A property binding identity is absent from the exact registry snapshot. */
    extern const ErrorCodeDescriptor PropertyBindingMissing;
    /** @brief Property activation attempted to consume a registry before it was frozen. */
    extern const ErrorCodeDescriptor PropertyRegistryUnfrozen;
    /** @brief A property target belongs to a retired scene or component generation. */
    extern const ErrorCodeDescriptor PropertyBindingStale;
    /** @brief A property target object or component instance is unavailable. */
    extern const ErrorCodeDescriptor PropertyBindingTargetMissing;
    /** @brief A property target component type does not match its binding descriptor. */
    extern const ErrorCodeDescriptor PropertyComponentMismatch;
    /** @brief A property track type or channel shape does not match its binding descriptor. */
    extern const ErrorCodeDescriptor PropertyTypeMismatch;
    /** @brief Property-track data or its target relationship is malformed. */
    extern const ErrorCodeDescriptor PropertyMalformed;
    /** @brief Property evaluation exceeds a compiled input or caller output bound. */
    extern const ErrorCodeDescriptor PropertyLimitExceeded;
    /** @brief Sampled property channels are non-finite or violate a binding constraint. */
    extern const ErrorCodeDescriptor PropertySampleInvalid;
    /** @brief A property owner rejected a typed write, so the track was skipped. */
    extern const ErrorCodeDescriptor PropertyWriteRejected;
}  // namespace Horo::Cinematic::CinematicErrors
