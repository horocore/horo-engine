#pragma once
#include "Horo/Physics/CharacterControllerContracts.h"

namespace Horo::Character::Detail {
    /** @brief Admit only an explicit catalog-valid default; empty legacy capture keeps semantic capability unavailable. */
    [[nodiscard]] inline Result<void> ValidateCharacterSemanticDefault(const Physics::PhysicsSurfaceWorldBinding &binding,
                                                                       const std::optional<Physics::SurfaceMaterialId> &surface) {
        if (surface && (!binding.catalog || !binding.catalog->Contains(*surface)))
            return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid));
        if (binding.catalog && !binding.catalog->Entries().empty() && !surface)
            return Result<void>::Failure(MakeError(CharacterErrors::CapabilityUnavailable));
        return Result<void>::Success();
    }

    /** @brief Validate copied query meaning against the actual immutable world capture before movement can commit. */
    [[nodiscard]] inline bool HasCurrentCharacterSurface(const Physics::PhysicsSurfaceWorldBinding &binding,
                                                         const std::optional<Physics::PhysicsSurfaceEvidence> &surface) noexcept {
        return !surface || Physics::MatchesPhysicsSurfaceEvidence(binding, *surface);
    }

    /** @brief Reject stale or unbacked semantic query values before reducing any candidate contact. */
    [[nodiscard]] inline Result<void> ValidateCharacterSurfaceProbe(const Physics::PhysicsSurfaceWorldBinding &binding,
                                                                    const CharacterSweepProbeResult &probe) {
        for (std::uint32_t index = 0; index < probe.hitCount; ++index) {
            const auto &hit = probe.hits[index];
            if (!hit.surface)
                continue;
            if (!hit.material || !Physics::MatchesPhysicsSurfaceEvidence(binding, *hit.surface))
                return Result<void>::Failure(MakeError(CharacterErrors::QuerySnapshotStale));
        }
        return Result<void>::Success();
    }

    /** @brief Fence observer-provided support and every retained contact to the same immutable world capture. */
    [[nodiscard]] inline Result<void> ValidateCharacterMovementSurfaces(const Physics::PhysicsSurfaceWorldBinding &binding,
                                                                        const CharacterMovementResult &movement) {
        if (!HasCurrentCharacterSurface(binding, movement.groundSurface))
            return Result<void>::Failure(MakeError(CharacterErrors::QuerySnapshotStale));
        for (std::uint32_t index{}; index < movement.contactCount; ++index)
            if (!HasCurrentCharacterSurface(binding, movement.contacts[index].surface))
                return Result<void>::Failure(MakeError(CharacterErrors::QuerySnapshotStale));
        return Result<void>::Success();
    }

    /** @brief Apply an admitted explicit policy to unmapped contacts, retaining query-provided meaning. */
    inline void FinalizeCharacterContactSurfaces(const Physics::PhysicsSurfaceWorldBinding &binding,
                                                 const CharacterControllerDescriptor &descriptor,
                                                 CharacterMovementResult &movement) noexcept {
        if (!descriptor.defaultSurface)
            return;
        for (std::uint32_t index{}; index < movement.contactCount; ++index) {
            auto &contact = movement.contacts[index];
            if (contact.surface)
                continue;
            contact.surface = Physics::MakePhysicsSurfaceEvidence(binding, *descriptor.defaultSurface);
            contact.surfaceSource = CharacterSemanticSurfaceSource::DescriptorDefault;
        }
    }

    /** @brief Resolve only an explicit captured semantic default; airborne state retains no invented support. */
    inline void FinalizeCharacterSurface(const Physics::PhysicsSurfaceWorldBinding &binding,
                                         const CharacterControllerDescriptor &descriptor, CharacterMovementResult &movement) noexcept {
        FinalizeCharacterContactSurfaces(binding, descriptor, movement);
        movement.surfaceAvailability =
            descriptor.defaultSurface ? CharacterSurfaceAvailability::Available : CharacterSurfaceAvailability::Unavailable;
        if (!movement.grounded)
            return;
        if (movement.groundSurface) {
            movement.surfaceAvailability = CharacterSurfaceAvailability::Available;
            movement.groundSurfaceSource = CharacterSemanticSurfaceSource::Query;
            return;
        }
        if (descriptor.defaultSurface) {
            movement.groundSurface = Physics::MakePhysicsSurfaceEvidence(binding, *descriptor.defaultSurface);
            movement.groundSurfaceSource = CharacterSemanticSurfaceSource::DescriptorDefault;
        }
    }

}  // namespace Horo::Character::Detail
