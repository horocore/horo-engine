#pragma once

/** @file GameplayFormation.h
 * @brief Gameplay-owned formation assignments prepared for bounded Navigation queries.
 */

#include "Horo/Math/SceneMath.h"
#include "Horo/Navigation/NavigationAgentRegistry.h"

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
}  // namespace Horo::Gameplay
