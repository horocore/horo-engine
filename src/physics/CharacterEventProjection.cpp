#include "CharacterEventProjectionInternal.h"

#include <cmath>
#include <tuple>

namespace Horo::Character::Detail {
    namespace {
        bool HasFlag(CharacterCollisionFlags value, CharacterCollisionFlags flag) noexcept {
            return (static_cast<std::uint16_t>(value) & static_cast<std::uint16_t>(flag)) != 0;
        }

        /** @brief A semantic transition requires comparable admitted provenance, never a physical revision heuristic. */
        bool SurfaceChanged(const CharacterEventPreviousState &previous, const CharacterMovementResult &current) noexcept {
            if (!previous.grounded || !current.grounded || !previous.groundSurface || !current.groundSurface)
                return false;
            const auto &before = *previous.groundSurface;
            const auto &after = *current.groundSurface;
            if (before.mappingGeneration != after.mappingGeneration || before.catalogRevision != after.catalogRevision ||
                before.catalogFingerprint != after.catalogFingerprint)
                return false;
            return before.surface != after.surface;
        }

        /** @brief The selected support is complete even when the retained contact prefix is full. */
        std::optional<CharacterSurfaceContact> GroundContact(const CharacterMovementResult &movement) noexcept {
            if (!movement.grounded || !movement.groundMaterial)
                return {};
            CharacterSurfaceContact contact;
            contact.body = movement.groundBody;
            contact.shape = movement.groundShape;
            contact.point = movement.groundPoint;
            contact.normal = movement.groundNormal;
            contact.material = *movement.groundMaterial;
            contact.subshape = movement.groundSubshape;
            contact.materialSource = movement.groundMaterialSource;
            contact.surface = movement.groundSurface;
            contact.surfaceSource = movement.groundSurfaceSource;
            return contact;
        }

        /** @brief Select from canonical retained blocking contacts without another Physics query. */
        std::optional<CharacterSurfaceContact> BlockingContact(const CharacterMovementResult &movement,
                                                               const CharacterControllerDescriptor &descriptor, bool ceiling) noexcept {
            const float limit = std::cos(descriptor.maximumSlopeDegrees * Math::Pi / 180.F);
            for (std::uint32_t index = 0; index < movement.contactCount; ++index) {
                const auto &contact = movement.contacts[index];
                const float up = Math::Dot(contact.normal, movement.up);
                const bool isCeiling = up < 0 && -up >= limit - 1.0e-5F;
                if (ceiling && isCeiling)
                    return contact;
                if (!ceiling && !isCeiling && up < limit)
                    return contact;
            }
            return {};
        }

        /** @brief Build one stable event kind/ordinal from the final controller publication. */
        void Append(CharacterEventProjection &projection, CharacterMovementEventKind kind, const CharacterLocomotionSnapshot &current,
                    std::optional<CharacterSurfaceContact> contact = {}) noexcept {
            auto &event = projection.events[projection.count++];
            event.controller = current.controller;
            event.tick = current.tick;
            event.sequence = current.movement.sequence;
            event.stateRevision = current.stateRevision;
            event.publicationRevision = current.transform.publicationRevision;
            event.ordinal = static_cast<std::uint64_t>(kind) + 1;
            event.kind = kind;
            event.contact = std::move(contact);
            event.achievedVelocityMetersPerSecond = current.movement.achievedVelocityMetersPerSecond;
            if (event.contact)
                event.impactVelocity = event.contact->impactVelocityMetersPerSecond;
        }

        void ProjectGround(CharacterEventProjection &events, const CharacterEventPreviousState &previous,
                           const CharacterLocomotionSnapshot &current) noexcept {
            const auto &movement = current.movement;
            if (!previous.grounded && movement.grounded)
                Append(events, CharacterMovementEventKind::Landed, current, GroundContact(movement));
            if (previous.grounded && !movement.grounded)
                Append(events, CharacterMovementEventKind::LeftGround, current);
            if (SurfaceChanged(previous, movement))
                Append(events, CharacterMovementEventKind::SurfaceChanged, current, GroundContact(movement));
        }

        void ProjectContacts(CharacterEventProjection &events, const CharacterEventPreviousState &previous,
                             const CharacterLocomotionSnapshot &current, const CharacterControllerDescriptor &descriptor) noexcept {
            using enum Horo::Character::CharacterCollisionFlags;
            const auto collisions = current.movement.collisions;
            if (!HasFlag(previous.collisions, Sides) && HasFlag(collisions, Sides))
                Append(events, CharacterMovementEventKind::HitWall, current, BlockingContact(current.movement, descriptor, false));
            if (!HasFlag(previous.collisions, Ceiling) && HasFlag(collisions, Ceiling))
                Append(events, CharacterMovementEventKind::HitCeiling, current, BlockingContact(current.movement, descriptor, true));
        }

        /** @brief Reconcile end states before ordinary contact hints when the bounded event prefix fills. */
        bool Critical(CharacterMovementEventKind kind) noexcept {
            using enum Horo::Character::CharacterMovementEventKind;
            return kind == Landed || kind == LeftGround || kind == SlideEnd;
        }
    }  // namespace

    CharacterEventProjection ProjectCharacterMovementEvents(const CharacterEventPreviousState &previous,
                                                            const CharacterLocomotionSnapshot &current,
                                                            const CharacterControllerDescriptor &descriptor) noexcept {
        CharacterEventProjection events{};
        ProjectGround(events, previous, current);
        ProjectContacts(events, previous, current, descriptor);
        if (!previous.sliding && current.movement.sliding)
            Append(events, CharacterMovementEventKind::SlideStart, current, BlockingContact(current.movement, descriptor, false));
        if (previous.sliding && !current.movement.sliding)
            Append(events, CharacterMovementEventKind::SlideEnd, current);
        return events;
    }

    CharacterEventTerminalState ProjectCharacterTerminalState(const CharacterLocomotionSnapshot &current) noexcept {
        const auto &movement = current.movement;
        return {current.controller,
                current.tick,
                movement.sequence,
                current.stateRevision,
                current.transform.publicationRevision,
                movement.grounded,
                movement.sliding,
                movement.collisions,
                movement.surfaceAvailability,
                movement.groundSurface};
    }

    bool CharacterMovementEventLess(const CharacterMovementEvent &left, const CharacterMovementEvent &right) noexcept {
        return std::tie(left.tick, left.controller, left.kind, left.ordinal) <
               std::tie(right.tick, right.controller, right.kind, right.ordinal);
    }

    bool CharacterMovementEventRetainBefore(const CharacterMovementEvent &left, const CharacterMovementEvent &right) noexcept {
        if (left.tick != right.tick)
            return left.tick > right.tick;
        if (Critical(left.kind) != Critical(right.kind))
            return Critical(left.kind);
        return CharacterMovementEventLess(left, right);
    }
}  // namespace Horo::Character::Detail
