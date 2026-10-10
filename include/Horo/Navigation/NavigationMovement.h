#pragma once

/** @file NavigationMovement.h
 * @brief Generation-safe owner-thread move-to, follow and repath operations.
 */

#include "Horo/Navigation/NavigationAgentRegistry.h"
#include "Horo/Navigation/NavigationCoordinator.h"

#include <array>
#include <memory>
#include <optional>

namespace Horo::Navigation {
    /** @brief One movement request in the lifetime of an exact registered agent. */
    struct NavigationMovementId final {
        CrowdAgentHandle agent;
        std::uint64_t sequence{};
        constexpr auto operator<=>(const NavigationMovementId &) const noexcept = default;
    };

    /** @brief Move-to terminates on arrival; follow remains active while holding near its live target. */
    enum class NavigationMovementMode : std::uint8_t {
        MoveTo,
        Follow,
        Count
    };
    /** @brief Explicit owner-operation states; every state from Arrived onward is terminal. */
    enum class NavigationMovementState : std::uint8_t {
        Idle,
        Querying,
        Following,
        Holding,
        Arrived,
        Superseded,
        Cancelled,
        TargetLost,
        AgentLost,
        WorldLost,
        TimedOut,
        Unreachable,
        Failed
    };

    /** @brief Fully owned movement policy; only complete paths may authorize movement. */
    struct NavigationMovementRequest final {
        NavigationMovementMode mode{NavigationMovementMode::MoveTo};
        Math::Vec3 destination{};                   /**< Point target when entity is absent. */
        std::optional<NavigationAgentOwner> entity; /**< Exact target entity incarnation in the bound Scene. */
        float acceptanceRadiusMeters{0.25F};
        float waypointRadiusMeters{0.1F};
        float speedMetersPerSecond{1.0F};
        float arrivalSpeedMetersPerSecond{0.05F};
        std::uint64_t timeoutTicks{600};  /**< Exclusive deadline relative to acceptance. */
        std::uint32_t maximumQueries{16}; /**< Total accepted queries per operation, in [1, 64]. */
        NavigationRepathPolicy repath{.debounceTicks = 2, .cooldownTicks = 5};
        NavigationQueryRequirement query; /**< Provider quality and finite per-query bounds. */
        std::uint64_t capabilityRevision{};
    };

    /** @brief Current committed target value, supplied by the host without retaining a Scene pointer. */
    struct NavigationMovementTarget final {
        NavigationAgentOwner owner;
        Math::Vec3 position{};
        std::uint64_t revision{}; /**< Nonzero, monotonic; position changes require a new revision. */
    };

    /** @brief Achieved movement for one exact issued intent, from the sole movement authority. */
    struct NavigationMovementFeedback final {
        NavigationMovementId operation;
        PathId path;
        std::uint64_t tick{}; /**< Exact tick stamped on the acknowledged intent. */
        float achievedSpeedMetersPerSecond{};
    };

    /** @brief One committed owner-phase observation; all borrowed data expires at the end of the call. */
    struct NavigationMovementObservation final {
        NavigationAgentSceneBinding binding;
        Math::Vec3 position{};
        float stepSeconds{};            /**< Finite positive fixed-step duration. */
        NavigationPathObservation path; /**< Atomic root/region/link revisions; goalRevision is assigned by this controller. */
        std::optional<NavigationMovementTarget> target;
        std::optional<NavigationMovementFeedback> feedback;
    };

    /** @brief Owned operation status, retaining typed provider/validation errors without callbacks. */
    struct NavigationMovementStatus final {
        NavigationMovementId id;
        NavigationMovementState state{NavigationMovementState::Idle};
        std::optional<Error> error;
    };

    /** @brief Accepted replacement policy: every new request supersedes the current operation. */
    struct NavigationMovementAcceptance final {
        NavigationMovementId accepted;
        std::optional<NavigationMovementStatus> superseded;
    };

    /** @brief Tick-addressed desired velocity; only Character/Physics may commit actual movement. */
    struct NavigationMovementIntent final {
        NavigationMovementId operation;
        PathId path;
        std::uint64_t tick{};
        Math::Vec3 desiredVelocity{}; /**< Zero for pending, held, invalidated or terminal operations. */
        NavigationMovementState state{NavigationMovementState::Idle};
    };

    /**
     * @brief Bounded per-agent movement lifecycle composed by the host above canonical query/currentness services.
     * @details All calls are owner-thread only. Registry, world and coordinator outlive this controller. The host commits
     * coordinator results before Update, dispatches admitted queries afterwards and forwards returned intent to its
     * Character adapter. No provider work, waits, callbacks, Scene pointers or Transform writes occur here. Up to eight
     * accepted query handles (including retired requests) are retained; pressure rejects replacement without changing
     * the old operation. BeginShutdown closes acceptance; continue Update after coordinator Commit until IsDrained
     * before destroying this controller during normal retirement. Destruction cancels remaining work without waiting;
     * final host teardown must also shut down the coordinator/JobSystem before unloading provider code.
     */
    class NavigationMovementController final {
    public:
        /** @brief Bind one live agent and host-selected caller authority outside fixed-tick work.
         * @param registry Scene-bound agent authority, outliving the controller.
         * @param world Navigation publication authority, outliving the controller.
         * @param coordinator Canonical process-job query owner, outliving the controller.
         * @param agent Exact registered handle.
         * @param caller Unique logical caller and incarnation published by the host for this agent.
         * @return Prepared controller or typed invalid/stale/capacity failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<NavigationMovementController>> Create(NavigationAgentRegistry &registry,
                                                                                          NavigationWorldLifecycle &world,
                                                                                          NavigationCoordinator &coordinator,
                                                                                          CrowdAgentHandle agent,
                                                                                          NavigationPathCaller caller);
        NavigationMovementController(const NavigationMovementController &) = delete;
        NavigationMovementController &operator=(const NavigationMovementController &) = delete;
        /** @brief Cancel outstanding queries without joining; normal retirement first drains them. */
        ~NavigationMovementController();

