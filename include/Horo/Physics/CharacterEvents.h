#pragma once
/** @file CharacterEvents.h @brief Bounded immutable post-commit Character movement facts and terminal-state projection. */
#include "Horo/Physics/CharacterControllerContracts.h"

#include <cstdint>
#include <optional>
#include <span>

namespace Horo::Character {
    /** @brief Closed physical state transitions; Character never emits animation footstep cadence. */
    enum class CharacterMovementEventKind : std::uint8_t {
        Landed,
        LeftGround,
        HitWall,
        HitCeiling,
        SurfaceChanged,
        SlideStart,
        SlideEnd
    };

    /** @brief One owned fact whose complete correlation tuple is its stable deduplication identity. */
    struct CharacterMovementEvent final {
        CharacterControllerHandle controller;
        std::uint64_t tick{}, sequence{}, stateRevision{}, publicationRevision{}, ordinal{};
        std::uint64_t streamPublicationRevision{}; /**< Committed world stream watermark, distinct from transform revision. */
        CharacterMovementEventKind kind{CharacterMovementEventKind::Landed};
        std::optional<CharacterSurfaceContact> contact;
        Math::Vec3 achievedVelocityMetersPerSecond{};
        std::optional<Math::Vec3> impactVelocity;
    };

    /** @brief Immutable canonical terminal projection, retained independently of event-prefix loss.
     * @details sourceTick/revisions remain the actual source publication, including on no-command stream ticks.
     */
    struct CharacterEventTerminalState final {
        CharacterControllerHandle controller;
        std::uint64_t sourceTick{}, sequence{}, stateRevision{}, publicationRevision{};
        bool grounded{}, sliding{};
        CharacterCollisionFlags collisions{CharacterCollisionFlags::None};
        CharacterSurfaceAvailability surfaceAvailability{CharacterSurfaceAvailability::Unavailable};
        std::optional<Physics::PhysicsSurfaceEvidence> groundSurface;
    };

    /** @brief Exact read fence and consumer watermark; repeated capture never consumes or emits a fact. */
    struct CharacterEventCaptureRequest final {
        std::uint64_t sceneGeneration{};
        CharacterWorldId world;
        std::uint64_t afterPublicationRevision{};
    };
    enum class CharacterEventCaptureStatus : std::uint8_t {
        Captured,
        NoNewPublication,
        CapacityLimited,
        NoPublication,
        WrongThread,
        Busy,
        Stale,
        Closed
    };

    /** @brief Bounded capture receipt; output spans are caller-owned copies and retain no world or callback. */
    struct CharacterEventCapture final {
        CharacterEventCaptureStatus status{CharacterEventCaptureStatus::NoPublication};
        std::uint64_t sceneGeneration{};
        CharacterWorldId world;
        Physics::PhysicsWorldId physicsWorld;
        std::uint64_t tick{}, publicationRevision{}, emittedEvents{};
        std::uint64_t droppedEvents{},
            cumulativeDroppedEvents{}; /**< Journal retention/eviction pressure, including already-read facts; not per-consumer loss. */
        std::uint32_t availableEvents{}, copiedEvents{}, availableTerminalStates{}, copiedTerminalStates{};
        std::uint64_t readDroppedEvents{}; /**< Unseen retained facts omitted only by this capture's event output quota. */
    };
}  // namespace Horo::Character
