#pragma once

/** @file PerceptionSight.h
 * @brief Scene-owned bounded sight evaluation through canonical Physics query batches.
 */

#include "Horo/AI/PerceptionSpatialBroadphase.h"
#include "Horo/Physics/PhysicsQueryEventCapability.h"

#include <optional>

namespace Horo::AI {
    /** @brief One listener's immutable range, target-point and visibility hysteresis policy. */
    struct PerceptionSightPolicy final {
        std::uint64_t radiusMillimeters{50'000};     /**< At most 980,000 mm; reserves broadphase room for aim offsets. */
        double halfAngleRadians{0.7853981633974483}; /**< Inclusive cone half angle in [0, pi]. */
        double maximumHeightMeters{50};              /**< Inclusive vertical separation from the eye point. */
        Math::Vec3 eyeOffsetMeters{};                /**< Eye offset with length at most 10 meters. */
        Math::Vec3 targetOffsetMeters{};             /**< Aim offset with length at most 10 meters, relative to the declared source. */
        std::uint32_t gainSamples{1};                /**< Consecutive confirmed samples required to acquire visibility. */
        std::uint32_t lossSamples{2};                /**< Consecutive confirmed misses required to lose visibility. */

        [[nodiscard]] bool operator==(const PerceptionSightPolicy &) const noexcept = default;
    };

    /** @brief Host-owned shared per-tick budget; pass the same instance to all sight listeners. */
    struct PerceptionSightBudget final {
        std::uint64_t simulationTick{};
        std::size_t remainingRaycasts{}; /**< Aggregate LOS allowance remaining in this fixed tick. */
    };

    /** @brief Exact host-composed scene/Physics binding and listener pose for a sensing window. */
    struct PerceptionSightFrame final {
        Runtime::EntityRef listener;
        Math::Vec3 forward{0, 0, -1};          /**< Finite unit vector in canonical Y-up world axes. */
        Math::WorldCoordinate64 physicsOrigin; /**< Physics local origin; listener lies within 1024 meters on each axis. */
        std::uint64_t physicsSceneGeneration{};
        std::uint64_t physicsPublicationRevision{};
        std::uint64_t physicsCompletedTick{};
        Physics::PhysicsQueryFilter occlusionFilter; /**< Dedicated visibility channel; normally excludes listener body. */
        std::uint64_t visibleLayers{1};
        PerceptionAffiliationFilter affiliationFilter{PerceptionAffiliationFilter::Any};
        std::uint64_t affiliation{};
    };

    /** @brief Current host binding used to fence copied terminal results after world/capability replacement. */
    struct PerceptionSightPhysicsPublication final {
        Physics::PhysicsQueryEventIdentity identity;
        std::uint64_t sceneGeneration{};
        std::uint64_t publicationRevision{};
        std::uint64_t completedTick{};
    };

    /** @brief Evidence for one candidate; unknown outcomes never count as confirmed loss. */
    enum class PerceptionSightEvidence : std::uint8_t {
        Pending,
        Clear,
        Occluded,
        OutsideCone,
        BudgetExhausted,
        QueryTruncated,
        Stale,
    };

    /** @brief Value-only sight result, fenced by the exact source incarnation. */
    struct PerceptionSightObservation final {
        Runtime::EntityRef source;
        Math::WorldCoordinate64 sourcePosition; /**< Captured source base point; query policy adds its aim offset. */
        PerceptionSightEvidence evidence{PerceptionSightEvidence::Pending};
        bool visible{}; /**< Hysteresis-confirmed state, never a live transform read. */
    };

    /** @brief Bounded ordered result; truncation is explicit and cannot imply occlusion. */
    struct PerceptionSightResult final {
        std::array<PerceptionSightObservation, PerceptionSpatialLimits::CandidatesPerQuery> observations{};
        std::size_t count{};
        std::uint64_t simulationTick{};
        std::uint64_t spatialRevision{};
        bool pending{};
        bool candidatesTruncated{};
    };

    /**
     * @brief Owner-thread sight kernel with bounded transient history and one owned in-flight batch.
     * @details Hosts call Begin at a sensing safe point, process Physics batches on the Physics owner lane,
     * then Poll before staging sight deltas. No Scene/Physics pointer or native handle is retained.
     * The dedicated occlusion channel must contain blockers, not target colliders: Clear means no blocker
     * up to the declared target point. Physics constructs one owned command batch and one owned completion per
     * sensing window, with at most 128 rays and 32 hits per ray (4096 total). These existing canonical batch
     * construction allocations occur only when a scheduled window is admitted, never for idle/Poll/history work.
     * Typed failure construction may allocate diagnostic storage. Sight itself uses fixed storage throughout.
     * One object belongs to one listener/configuration lifetime; disable, reconfigure and unload call Reset.
     */
    class PerceptionSight final {
    public:
        PerceptionSight() = default;
        PerceptionSight(const PerceptionSight &) = delete;
        PerceptionSight &operator=(const PerceptionSight &) = delete;
        /** @brief Cancels outstanding work without borrowing its retired world. */
        ~PerceptionSight();

