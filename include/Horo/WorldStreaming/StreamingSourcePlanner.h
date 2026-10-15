#pragma once

/** @file StreamingSourcePlanner.h
 * @brief Explicit bounded source ownership, desired-state planning and read-only travel readiness.
 */
#include "Horo/WorldStreaming/StreamingCellStability.h"
#include "Horo/WorldStreaming/StreamingCellState.h"
#include "Horo/WorldStreaming/StreamingPriorityPolicy.h"
#include "Horo/WorldStreaming/StreamingSourcePrefetch.h"
#include "Horo/WorldStreaming/WorldPartitionRegistry.h"

#include <memory>
#include <span>

namespace Horo::WorldStreaming {
    /** @brief Explicit host-selected profile identity; zero is invalid. */
    struct StreamingSourceProfile final {
        std::uint64_t value{};
        [[nodiscard]] constexpr auto operator<=>(const StreamingSourceProfile &) const noexcept = default;
    };

    /** @brief Value-owned source filters; empty optional selectors mean every admitted value. */
    struct StreamingSourceSelection final {
        std::optional<WorldPartitionRegistryId> grid{}; /**< Exact mounted spatial registry, never a backend selector. */
        std::optional<StreamingLayerId> layer{};
        std::optional<std::uint8_t> lod{};
        std::optional<StreamingSourceProfile> profile{};
        std::uint64_t platformMask{~std::uint64_t{0}};
        std::uint64_t requiredGameplayTags{};
        std::uint64_t excludedGameplayTags{};
    };

    /** @brief Immutable runtime audience supplied explicitly by the host, independent of authored layer existence. */
    struct StreamingSourceEnvironment final {
        std::uint64_t revision{};
        StreamingSourceProfile profile{};
        std::uint64_t platformBit{}; /**< Exactly one host-issued platform bit. */
        std::uint64_t gameplayTags{};
        std::span<const StreamingLayerId> enabledLayers{}; /**< Canonical unique admitted runtime layers, copied on replacement. */
    };

    /** @brief Source-owned canonical kinematic sample; registration supplies its exact identity. */
    struct StreamingSourceMotionSample final {
        Math::WorldCoordinate64 position{};
        StreamingVelocity64 velocity{};
        std::uint64_t sampledAtServiceMilliseconds{};
    };

    /** @brief Camera/player/gameplay/network/preload owner publication; it performs no registration by itself. */
    struct StreamingSourceRequest final {
        StreamingSourceId id{};
        StreamingSourceRevision revision{};
        StreamingSourceIntent intent{StreamingSourceIntent::Gameplay}; /**< Player ownership uses Gameplay intent. */
        StreamingSourcePriority priority{};
        StreamingSourceShape shape{};
        StreamingDesiredResidency residency{StreamingDesiredResidency::Loaded};
        StreamingRetention retention{StreamingRetention::Releasable};
        StreamingSourceSelection selection{};
        std::optional<StreamingSourceMotionSample> prediction{}; /**< Camera/gameplay prediction; other intents reject it. */
    };

    /** @brief Mandatory preallocated planner limits and per-service work limits. */
    struct StreamingSourcePlannerLimits final {
        std::uint32_t sources{};       /**< Maximum active owners and slots visited during a complete cycle. */
        std::uint32_t cells{};         /**< Maximum tracked plan/stability rows; never the whole manifest by implication. */
        std::uint32_t contributions{}; /**< Maximum source/cell provenance rows, including predicted demand. */
        std::uint32_t sourceVisitsPerFrame{};
        std::uint32_t nodesPerFrame{};
        std::uint32_t candidatesPerFrame{};
        std::uint32_t stabilityRowsPerFrame{};
        std::uint64_t maximumPlanAgeMilliseconds{}; /**< Explicit bounded-motion publication age, in [1,60000]. */
    };

    /** @brief Construction facts for the single explicit planner owner. */
    struct StreamingSourcePlannerConfig final {
        StreamingSourcePlannerLimits limits{};
        StreamingSourceShapeSupport shapeSupport{};
        StreamingPriorityPolicy priority;
        StreamingCellStabilityPolicy stability;
        StreamingPrefetchPolicy prefetch;
    };

    /** @brief Complete canonical cell demand, distinct from actual residency or scheduler admission. */
    struct StreamingPlannedCell final {
        WorldPartitionCellHandle cell{};
        StreamingDesiredResidency residency{StreamingDesiredResidency::Unloaded};
        std::optional<StreamingDesiredResidency> pinnedFloor{};
        std::optional<StreamingSourceDescriptor> prioritySource{}; /**< Empty for policy-only linger; never a revoked source descriptor. */
        double priority{};
        std::uint64_t queuedAtServiceMilliseconds{};
        StreamingCellStabilitySnapshot stability{};
        std::int64_t boundaryThresholdMillimeters{}; /**< Threshold evidence, never a measured metric distance. */
    };