        /** @brief Accept a fully validated request and explicitly supersede the prior active request.
         * @param request Owned policy/point or generation-safe entity target.
         * @param current Fresh committed observation at acceptance.
         * @return Accepted identity and superseded status, or error leaving the current operation unchanged.
         */
        [[nodiscard]] Result<NavigationMovementAcceptance> Start(NavigationMovementRequest request,
                                                                 const NavigationMovementObservation &current);
        /** @brief Consume published results, revalidate authority/path, repath and produce bounded movement intent.
         * @param current Fresh observation at a strictly advancing owner tick (first update may equal acceptance tick).
         * @return Intent or descriptor error; on error the host must submit zero desired motion.
         */
        [[nodiscard]] Result<NavigationMovementIntent> Update(const NavigationMovementObservation &current);
        /** @brief Cancel only the exact active operation. @param id Accepted movement identity.
         * @return True only when that nonterminal operation was cancelled.
         */
        [[nodiscard]] bool Cancel(NavigationMovementId id) noexcept;
        /** @brief Permanently close acceptance and cancel the active operation without waiting. */
        void BeginShutdown() noexcept;
        /** @brief Return true after all retained accepted query results have been consumed. */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Return owned status of the latest accepted operation. */
        [[nodiscard]] const NavigationMovementStatus &Status() const noexcept;

    private:
        NavigationMovementController(NavigationAgentRegistry &registry, NavigationWorldLifecycle &world, NavigationCoordinator &coordinator,
                                     NavigationAgentRecord agent, NavigationAgentSceneBinding binding,
                                     NavigationPathCaller caller) noexcept;
        /** @brief Check complete observation shape before changing state. */
        [[nodiscard]] bool ValidObservation(const NavigationMovementObservation &current) const noexcept;
        /** @brief Validate agent, Scene/world and target identities without replacing the current operation. */
        [[nodiscard]] Result<void> ValidateStartIdentity(const NavigationMovementRequest &request,
                                                         const NavigationMovementObservation &current) const;
        /** @brief Check live registry and Scene/world authority, terminating revoked operations. */
        [[nodiscard]] bool CheckAuthority(const NavigationMovementObservation &current);
        /** @brief Observe the exact live target incarnation and advance its movement revision. */
        [[nodiscard]] bool ObserveTarget(const NavigationMovementObservation &current);
        /** @brief Retain cancellation until the coordinator transfers its terminal record. */
        void RetireQuery() noexcept;
        /** @brief Consume obsolete results without ever installing their paths. */
        void DrainRetired();
        /** @brief Clear movement authority and cancel outstanding work. */
        void Finish(NavigationMovementState state, std::optional<Error> error = {}) noexcept;
        /** @brief Install one exact published query or preserve its typed terminal failure. */
        void Consume(const NavigationPathObservation &current);
        /** @brief Validate the complete waypoint contract and install the exact accepted query path. */
        void InstallPath(NavRequestHandle handle, NavigationPath result, const NavigationOutcomeProvenance &source,
                         const NavigationPathObservation &current);
        /** @brief Admit one coalesced path query under the operation deadline. */
        void Submit(const NavigationMovementObservation &current, const NavigationPathObservation &path);
        /** @brief Advance corridor targets and require exact achieved-motion evidence for arrival. */
        [[nodiscard]] NavigationMovementIntent Follow(const NavigationMovementObservation &current, const NavigationPathObservation &path);

        NavigationAgentRegistry &registry_;
        NavigationWorldLifecycle &world_;
        NavigationCoordinator &coordinator_;
        NavigationAgentRecord agent_;
        NavigationAgentSceneBinding binding_;
        NavigationPathCaller caller_;
        NavigationMovementStatus status_;
        NavigationMovementRequest request_;
        NavigationPathPolicy policy_{{}};
        std::optional<NavRequestHandle> query_;
        std::array<NavRequestHandle, 8> retired_{};
        std::size_t retiredCount_{};
        NavigationOutcomeProvenance querySource_;
        Math::Vec3 queryStart_{};
        std::uint64_t queryGoal_{};
        std::uint64_t queryLink_{};
        std::uint64_t goalRevision_{1};
        std::uint64_t targetRevision_{};
        Math::Vec3 goal_{};
        std::uint64_t sequence_{};
        std::uint64_t deadline_{};
        std::size_t waypoint_{};
        std::uint32_t queries_{};
        std::optional<std::uint64_t> lastUpdate_;
        std::optional<std::uint64_t> lastIntent_;
        bool shutdown_{};
    };
}  // namespace Horo::Navigation