        /**
         * @brief Filters immutable candidates and submits one bounded canonical LOS batch.
         * @param scene Fresh current owner-thread Scene view; borrowed only for this call.
         * @param spatial Current immutable candidate snapshot of the same scene.
         * @param physics Explicit host-issued canonical query capability.
         * @param frame Exact listener pose and scene/Physics publication binding.
         * @param policy Valid immutable policy, unchanged until Reset.
         * @param budget Shared allowance; charged only for successfully admitted rays.
         * @return Pending/complete value result or typed invalid, stale, lifecycle or Physics error.
         * @pre No batch is pending, ticks increase, and scene owners outlive the synchronous call.
         */
        [[nodiscard]] Result<PerceptionSightResult> Begin(const Runtime::RuntimeSceneView &scene, const PerceptionSpatialSnapshot &spatial,
                                                          const Physics::PhysicsQueryEventCapability &physics,
                                                          const PerceptionSightFrame &frame, const PerceptionSightPolicy &policy,
                                                          PerceptionSightBudget &budget);

        /**
         * @brief Consumes terminal LOS evidence only after revalidating live generation/publication fences.
         * @param scene Fresh current Scene view; old listener/source generations are discarded.
         * @param spatialRevision Current candidate publication revision.
         * @param physics Current host-selected world/capability, scene generation, tick and publication.
         * @return Current copied result, pending while Physics has not processed it, or original typed Physics failure.
         * @details Repeated Poll does not advance hysteresis. A replaced scene/listener invalidates all history.
         */
        [[nodiscard]] Result<PerceptionSightResult> Poll(const Runtime::RuntimeSceneView &scene, std::uint64_t spatialRevision,
                                                         const PerceptionSightPhysicsPublication &physics);

        /** @brief Cancels pending work and clears visibility/history before disable, reconfiguration or unload. */
        void Reset() noexcept;

    private:
        friend struct PerceptionSightTestAccess;

        struct History final {
            Runtime::EntityRef source;
            std::uint32_t confirmations{};
            bool lastClear{};
            bool visible{};
        };

        /** @brief Applies exactly one confirmed sample; unknown/budget/stale results never call this helper. */
        void Confirm(std::size_t index, bool clear);
        /** @brief Removes stale generation evidence even on repeated reads of already completed work. */
        void Revalidate(const Runtime::RuntimeSceneView &scene, bool publicationCurrent);
        /** @brief Checks exact current Scene/listener binding without changing owned history. */
        [[nodiscard]] bool ListenerBindingCurrent(const Runtime::RuntimeSceneView &scene, const PerceptionSpatialSnapshot &spatial,
                                                  const PerceptionSightFrame &frame) const;
        /** @brief Validates admission policy/lifecycle before resolving the frozen listener position. */
        [[nodiscard]] Result<Math::WorldCoordinate64> ValidateWindow(const Runtime::RuntimeSceneView &scene,
                                                                     const PerceptionSpatialSnapshot &spatial,
                                                                     const PerceptionSightFrame &frame, const PerceptionSightPolicy &policy,
                                                                     std::uint64_t simulationTick) const;
        /** @brief Checks every current candidate/Physics publication fence before consuming owned work. */
        [[nodiscard]] bool PublicationCurrent(std::uint64_t spatialRevision, const PerceptionSightPhysicsPublication &physics) const;
        /** @brief Starts a validated window without carrying unrelated candidate history. */
        void InitializeWindow(const PerceptionSpatialResult &candidates, std::uint64_t spatialRevision,
                              const Physics::PhysicsQueryEventIdentity &identity, const PerceptionSightFrame &frame,
                              const PerceptionSightPolicy &policy, std::uint64_t simulationTick);
        /** @brief Restores exact-source history and prepares one bounded candidate; returns a descriptor error if invalid. */
        [[nodiscard]] std::optional<Error> PrepareCandidate(
            const Runtime::RuntimeSceneView &scene, const PerceptionSpatialCandidate &candidate,
            const Math::WorldCoordinate64 &listenerPosition,
            const std::array<History, PerceptionSpatialLimits::CandidatesPerQuery> &previous, std::size_t index,
            std::size_t remainingRaycasts);
        /** @brief Admits prepared rays once and charges the shared budget only on success. */
        [[nodiscard]] Result<PerceptionSightResult> Submit(const Physics::PhysicsQueryEventCapability &physics,
                                                           PerceptionSightBudget &budget);
        /** @brief Validates one copied canonical completion without mutating history or visibility. */
        [[nodiscard]] Result<PerceptionSightEvidence> ValidateEntry(const Physics::PhysicsQueryBatchEntry &entry,
                                                                    const Physics::PhysicsQueryDescriptor &descriptor) const;
        /** @brief Validates every entry before committing any visibility; corrupt input becomes terminal. */
        [[nodiscard]] Result<PerceptionSightResult> Consume(const Physics::PhysicsQueryBatchCompletion &completion);
        /** @brief Closes failed work and caches its typed terminal error until the next admitted window. */
        [[nodiscard]] Result<PerceptionSightResult> Fail(Error error);

        std::array<History, PerceptionSpatialLimits::CandidatesPerQuery> history_{};
        std::array<std::size_t, PerceptionSpatialLimits::CandidatesPerQuery> rayIndices_{};
        std::array<Physics::PhysicsQueryCommand, PerceptionSpatialLimits::CandidatesPerQuery> commands_{};
        std::size_t rayCount_{};
        PerceptionSightPolicy policy_;
        PerceptionSightFrame frame_;
        Physics::PhysicsQueryEventIdentity physicsIdentity_;
        PerceptionSightResult result_;
        std::optional<Physics::PhysicsQueryBatchHandle> batch_;
        bool started_{};
        std::optional<Error> failure_;
    };
}  // namespace Horo::AI