    /** @brief Actual bounded work and publication evidence for one service quantum. */
    struct StreamingSourcePlannerProgress final {
        std::uint64_t revision{};
        std::size_t sourceVisits{};
        std::size_t nodesVisited{};
        std::size_t candidatesExamined{};
        std::size_t stabilityRows{};
        bool published{};
    };

    /** @brief Exact source/cell demand retained independently of hysteresis or actual residency. */
    struct StreamingPlannedSourceCell final {
        WorldPartitionCellHandle cell{};
        StreamingSourceDescriptor source{};
        StreamingDesiredResidency residency{StreamingDesiredResidency::Unloaded};
        StreamingRetention retention{StreamingRetention::Releasable};
        bool predicted{};
        std::uint64_t motionSampledAtServiceMilliseconds{};
    };

    /** @brief Borrowed immutable plan valid until the next owner mutation or service quantum. */
    struct StreamingSourcePlanView final {
        WorldPartitionRegistryBinding partition{};
        std::uint64_t revision{};
        std::uint64_t sampledAtServiceMilliseconds{};
        std::span<const StreamingPlannedCell> cells{};
        std::span<const StreamingPlannedSourceCell> contributions{};
    };

    /** @brief Caller-captured current residency fact from the partition authority, never inferred by the planner. */
    struct StreamingTravelResidencyFact final {
        StreamingFence fence{};
        StreamingCellState state{StreamingCellState::Unloaded};
    };

    /** @brief Immutable authority evidence for a nonblocking travel poll. */
    struct StreamingTravelResidencyEvidence final {
        WorldPartitionRegistryBinding partition{};
        std::uint64_t revision{}; /**< Nonzero residency publication revision supplied by its owner. */
        std::uint64_t sampledAtServiceMilliseconds{};
        std::span<const StreamingTravelResidencyFact> cells{}; /**< Canonical unique exact-generation facts. */
    };

    /** @brief Travel remains pending until every destination cell has actual sufficient residency evidence. */
    enum class StreamingTravelReadiness : std::uint8_t {
        Pending,
        Ready,
        Failed
    };

    struct StreamingSourcePlannerState;
    class StreamingSourcePlanner;

    /** @brief Move-only RAII registration; owner-lane destruction withdraws its exact generation immediately. */
    class StreamingSourceRegistration final {
    public:
        StreamingSourceRegistration() = default;
        StreamingSourceRegistration(const StreamingSourceRegistration &) = delete;
        StreamingSourceRegistration &operator=(const StreamingSourceRegistration &) = delete;
        StreamingSourceRegistration(StreamingSourceRegistration &&other) noexcept;
        StreamingSourceRegistration &operator=(StreamingSourceRegistration &&other) noexcept;
        ~StreamingSourceRegistration() noexcept;
        /** @brief Returns the issued exact owner fence. @return Invalid for an empty/moved receipt. */
        [[nodiscard]] const StreamingSourceOwnerToken &Owner() const noexcept;
        /**
         * @brief Atomically replaces this source's complete intent at a strictly newer revision.
         * @param request Complete new intent, preserving source identity and intent category.
         * @return Success or typed invalid/stale/lifecycle/capacity failure without mutation.
         */
        [[nodiscard]] Result<void> Replace(const StreamingSourceRequest &request);
        /**
         * @brief Coalesces a newer motion sample without cancelling an in-flight bounded planning cycle.
         * @param sample Canonical source anchor position, velocity and strictly newer unscaled timestamp.
         * @return Success or typed stale/unsupported/range/lifecycle failure without mutation.
         * @details Camera/gameplay/network owners may move. The cycle captures one sample per source; later samples feed the next cycle.
         * Preload destinations require Replace, preserving exact travel revision semantics. Velocity prediction remains explicitly opt-in
         * through the registration request. Box anchors are minima; path anchors are first points; other shapes use center/anchor.
         */
        [[nodiscard]] Result<void> SampleMotion(const StreamingSourceMotionSample &sample);
        /** @brief Withdraws this generation idempotently. @return Typed owner-lane failure or success. */
        [[nodiscard]] Result<void> Close();

