#pragma once

/** @file GameplayFormation.h
 * @brief Gameplay-owned formation assignments prepared for bounded Navigation queries.
 */

#include "Horo/Math/SceneMath.h"
#include "Horo/Navigation/NavigationAgentRegistry.h"
#include "Horo/Navigation/NavigationWorldLifecycle.h"

#include <optional>
#include <span>
#include <vector>

namespace Horo::Gameplay {
    struct GameplayFormationIdentityTag;
    /** @brief Host-issued group identity within one exact Scene incarnation. */
    using GameplayFormationId = Navigation::NavigationIdentity<GameplayFormationIdentityTag>;

    /** @brief Geometry selected by Gameplay before preparing assignments. */
    enum class GameplayFormationShape : std::uint8_t {
        Line,
        Column,
        Wedge,
        Custom,
    };

    /** @brief Assignment availability before reachability queries. */
    enum class GameplayFormationAvailability : std::uint8_t {
        Complete,
        Degraded,
        Dissolved,
    };

    /** @brief Position sampled by the host for one exact registered member generation. */
    struct GameplayFormationMember final {
        Navigation::CrowdAgentHandle agent;
        Math::Vec3 position;
    };

    /** @brief Caller-selected layout and membership revision. */
    struct GameplayFormationRequest final {
        static constexpr std::size_t MaximumMembers = 64;
        GameplayFormationId group;
        std::uint64_t revision{};
        GameplayFormationShape shape{GameplayFormationShape::Line};
        float spacingMeters{1.0F};   /**< Positive finite spacing used by the built-in shapes. */
        Math::Vec3 forward{0, 0, 1}; /**< Nonzero planar direction, normalized during preparation. */
        std::optional<Navigation::CrowdAgentHandle> preferredLeader;
        std::span<const Math::Vec3> customSlots; /**< Unique nonzero planar offsets, in right/forward coordinates. Slot zero is implicit. */
    };

    /** @brief Proposed per-member path endpoints. A slot is unique within its group revision. */
    struct GameplayFormationSlot final {
        std::uint32_t slot{};
        Navigation::CrowdAgentHandle agent;
        Math::Vec3 start;
        Math::Vec3 unprojectedTarget;
    };

    /** @brief Owned preparation suitable for copying into a detached query batch. */
    struct GameplayFormationPlan final {
        GameplayFormationId group;
        std::uint64_t revision{};
        Navigation::NavigationAgentSceneBinding binding;
        GameplayFormationAvailability availability{GameplayFormationAvailability::Dissolved};
        std::vector<GameplayFormationSlot> slots;
        std::vector<Navigation::CrowdAgentHandle> unassigned;
    };

    /** @brief Prepares unique assignments from one owner-supplied Scene snapshot.
     * @param request Exact group revision, layout and preferred leader.
     * @param agents Current registered navigation agents at the same owner cutoff as the member positions.
     * @param members Bounded set of intended members. Stale, disabled and non-grounded members have no slots.
     * @return Owned proposals or a typed validation/capacity failure. Slot zero is the live preferred leader or
     *         the lowest live handle. Custom layouts with too few slots report unassigned live members.
     * @details Targets require separate projection and path validation before becoming movement goals.
     */
    [[nodiscard]] Result<GameplayFormationPlan> PrepareGameplayFormation(const GameplayFormationRequest &request,
                                                                         const Navigation::NavigationAgentSnapshot &agents,
                                                                         std::span<const GameplayFormationMember> members);

    /** @brief Host-admitted bounds for a detached formation query batch. */
    struct GameplayFormationQueryPolicy final {
        Navigation::NavigationQueryRequirement projection{.query = Navigation::NavigationQueryKind::NearestPoint};
        Navigation::NavigationQueryRequirement path;
        Math::Vec3 projectionHalfExtents;
    };

    /** @brief A unique grounded target for the exact member and slot. */
    struct GameplayFormationGoal final {
        std::uint32_t slot{};
        Navigation::CrowdAgentHandle agent;
        Math::Vec3 groundTarget;
        Math::Vec3 movementTarget; /**< Target on the projected surface with the captured Character root offset. */
    };

    /** @brief Owned reachability results to revalidate against live group and topology before movement admission. */
    struct GameplayFormationResolution final {
        GameplayFormationId group;
        std::uint64_t revision{};
        Navigation::NavigationWorldActivationDescriptor source;
        GameplayFormationAvailability availability{GameplayFormationAvailability::Dissolved};
        std::vector<GameplayFormationGoal> goals;
        std::vector<Navigation::CrowdAgentHandle> unassigned;
    };

    /** @brief Projects each proposed slot and requires a complete path under that member's registered traversal filter.
     * @param plan Detached proposals from the owner cutoff.
     * @param agents Agent records captured at that same cutoff.
     * @param world Exact pinned world and topology read lease.
     * @param policy Explicit per-query budgets and finite positive projection extents.
     * @param cancellation Optional caller cancellation, checked between bounded provider calls.
     * @return Owned results with unavailable slots marked unassigned, or a typed batch failure.
     * @details Run as bounded background work. A revoked lease aborts the batch. The caller must recheck
     *          group, agent and topology generations at the owner cutoff before using a result.
     */
    [[nodiscard]] Result<GameplayFormationResolution> ResolveGameplayFormation(const GameplayFormationPlan &plan,
                                                                               const Navigation::NavigationAgentSnapshot &agents,
                                                                               const Navigation::NavigationWorldReadLease &world,
                                                                               const GameplayFormationQueryPolicy &policy,
                                                                               const CancellationToken &cancellation = {});
}  // namespace Horo::Gameplay
