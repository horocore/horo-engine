#pragma once

/** @file TacticalCover.h
 * @brief Gameplay ownership of tactical cover and explicit reservations at Scene safe points.
 */

#include "Horo/AI/EnvironmentQueryScoring.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace Horo::Gameplay {
    /** @brief Stable authored identity distinct from the anchor's runtime generation. */
    using TacticalCoverId = AI::AiStableIdentity<struct TacticalCoverIdentityTag>;
    /** @brief Process-local provider incarnation. Never part of an authored cover asset. */
    using TacticalCoverProviderGeneration = AI::AiStableIdentity<struct TacticalCoverProviderGenerationTag>;

    /** @brief Stance for which the authored point offers cover. */
    enum class TacticalCoverStance : std::uint8_t {
        Crouched,
        Standing,
        Count
    };

    /** @brief Complete authored cover descriptor copied during structural publication. */
    struct TacticalCoverPoint final {
        TacticalCoverId id;
        Runtime::EntityRef anchor; /**< Exact Scene entity generation owning the point. */
        Math::WorldCoordinate64 position;
        Math::Vec3 direction; /**< Unit vector toward the protected threat sector. */
        TacticalCoverStance stance{};
        bool operator==(const TacticalCoverPoint &) const = default;
    };

    /** @brief Historical selection evidence fenced by the anchor and descriptor generation. */
    struct TacticalCoverHandle final {
        TacticalCoverId id;
        Runtime::EntityRef anchor;
        std::uint64_t revision{};
        TacticalCoverProviderGeneration provider;
        bool operator==(const TacticalCoverHandle &) const = default;
    };

    /** @brief Read-only candidate handed to EQS without acquiring occupancy. */
    struct TacticalCoverCandidate final {
        TacticalCoverPoint point;
        TacticalCoverHandle handle;
    };

    /** @brief Exact reservation receipt. Late releases cannot clear a newer reservation. */
    struct TacticalCoverReservation final {
        TacticalCoverHandle cover;
        Runtime::EntityRef occupant;
        std::uint64_t sequence{};
        bool operator==(const TacticalCoverReservation &) const = default;
    };

    /** @brief Owner-lane, generation-fenced provider for one Scene's cover point inventory.
     * @details Operations require the construction thread at Scene/decision safe points. A retained selection or
     * receipt is an immutable value. The provider does not retain any Scene view or backend object.
     */
    class TacticalCoverRegistry final {
    public:
        static constexpr std::size_t MaximumPoints = 4096;
        TacticalCoverRegistry() noexcept;
        TacticalCoverRegistry(const TacticalCoverRegistry &) = delete;
        TacticalCoverRegistry &operator=(const TacticalCoverRegistry &) = delete;

        /** @brief Publishes the complete resident inventory in stable ID order.
         * @param scene Fresh Scene view. @param revision Increasing cover publication revision.
         * @param points Complete bounded descriptors; all anchors must be live.
         * @return Success or typed failure preserving the prior inventory. Unchanged points retain reservations.
         */
        [[nodiscard]] Result<void> Publish(const Runtime::RuntimeSceneView &scene, std::uint64_t revision,
                                           std::span<const TacticalCoverPoint> points);
        /** @brief Copies currently resident candidates for EQS in stable identity order.
         * @param scene Fresh Scene view. @param maximumItems Admitted query result cap.
         * @return Bounded candidates or typed stale/capacity failure. No occupancy is changed.
         */
        [[nodiscard]] Result<std::vector<TacticalCoverCandidate>> Capture(const Runtime::RuntimeSceneView &scene,
                                                                          std::size_t maximumItems) const;
        /** @brief Reads one exact resident candidate without copying the inventory or changing occupancy. */
        [[nodiscard]] Result<TacticalCoverCandidate> Resolve(const Runtime::RuntimeSceneView &scene, TacticalCoverHandle cover) const;
        /** @brief Combines existing Navigation and Physics facts with current Gameplay contention.
         * @param scene Fresh Scene view. @param cover Exact candidate sampled by both adapters.
         * @param reachability Navigation sample. @param threatVisibility Physics clear/blocked sample, with clear indicating exposure.
         * @param contentionStage Authored contention stage. @return Facts in stage-ID order for the existing scoring plan,
         * or a typed stale/invalid outcome. Navigation/Physics unavailability remains a typed sample.
         */
        [[nodiscard]] Result<std::array<AI::QueryTestSample, 3>> Score(const Runtime::RuntimeSceneView &scene, TacticalCoverHandle cover,
                                                                       AI::QueryTestSample reachability,
                                                                       AI::QueryTestSample threatVisibility,
                                                                       AI::QueryStageId contentionStage) const;
        /** @brief Requests occupancy explicitly after a query has selected cover.
         * @param scene Fresh Scene view. @param cover Exact current selection. @param occupant Live requesting entity.
         * @return Receipt, an empty result on contention, or a typed stale/invalid failure.
         */
        [[nodiscard]] Result<std::optional<TacticalCoverReservation>> TryReserve(const Runtime::RuntimeSceneView &scene,
                                                                                 TacticalCoverHandle cover, Runtime::EntityRef occupant);
        /** @brief Releases only the matching receipt. @param receipt Exact accepted reservation.
         * @return Whether this call cleared a reservation.
         */
        bool Release(TacticalCoverReservation receipt) noexcept;

    private:
        struct Entry final {
            TacticalCoverCandidate candidate;
            std::optional<TacticalCoverReservation> reservation;
        };

        /** @brief Finds the exact incarnation of one authored point. */
        [[nodiscard]] std::vector<Entry>::const_iterator Find(TacticalCoverHandle cover) const noexcept;
        /** @brief Verifies the owner's current Scene publication. */
        [[nodiscard]] bool IsCurrent(const Runtime::RuntimeSceneView &scene) const noexcept;
        /** @brief Checks that the retained receipt still belongs to a live occupant. */
        [[nodiscard]] static bool IsOccupied(const Runtime::RuntimeSceneView &scene, const Entry &entry);
        [[nodiscard]] bool OnOwnerLane() const noexcept;
        const std::thread::id owner_{std::this_thread::get_id()};
        const TacticalCoverProviderGeneration provider_;
        Runtime::SceneRuntimeId scene_;
        std::uint64_t revision_{};
        std::uint64_t nextReservation_{};
        std::vector<Entry> points_;
    };
}  // namespace Horo::Gameplay
