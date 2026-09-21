#pragma once

/**
 * @file VfxErrors.h
 * @brief Stable VFX identity and quality-resolution errors independent of simulation and rendering backends.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Vfx::VfxErrors {
    /** @brief An identity contains a reserved scope, slot, or generation value. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief An identity does not name the requested owner slot. */
    extern const ErrorCodeDescriptor IdentityUnknown;
    /** @brief An identity names a known slot with a retired generation. */
    extern const ErrorCodeDescriptor IdentityStale;
    /** @brief A generation cannot advance without wrapping into a reserved value. */
    extern const ErrorCodeDescriptor GenerationExhausted;
    /** @brief Canonical serialized identity bytes are malformed. */
    extern const ErrorCodeDescriptor SerializedIdentityInvalid;
    /** @brief Capability evidence is malformed, incomplete, or contradictory. */
    extern const ErrorCodeDescriptor CapabilityDataInvalid;
    /** @brief A quality policy contains an invalid profile, revision, budget, or threshold. */
    extern const ErrorCodeDescriptor QualityPolicyInvalid;
    /** @brief An effect requirement or authored fallback variant is malformed. */
    extern const ErrorCodeDescriptor RequirementInvalid;
    /** @brief Authored simulation intent conflicts with a gameplay-mandatory CPU path. */
    extern const ErrorCodeDescriptor DomainConflict;
    /** @brief No permitted path satisfies the required capability facts. */
    extern const ErrorCodeDescriptor UnsupportedCapability;
    /** @brief A required CPU or GPU simulation kernel is absent. */
    extern const ErrorCodeDescriptor MissingKernel;
    /** @brief No authored compatible fallback variant can satisfy the request. */
    extern const ErrorCodeDescriptor MissingVariant;
    /** @brief A count, memory, or work request exceeds an effective finite limit. */
    extern const ErrorCodeDescriptor LimitExceeded;
    /** @brief A prepared decision references a retired capability revision. */
    extern const ErrorCodeDescriptor CapabilityRevisionStale;
    /** @brief A prepared decision references a retired quality-policy revision. */
    extern const ErrorCodeDescriptor QualityPolicyRevisionStale;
    /** @brief Particle-system source bytes or field shapes are malformed. */
    extern const ErrorCodeDescriptor ParticleDescriptorMalformed;
    /** @brief Particle-system source contains an ambiguous duplicate JSON field. */
    extern const ErrorCodeDescriptor ParticleDescriptorDuplicate;
    /** @brief Particle-system source requires migration or a newer reader. */
    extern const ErrorCodeDescriptor ParticleDescriptorVersionUnsupported;
    /** @brief Particle-system source exceeds a compiled parser or semantic ceiling. */
    extern const ErrorCodeDescriptor ParticleDescriptorLimitExceeded;
    /** @brief A particle numeric range contains non-finite or inverted values. */
    extern const ErrorCodeDescriptor ParticleRangeInvalid;
    /** @brief An infinite-lifetime particle has no independent terminal condition. */
    extern const ErrorCodeDescriptor ParticleLifetimeUnbounded;
    /** @brief Particle render, collision, kill, or simulation policies conflict. */
    extern const ErrorCodeDescriptor ParticleModeIncompatible;
    /** @brief The particle material is absent from the exact cook snapshot. */
    extern const ErrorCodeDescriptor ParticleMaterialMissing;
    /** @brief The particle material identity resolves to a non-material asset. */
    extern const ErrorCodeDescriptor ParticleMaterialTypeMismatch;
    /** @brief The particle material cannot be loaded from the exact cook snapshot. */
    extern const ErrorCodeDescriptor ParticleMaterialUnloadable;
    /** @brief The particle descriptor exceeds the explicitly selected cook tier. */
    extern const ErrorCodeDescriptor ParticleCookTierExceeded;
    /** @brief A stable particle simulation identity is invalid or not strictly monotonic. */
    extern const ErrorCodeDescriptor ParticleSimulationIdentityInvalid;
    /** @brief CPU particle storage configuration is zero, excessive, or arithmetically unsafe. */
    extern const ErrorCodeDescriptor ParticleBufferInvalid;
    /** @brief CPU particle storage could not be allocated during explicit preparation. */
    extern const ErrorCodeDescriptor ParticleBufferAllocationFailed;
    /** @brief No reusable CPU particle slot remains. */
    extern const ErrorCodeDescriptor ParticleBufferCapacityExceeded;
    /** @brief A CPU particle operation occurred on a non-owning thread. */
    extern const ErrorCodeDescriptor ParticleBufferThreadViolation;
    /** @brief A CPU particle operation targeted a shut-down buffer. */
    extern const ErrorCodeDescriptor ParticleBufferShutDown;
    /** @brief A CPU particle handle is malformed or belongs to another buffer. */
    extern const ErrorCodeDescriptor ParticleHandleInvalid;
    /** @brief A CPU particle handle names a retired or reused slot generation. */
    extern const ErrorCodeDescriptor ParticleHandleStale;
    /** @brief CPU particle spawn-step input or prepared limits are malformed. */
    extern const ErrorCodeDescriptor ParticleSpawnStepInvalid;
    /** @brief A CPU particle spawn step was cancelled before mutation. */
    extern const ErrorCodeDescriptor ParticleSpawnStepCancelled;
    /** @brief The stable particle spawn ordinal cannot advance without wrapping. */
    extern const ErrorCodeDescriptor ParticleSpawnOrdinalExhausted;
    /** @brief A stable survivor list is malformed or not in current dense order. */
    extern const ErrorCodeDescriptor ParticleStableCompactionInvalid;
    /** @brief A simulation stage callback or stage sentinel violated the fixed contract. */
    extern const ErrorCodeDescriptor ParticleStageContractViolation;
    /** @brief Gameplay attempted to access a private or mutable simulation payload. */
    extern const ErrorCodeDescriptor ParticleGameplayAccessDenied;
    /** @brief A gameplay payload channel does not match its prepared schema. */
    extern const ErrorCodeDescriptor ParticlePayloadSchemaMismatch;
    /** @brief A committed-generation read is stale. */
    extern const ErrorCodeDescriptor ParticleGenerationStale;
    /** @brief A mandatory step request cannot fit the prepared capacity. */
    extern const ErrorCodeDescriptor ParticleStepCapacityExceeded;
    /** @brief A simulation step was cancelled before its candidate generation committed. */
    extern const ErrorCodeDescriptor ParticleSimulationStepCancelled;
    /** @brief A required typed collision query seam is not available. */
    extern const ErrorCodeDescriptor ParticleCollisionQueryUnavailable;
    /** @brief A typed collision query seam rejected or failed the request. */
    extern const ErrorCodeDescriptor ParticleCollisionQueryFailed;
    /** @brief CPU simulation preparation inputs are malformed or exceed fixed limits. */
    extern const ErrorCodeDescriptor ParticleSimulationDescriptorInvalid;
}  // namespace Horo::Vfx::VfxErrors
