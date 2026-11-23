#pragma once

/** @file GameplayFormationMovement.h
 * @brief Owner-thread application and cancellation of one group's live movement assignments.
 */

#include "Horo/Gameplay/GameplayNavigationContext.h"

#include <thread>

namespace Horo::Gameplay {
    /** @brief Borrowed exact member grant for the duration of Apply. */
    struct GameplayFormationMovementBinding final {
        Navigation::CrowdAgentHandle agent;
        const GameplayNavigationContext *navigation{};
    };

    /** @brief One refusal from the ordinary movement admission boundary. */
    struct GameplayFormationMovementRefusal final {
        Navigation::CrowdAgentHandle agent;
        Error error;
    };

    /** @brief Started members, unavailable members and individual movement refusals for one applied revision. */
    struct GameplayFormationMovementReport final {
        GameplayFormationAvailability availability{GameplayFormationAvailability::Dissolved};
        std::vector<Navigation::CrowdAgentHandle> started;
        std::vector<Navigation::CrowdAgentHandle> unassigned;
        std::vector<GameplayFormationMovementRefusal> refused;
    };

    /** @brief Owns one cancellable group revision at a time. It never retains borrowed movement grants. */
    class GameplayFormationMovement final {
    public:
        /** @brief Binds one group identity on the current owner thread. @param group Exact Gameplay group. */
        explicit GameplayFormationMovement(GameplayFormationId group) noexcept;
        /** @brief Cancels the current revision's movement requests. */
        ~GameplayFormationMovement();
        GameplayFormationMovement(const GameplayFormationMovement &) = delete;
        GameplayFormationMovement &operator=(const GameplayFormationMovement &) = delete;

        /** @brief Replaces the active group revision and starts reachable assigned members.
         * @param resolved Detached output from the formation query batch.
         * @param activeRevision Exact current Gameplay revision. Stale and repeated revisions are rejected.
         * @param world Live Navigation owner, checked before replacing the previous revision.
         * @param members Current optional grants by distinct exact agent generation.
         * @param cancellation Optional caller scope for all members of this revision.
         * @return Explicit per-member admission results or a typed group failure.
         * @details Every previously owned move is cancelled before replacement. A refused or unassigned
         *          member therefore cannot continue moving under the old formation. Tactical movement
         *          outside this group owns its own request lifetime. A storage failure after
         *          replacement cancels the newly admitted requests and retires this owner.
         */
        [[nodiscard]] Result<GameplayFormationMovementReport> Apply(const GameplayFormationResolution &resolved,
                                                                    std::uint64_t activeRevision,
                                                                    const Navigation::NavigationWorldLifecycle &world,
                                                                    std::span<const GameplayFormationMovementBinding> members,
                                                                    const CancellationToken &cancellation = {});
        /** @brief Permanently closes admission on the owner thread and cancels this group's active movement. */
        void Retire() noexcept;

    private:
        /** @brief Dispatches this admitted revision through the matched member grants. */
        void StartMembers(const GameplayFormationResolution &resolved, std::span<const GameplayFormationMovementBinding> members,
                          GameplayFormationMovementReport &result) const;

        const GameplayFormationId group_;
        const std::thread::id owner_;
        std::optional<CancellationSource> active_;
        std::uint64_t revision_{};
        bool retired_{};
    };
}  // namespace Horo::Gameplay
