#pragma once

/** @file CharacterDebugSnapshot.h
 * @brief Owned, bounded Character visualization evidence without simulation or backend authority.
 */

#include "Horo/Physics/CharacterControllerContracts.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace Horo::Character {
    class CharacterWorld;

    /** @brief Absolute copied-probe ceiling; world settings may admit a smaller retained prefix. */
    inline constexpr std::uint32_t MaximumCharacterDebugProbes = 32;

    /** @brief Actual operation that submitted copied capsule geometry, not a reconstructed visualization. */
    enum class CharacterDebugProbePurpose : std::uint8_t {
        Movement,
        Ground,
        Step,
        ShapeClearance,
        SpawnRecovery,
        TeleportRecovery,
    };

    /** @brief Closed geometry vocabulary independent from Physics backend collectors. */
    enum class CharacterDebugProbeKind : std::uint8_t {
        Sweep,
        Overlap
    };

    /** @brief One completed query's owned request geometry and bounded inventory counts. */
    struct CharacterDebugProbe final {
        CharacterDebugProbePurpose purpose{};
        CharacterDebugProbeKind kind{};
        Physics::PhysicsCapsuleShape capsule;
        Math::Vec3 position{};
        Math::Vec3 up{};
        Math::Vec3 direction{}; /**< Zero for overlap; no synthetic sweep or step arc. */
        float distanceMeters{};
        std::uint32_t iteration{};
        std::uint32_t reportedHits{};
        bool inventoryTruncated{};
        CharacterCollisionSelectors selectors; /**< Exact per-query filter evidence, including a staged filter change. */
        Physics::CollisionProfileId collisionProfile;
        Physics::PhysicsQueryChannelId queryChannel;
    };

    /** @brief Availability of actual retained probes; missing producer evidence is never fabricated. */
    enum class CharacterDebugProbeAvailability : std::uint8_t {
        Complete,
        CapacityLimited,
        UnsupportedProvider,
        StorageUnavailable,
    };

    /** @brief Allocation-free capture result identity; none of these outcomes modifies simulation. */
    enum class CharacterDebugCaptureStatus : std::uint8_t {
        Captured,
        CapacityLimited,
        InvalidRequest,
        WrongThread,
        Busy,
        Retired,
        ForeignGeneration,
        StaleController,
        NoPublication,
        AgeExceeded,
    };

    /** @brief Explicit consumer fences and prefix capacities for one owner-thread capture. */
    struct CharacterDebugCaptureRequest final {
        CharacterControllerHandle controller;
        Physics::PhysicsWorldId physicsWorld;
        std::uint64_t collisionFilterGeneration{};
        std::uint64_t originGeneration{};
        std::uint64_t maximumAgeTicks{std::numeric_limits<std::uint64_t>::max()};
        std::uint32_t maximumProbes{MaximumCharacterDebugProbes};
        std::uint32_t maximumContacts{MaximumCharacterContacts};
    };

    /** @brief Exact publication age and source fences, independent from presentation wall time. */
    struct CharacterDebugSnapshotIdentity final {
        CharacterControllerHandle controller;
        Physics::PhysicsWorldId physicsWorld;
        std::uint64_t collisionFilterGeneration{};
        std::uint64_t originGeneration{};
        std::uint64_t physicsSnapshotRevision{}; /**< Source publication's captured query revision, not the latest revision. */
        std::uint64_t observationTick{};
        std::uint64_t sourceTick{};
        std::uint64_t ageTicks{};
        std::uint64_t worldPublicationRevision{};
        std::uint64_t controllerStateRevision{};
        std::uint64_t transformPublicationRevision{};
    };

    /** @brief Immutable detached value; getters retain no registry, native query, world or caller borrow.
     *
     * Copies may be consumed on any thread and survive shutdown/replacement. This is debug evidence,
     * never canonical restore state, a query capability or permission to publish a transform.
     */
    class CharacterDebugSnapshot final {
    public:
        CharacterDebugSnapshot(const CharacterDebugSnapshot &) = default;
        CharacterDebugSnapshot(CharacterDebugSnapshot &&) = default;
        CharacterDebugSnapshot &operator=(const CharacterDebugSnapshot &) = default;
        CharacterDebugSnapshot &operator=(CharacterDebugSnapshot &&) = default;

        /** @brief Returns copied generation and tick evidence. @return Borrow owned by this snapshot. */
        [[nodiscard]] const CharacterDebugSnapshotIdentity &Identity() const noexcept {
            return identity_;
        }

        /** @brief Returns actual committed capsule geometry. @return Borrow owned by this snapshot. */
        [[nodiscard]] const Physics::PhysicsCapsuleShape &Capsule() const noexcept {
            return capsule_;
        }

        /** @brief Returns the authoritative root publication. @return Borrow owned by this snapshot. */
        [[nodiscard]] const CharacterTransformPublication &Transform() const noexcept {
            return transform_;
        }

        /** @brief Returns optional committed movement, ground and platform evidence.
         * @return Owned optional; absent after spawn/teleport until movement publishes again.
         */
        [[nodiscard]] const std::optional<CharacterLocomotionSnapshot> &Locomotion() const noexcept {
            return locomotion_;
        }

        /** @brief Returns only the retained completed-query prefix. @return Snapshot-owned copied probe span. */
        [[nodiscard]] std::span<const CharacterDebugProbe> Probes() const noexcept {
            return std::span{probes_}.first(probeCount_);
        }

        /** @brief Reports actual producer/storage coverage. @return Closed probe availability value. */
        [[nodiscard]] CharacterDebugProbeAvailability ProbeAvailability() const noexcept {
            return probeAvailability_;
        }

        /** @brief Returns the number of actual probes omitted by storage or consumer capacity. @return Saturating count. */
        [[nodiscard]] std::uint32_t OmittedProbes() const noexcept {
            return omittedProbes_;
        }

        /** @brief Returns contact-prefix omission independently from solver constraints. @return Omitted contact count. */
        [[nodiscard]] std::uint32_t OmittedContacts() const noexcept {
            return omittedContacts_;
        }

        /** @brief Returns the world's preparation-fixed per-controller probe retention bound.
         * @return Candidate and committed prefixes each have this capacity; consumer limits may retain fewer.
         */
        [[nodiscard]] std::uint32_t ProbeRetentionCapacity() const noexcept {
            return probeRetentionCapacity_;
        }

    private:
        friend class CharacterWorld;
        CharacterDebugSnapshot() = default;
        CharacterDebugSnapshotIdentity identity_;
        Physics::PhysicsCapsuleShape capsule_;
        CharacterTransformPublication transform_;
        std::optional<CharacterLocomotionSnapshot> locomotion_;
        std::array<CharacterDebugProbe, MaximumCharacterDebugProbes> probes_{};
        std::uint32_t probeCount_{};
        std::uint32_t omittedProbes_{};
        std::uint32_t omittedContacts_{};
        std::uint32_t probeRetentionCapacity_{};
        CharacterDebugProbeAvailability probeAvailability_{CharacterDebugProbeAvailability::UnsupportedProvider};
    };

    /** @brief Capture outcome owns a snapshot only for Captured or CapacityLimited, never borrowed mutable state. */
    struct CharacterDebugCapture final {
        CharacterDebugCaptureStatus status{CharacterDebugCaptureStatus::NoPublication};
        std::optional<CharacterDebugSnapshot> snapshot;
    };
}  // namespace Horo::Character
