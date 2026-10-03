#pragma once

/**
 * @file NavigationCrowdSnapshot.h
 * @brief Bounded immutable spatial facts captured for navigation avoidance jobs.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Navigation/NavigationAgentRegistry.h"
#include "Horo/Navigation/NavigationDynamicRegistry.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>

namespace Horo::Navigation {
    namespace Detail {
        struct NavigationCrowdSnapshotStorage;
    }

    /** @brief Declared avoidance mode whose spatial capture must be independent of worker completion order. */
    enum class AvoidanceExecutionMode : std::uint8_t {
        Disabled,
        BestEffortBounded,
        DeterministicQualified,
        Count,
    };

    /** @brief Product-owned finite work and storage limits for one owner-safe-point capture. */
    struct NavigationCrowdSnapshotLimits final {
        std::size_t maximumAgents{4096};
        std::size_t maximumBoundarySegments{16'384};
        std::size_t maximumCellEntries{65'536};
        std::size_t maximumNeighborFacts{131'072};
        std::size_t maximumBoundaryFacts{98'304};
        std::size_t maximumPairChecks{1'000'000};
        std::size_t maximumBoundaryChecks{4'000'000};
        float cellSizeMeters{4.0F};
        AvoidanceExecutionMode mode{AvoidanceExecutionMode::BestEffortBounded};
    };

    /** @brief Profile-local neighborhood policy resolved before capture, never chosen by a provider or renderer. */
    struct NavigationCrowdProfileFacts final {
        NavigationAgentProfileId profile;
        float radiusMeters{0.5F};
        float neighborRadiusMeters{4.0F};
        std::uint32_t maximumNeighbors{16};
        std::uint32_t maximumBoundarySegments{16};
        NavigationDynamicLayerMask obstacleLayers{1};
    };

    /** @brief Project-stable avoidance layer mapped to one fixed mask bit; independent of obstacle layers. */
    struct NavigationAvoidanceLayerDescriptor final {
        NavigationAvoidanceLayerId id; /**< Durable project identity; zero is invalid. */
        std::uint8_t bitIndex{};       /**< Stable bit position in [0, 63]. */
    };

    /** @brief Directed local-steering policy, not gameplay/network authority or scheduling priority. */
    struct NavigationAvoidanceAgentPolicy final {
        std::uint8_t layerBit{};       /**< Declared layer occupied by this agent. */
        std::uint64_t avoidsLayers{1}; /**< Declared layers this agent steers around; direction need not be reciprocal. */
        float priority{0.5F};          /**< Finite [0, 1] right-of-way preference; higher values resist deviation. */
    };

    /** @brief One committed owner-thread motion sample for an enabled registered agent. */
    struct NavigationCrowdMotionSample final {
        CrowdAgentHandle handle;
        Math::Vec3 position;
        Math::Vec3 velocity;
        std::int32_t priority{};
        NavigationAvoidanceAgentPolicy avoidance;
    };

    /** @brief Immutable per-agent value and contiguous fact ranges; indices address this snapshot only. */
    struct NavigationCrowdAgentFact final {
        CrowdAgentHandle handle;
        NavigationAgentProfileId profile;
        Math::Vec3 position;
        Math::Vec3 velocity;
        float radiusMeters{};
        std::int32_t priority{};
        NavigationAvoidanceAgentPolicy avoidance;
        std::uint32_t firstNeighbor{};
        std::uint32_t neighborCount{};
        std::uint32_t truncatedNeighbors{};
        std::uint32_t firstBoundary{};
        std::uint32_t boundaryCount{};
        std::uint32_t truncatedBoundaries{};
    };

    /** @brief Stable source identity of a conservative planar obstacle boundary. */
    using NavigationCrowdBoundarySource = std::variant<NavigationObstacleHandle, NavigationModifierHandle>;

    /** @brief Snapshot-owned segment; a cylinder is represented by a conservative enclosing square. */
    struct NavigationCrowdBoundarySegment final {
        NavigationCrowdBoundarySource source;
        std::uint64_t stableId{};
        std::uint8_t edge{};
        NavigationDynamicLayerMask layers;
        Math::Vec3 first;
        Math::Vec3 second;
        float minimumY{}; /**< Conservative lower vertical extent of the source obstacle. */
        float maximumY{}; /**< Conservative upper vertical extent of the source obstacle. */
    };

    /** @brief Stable spatial-cell partition and its contiguous agent-index range. */
    struct NavigationCrowdCell final {
        std::int32_t x{};
        std::int32_t z{};
        std::uint32_t firstAgent{};
        std::uint32_t agentCount{};
    };

    /** @brief Profile aggregate of exact omitted facts after per-agent caps. */
    struct NavigationCrowdProfileTruncation final {
        NavigationAgentProfileId profile;
        std::uint32_t affectedAgents{};
        std::uint64_t neighborsOmitted{};
        std::uint64_t boundariesOmitted{};
    };

    /**
     * @brief Shared immutable owned value captured before avoidance workers are scheduled.
     * @details Retaining a copy pins all contiguous data after Scene/registry replacement. Currentness at result publication
     * must still be checked against Binding(), DynamicRevision(), and CaptureTick(). Spans are valid while a copy lives.
     */
    class NavigationCrowdSnapshot final {
    public:
        /** @brief Constructs an invalid empty snapshot. */
        NavigationCrowdSnapshot() noexcept = default;

        /** @brief Reports whether a complete capture was published. @return True for a valid immutable capture. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the captured Scene/world generation. @return Exact binding or invalid empty binding. */
        [[nodiscard]] NavigationAgentSceneBinding Binding() const noexcept;
        /** @brief Returns the captured dynamic-overlay revision. @return Exact revision or invalid identity. */
        [[nodiscard]] NavigationDynamicRegistryRevision DynamicRevision() const noexcept;
        /** @brief Returns the owner-declared fixed tick. @return Capture tick, or zero for invalid snapshots. */
        [[nodiscard]] std::uint64_t CaptureTick() const noexcept;
        /** @brief Returns the declared execution mode. @return Mode captured at the owner safe point. */
        [[nodiscard]] AvoidanceExecutionMode Mode() const noexcept;
        /** @brief Returns stable-handle-ordered agents. @return Snapshot-owned contiguous facts. */
        [[nodiscard]] std::span<const NavigationCrowdAgentFact> Agents() const noexcept;
        /** @brief Returns each agent's selected neighbor indices, addressed by its range. @return Contiguous indices. */
        [[nodiscard]] std::span<const std::uint32_t> NeighborIndices() const noexcept;
        /** @brief Returns each agent's selected boundary indices, addressed by its range. @return Contiguous indices. */
        [[nodiscard]] std::span<const std::uint32_t> BoundaryIndices() const noexcept;
        /** @brief Returns all owned conservative boundary segments. @return Contiguous segments. */
        [[nodiscard]] std::span<const NavigationCrowdBoundarySegment> BoundarySegments() const noexcept;
        /** @brief Returns deterministic planar cells. @return Coordinate-ordered cell ranges. */
        [[nodiscard]] std::span<const NavigationCrowdCell> Cells() const noexcept;
        /** @brief Returns agent indices in cell order. @return Contiguous spatial partition. */
        [[nodiscard]] std::span<const std::uint32_t> CellAgentIndices() const noexcept;
        /** @brief Returns per-profile truncation evidence. @return Profile-ordered aggregates. */
        [[nodiscard]] std::span<const NavigationCrowdProfileTruncation> ProfileTruncation() const noexcept;
        /** @brief Returns the immutable project-stable avoidance layer table captured for this tick. */
        [[nodiscard]] std::span<const NavigationAvoidanceLayerDescriptor> AvoidanceLayers() const noexcept;

    private:
        friend Result<NavigationCrowdSnapshot> BuildNavigationCrowdSnapshot(const NavigationAgentSnapshot &,
                                                                            const NavigationDynamicRegistrySnapshot &,
                                                                            std::span<const NavigationCrowdMotionSample>,
                                                                            std::span<const NavigationCrowdProfileFacts>,
                                                                            const NavigationCrowdSnapshotLimits &, std::uint64_t,
                                                                            std::span<const NavigationAvoidanceLayerDescriptor>);
        /** @brief Wraps a complete immutable capture. @param storage Snapshot-owned storage. */
        explicit NavigationCrowdSnapshot(std::shared_ptr<const Detail::NavigationCrowdSnapshotStorage> storage) noexcept;
        std::shared_ptr<const Detail::NavigationCrowdSnapshotStorage> storage_;
    };

    /**
     * @brief Captures a finite planar avoidance fact set from immutable registry publications and committed motion values.
     * @param agents Exact logical-agent publication; disabled records require no motion sample.
     * @param dynamic Exact same-Scene dynamic publication supplying enabled obstacles and exclusion modifiers.
     * @param motions One sample for every enabled agent, in any order; stale, duplicate, missing, or non-finite input fails.
     * @param profiles Unique policy for every enabled agent profile, with finite radii and hard-bounded fact caps.
     * @param limits Product ceiling, cell size, and declared mode; no source is silently truncated to fit storage.
     * @param captureTick Non-zero owner fixed tick represented by all inputs.
     * @param avoidanceLayers Complete project-stable layer table; empty uses only the built-in default layer.
     * @return Owned immutable snapshot or a typed invalid, stale, or capacity error without partial publication.
     */
    [[nodiscard]] Result<NavigationCrowdSnapshot> BuildNavigationCrowdSnapshot(
        const NavigationAgentSnapshot &agents, const NavigationDynamicRegistrySnapshot &dynamic,
        std::span<const NavigationCrowdMotionSample> motions, std::span<const NavigationCrowdProfileFacts> profiles,
        const NavigationCrowdSnapshotLimits &limits, std::uint64_t captureTick,
        std::span<const NavigationAvoidanceLayerDescriptor> avoidanceLayers = {});
}  // namespace Horo::Navigation
