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
        Math::Vec3 achievedDisplacement{}; /**< Optional exact-step Character displacement for the velocity pipeline. */
        bool requestRepath{};              /**< Bounded pipeline hysteresis decision for this exact acknowledged intent. */
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
     * before destroying this controller during normal retirement. Destruction cancels and relinquishes remaining result
     * reservations to the surviving coordinator without waiting;
     * final host teardown must also shut down the coordinator/JobSystem before unloading provider code.
     */
    class NavigationMovementController final {
        struct ConstructionKey {
        private:
            friend class NavigationMovementController;
            ConstructionKey() = default;
        };

    public:
        /** @brief Factory-only construction; the private key preserves validated agent/caller admission.
         * @param registry Borrowed agent authority. @param world Borrowed world authority.
         * @param coordinator Borrowed query owner. @param agent Validated owned registration snapshot.
         * @param binding Validated Scene/world incarnation. @param caller Validated unique host authority.
         */
        NavigationMovementController(ConstructionKey, NavigationAgentRegistry &registry, NavigationWorldLifecycle &world,
                                     NavigationCoordinator &coordinator, const NavigationAgentRecord &agent,
                                     NavigationAgentSceneBinding binding, NavigationPathCaller caller) noexcept;
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
        /** @brief Cancel and relinquish outstanding queries without joining; coordinator reclaims after worker drain. */
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
        /** @brief Invalidate motion/corridor history after teleport or Character incarnation replacement.
         * @param current Fresh committed observation without old feedback.
         * @return Success or descriptor/stale error; active requests retain their goal and finite query ceiling.
         */
        [[nodiscard]] Result<void> ResetMotionAtSafePoint(const NavigationMovementObservation &current);
        /** @brief Rebind compatible registration/topology at the owner safe point without restarting operation budgets.
         * @param agent Fresh enabled registration for the same exact Scene/entity owner; never a replacement entity.
         * @param caller Same logical caller with a strictly newer incarnation.
         * @param current Fresh committed replacement binding/root, with no feedback or stale target evidence.
         * @return Success or typed failure preserving the previous controller state.
         * @details A successful rebind withdraws corridor and old request authority, updates the operation's agent
         * identity, and preserves its semantic goal, exclusive deadline and total accepted-query count. The new
         * provider must admit the existing bounded query policy. Terminal operations stay terminal. Old requests
         * are relinquished to the coordinator; workers retain only their immutable old-world leases until drained.
         */
        [[nodiscard]] Result<void> RebindAtSafePoint(CrowdAgentHandle agent, NavigationPathCaller caller,
                                                     const NavigationMovementObservation &current);
        /** @brief Cancel only the exact active operation. @param id Accepted movement identity.
         * @return True only when that nonterminal operation was cancelled.
         */
        [[nodiscard]] bool Cancel(NavigationMovementId id) noexcept;
        /** @brief Permanently close acceptance and cancel the active operation without waiting. */
        void BeginShutdown() noexcept;
        /** @brief Relinquish obsolete requests and collect coordinator fences independently of movement updates.
         * @details The owner calls this at every lifecycle safe point, including Idle/TargetLost/terminal states.
         * At most the fixed retired-request capacity is handed off. Running workers retain coordinator-owned slots
         * and provider leases until exact terminal fences; no result is installed, no movement tick advances, and
         * no goal/deadline/query budget changes. The borrowed coordinator must outlive this controller.
         */
        void CollectRetiredAtSafePoint();
        /** @brief Report no query or retirement receipt retained by this movement owner.
         * @return True after results are consumed or handed to coordinator retirement. The coordinator may still
         * retain running worker/provider pins; its IsDrained reports physical request/partition quiescence.
         */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Return owned status of the latest accepted operation. */
        [[nodiscard]] const NavigationMovementStatus &Status() const noexcept;

        /** @brief Return the exact bound logical agent. @return Prepared registration handle. */
        [[nodiscard]] CrowdAgentHandle Agent() const noexcept {
            return agent_.handle;
        }

        /** @brief Return the exact bound Scene/world incarnation. @return Prepared owner binding. */
        [[nodiscard]] NavigationAgentSceneBinding Binding() const noexcept {
            return binding_;
        }

        /** @brief Check latest owner-issued intent before publishing asynchronous safe motion.
         * @param intent Exact returned operation/path/tick/state identity.
         * @return True only while that identity remains the controller's latest non-retired owner output.
         */
        [[nodiscard]] bool IsCurrentIntent(const NavigationMovementIntent &intent) const noexcept;
        /** @brief Revalidate live agent/world and held corridor immediately before safe-motion publication.
         * @param intent Exact latest owner output.
         * @param current Fresh same-tick combined-world observation, without feedback.
         * @return Success or stale/descriptor failure; revoked authority is retired through the normal lifecycle.
         */
        [[nodiscard]] Result<void> ValidateIntentAtSafePoint(const NavigationMovementIntent &intent,
                                                             const NavigationMovementObservation &current);

    private:
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