    private:
        friend class StreamingSourcePlanner;
        StreamingSourceRegistration(std::shared_ptr<StreamingSourcePlannerState> state, StreamingSourceOwnerToken owner) noexcept;
        std::shared_ptr<StreamingSourcePlannerState> state_;
        StreamingSourceOwnerToken owner_{};
    };

    /**
     * @brief Single owner-lane source planner with preallocated bounded staging and no residency ownership.
     * @details All methods and registration destruction run on the construction lane. It stages complete cycles across service calls;
     * hard source/environment/partition mutations immediately revoke the old plan. Motion is separately coalesced and publishes exact
     * captured sample provenance within the explicit plan-age ceiling. It performs no I/O, entity publication or scheduling.
     * Registration move-assignment/destruction and planner destruction require the owner lane; a lifetime-affinity violation terminates.
     * Close/Shutdown provide typed rejection before mutation when called from a foreign lane.
     */
    class StreamingSourcePlanner final {
    public:
        StreamingSourcePlanner(const StreamingSourcePlanner &) = delete;
        StreamingSourcePlanner &operator=(const StreamingSourcePlanner &) = delete;
        ~StreamingSourcePlanner() noexcept;
        /**
         * @brief Allocates all bounded staging once and captures an immutable partition publication.
         * @param config Explicit validated policies, support and storage/work ceilings.
         * @param partition Owning immutable spatial snapshot.
         * @param environment Explicit runtime selection facts copied before admission.
         * @return Unique active planner or typed invalid/storage/capacity failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<StreamingSourcePlanner>> Create(StreamingSourcePlannerConfig config,
                                                                                    WorldPartitionRegistrySnapshot partition,
                                                                                    const StreamingSourceEnvironment &environment);
        /** @brief Admits one complete source and issues its RAII generation. @param request Source intent. @return Receipt or typed
         * failure. */
        [[nodiscard]] Result<StreamingSourceRegistration> Register(const StreamingSourceRequest &request);
        /** @brief Replaces runtime audience atomically at a newer revision. @param environment New facts. @return Typed outcome. */
        [[nodiscard]] Result<void> ReplaceEnvironment(const StreamingSourceEnvironment &environment);
        /**
         * @brief Rebinds a fresh partition/world snapshot, withdrawing every old registration and publication.
         * @param partition New exact immutable publication; replay of the current binding is rejected.
         * @param environment Complete new runtime audience.
         * @return Success or typed failure preserving the current world and registrations.
         */
        [[nodiscard]] Result<void> ReplacePartition(WorldPartitionRegistrySnapshot partition,
                                                    const StreamingSourceEnvironment &environment);
        /**
         * @brief Advances one deterministic bounded planning quantum without allocation.
         * @param serviceTimeMilliseconds Monotonic unscaled current service time.
         * @param pressure Explicit host pressure; no ambient sampling.
         * @return Actual work counters/publication status or typed failure; no partial plan is published.
         */
        [[nodiscard]] Result<StreamingSourcePlannerProgress> Service(
            std::uint64_t serviceTimeMilliseconds, StreamingCellStabilityPressure pressure = StreamingCellStabilityPressure::Normal);
        /** @brief Captures current complete intent. @return Typed unavailable while invalidated/recomputing/closed. */
        [[nodiscard]] Result<StreamingSourcePlanView> Plan() const;
        /**
         * @brief Polls destination readiness without blocking or starting any load.
         * @param source Exact live destination registration.
         * @param revision Exact source revision; replacement makes old travel requests stale.
         * @param evidence Fresh immutable actual residency supplied by the authority.
         * @param serviceTimeMilliseconds Current monotonic time.
         * @param maximumEvidenceAgeMilliseconds Explicit positive freshness bound.
         * @return Ready only for actual sufficient destination facts, Pending, Failed, or typed invalid/stale/lifecycle failure.
         */
        [[nodiscard]] Result<StreamingTravelReadiness> PollTravel(const StreamingSourceRegistration &source,
                                                                  StreamingSourceRevision revision,
                                                                  const StreamingTravelResidencyEvidence &evidence,
                                                                  std::uint64_t serviceTimeMilliseconds,
                                                                  std::uint64_t maximumEvidenceAgeMilliseconds) const;
        /** @brief Withdraws all sources and plans; retained receipts become inert. @return Typed lane failure or success. */
        [[nodiscard]] Result<void> Shutdown();

    private:
        explicit StreamingSourcePlanner(std::shared_ptr<StreamingSourcePlannerState> state) noexcept;
        std::shared_ptr<StreamingSourcePlannerState> state_;
    };
}  // namespace Horo::WorldStreaming
