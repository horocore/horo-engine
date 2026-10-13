#pragma once

/** @file NavigationVelocityPipeline.h
 * @brief Owner-phase preferred/safe velocity publication and exact achieved-motion feedback.
 */

#include "Horo/Navigation/NavigationCrowdAvoidance.h"
#include "Horo/Navigation/NavigationMovement.h"

#include <thread>

namespace Horo::Navigation {
    /** @brief Fixed-tick blockage hysteresis; both limits are nonzero and bounded at admission. */
    struct NavigationVelocityPolicy final {
        std::uint32_t blockedTicksBeforeRepath{3};
        std::uint64_t repathCooldownTicks{10};
        float minimumProgressMeters{0.001F};
    };

    /** @brief One owner-issued solver request; the caller pins the same immutable snapshot for worker execution. */
    struct NavigationVelocityWork final {
        std::uint64_t sequence{};
        NavigationMovementIntent preferred;
        NavigationAvoidanceRequest request;
        NavigationAgentSceneBinding binding;
        NavigationDynamicRegistryRevision dynamicRevision;
        std::uint64_t characterRevision{};
    };

    /** @brief Exact authority evidence and safe desired motion; this value never commits Physics movement. */
    struct NavigationVelocityPublication final {
        NavigationMovementIntent intent;
        NavigationAgentSceneBinding binding;
        std::uint64_t characterRevision{};
        NavigationAvoidanceDisposition disposition{NavigationAvoidanceDisposition::CollisionSafeStop};
        NavigationAvoidanceStopReason stopReason{NavigationAvoidanceStopReason::None};
        Math::Vec3 preferredVelocity{}; /**< Route intent before avoidance, for explicit blocked-progress diagnostics. */
    };

    /** @brief Character-owned result of one exact published intent, measured over its declared fixed step. */
    struct NavigationVelocityFeedback final {
        NavigationMovementId operation;
        PathId path;
        std::uint64_t tick{};
        std::uint64_t characterRevision{};
        Math::Vec3 achievedDisplacement{};
        bool blocked{};
    };

    /**
     * @brief Single-agent owner-thread pipeline above movement lifecycle and the optional asynchronous avoidance solver.
     * @details The borrowed movement controller outlives this object. Prepare runs in NavIntentCommit's owner phase;
     * workers solve only immutable work/snapshot values and never call this object. Commit rechecks the exact capture
     * at the same declared tick before publication. Character/Physics alone applies desired motion and returns feedback
     * before the next Prepare. Calls allocate no pipeline storage; one pending work and one feedback slot are retained.
     * Errors authorize no motion. The host must submit zero when a publication is absent or rejected.
     */
    class NavigationVelocityPipeline final {
        /** @brief Nonaggregate factory-only admission authority, never issued or retained. */
        class ConstructionKey final {
            friend class NavigationVelocityPipeline;
            ConstructionKey() noexcept = default;

        public:
            ConstructionKey(const ConstructionKey &) = delete;
            ConstructionKey &operator=(const ConstructionKey &) = delete;
        };

    public:
        /** @brief Construct only with the private factory's validated admission authority; callers use Create.
         * @param key Inaccessible factory key borrowed only during exact-type construction.
         * @param movement Borrowed movement owner that outlives this pipeline.
         * @param binding Validated Scene/world binding. @param agent Exact registered agent generation.
         * @param revision Nonzero Character incarnation/reset revision. @param policy Validated bounded feedback policy.
         */
        NavigationVelocityPipeline(ConstructionKey &&key, NavigationMovementController &movement, NavigationAgentSceneBinding binding,
                                   CrowdAgentHandle agent, std::uint64_t revision, NavigationVelocityPolicy policy) noexcept;
        /** @brief Validate bounded policy and initial authority outside fixed-tick work.
         * @param movement Borrowed prepared movement owner.
         * @param binding Exact Scene/world incarnation.
         * @param agent Exact registered logical agent.
         * @param characterRevision Nonzero host-owned Character incarnation/reset revision.
         * @param policy Fixed bounded blockage/repath policy.
         * @return Prepared value or typed descriptor error.
         */
        [[nodiscard]] static Result<std::unique_ptr<NavigationVelocityPipeline>> Create(NavigationMovementController &movement,
                                                                                        NavigationAgentSceneBinding binding,
                                                                                        CrowdAgentHandle agent,
                                                                                        std::uint64_t characterRevision,
                                                                                        NavigationVelocityPolicy policy = {});
        NavigationVelocityPipeline(const NavigationVelocityPipeline &) = delete;
        NavigationVelocityPipeline &operator=(const NavigationVelocityPipeline &) = delete;

        /** @brief Advance the real movement operation and prepare one bounded solver request.
         * @param current Committed owner observation; feedback is supplied exclusively through ObserveFeedback.
         * @param snapshot Complete exact-tick immutable avoidance capture.
         * @param request Authored finite solver envelope; agentIndex must name this pipeline's exact agent.
         * @return Work to schedule through JobSystem, or typed failure with no safe publication.
         * @note A new movement operation retires old feedback/work/history without changing Character incarnation.
         */
        [[nodiscard]] Result<NavigationVelocityWork> Prepare(const NavigationMovementObservation &current,
                                                             const NavigationCrowdSnapshot &snapshot, NavigationAvoidanceRequest request);

        /** @brief Publish only the latest exact work at its captured owner tick.
         * @param work Unmodified owner-issued request identity.
         * @param outcome Worker-produced sample or explicit stop.
         * @param current Current complete capture, re-read at publication.
         * @param observation Fresh same-tick world/path/agent observation before NavIntentCommit.
         * @return Safe desired motion or stale/malformed failure; failed commits never publish worker values.
         */
        [[nodiscard]] Result<NavigationVelocityPublication> Commit(const NavigationVelocityWork &work,
                                                                   const NavigationAvoidanceOutcome &outcome,
                                                                   const NavigationCrowdSnapshot &current,
                                                                   const NavigationMovementObservation &observation);
        /** @brief Discard exact cancelled/failed solver work without publishing motion or resetting blockage history.
         * @param work Latest owner-issued work identity.
         * @return True only when that pending work was retired; late completion is subsequently stale.
         */
        [[nodiscard]] bool CancelPending(const NavigationVelocityWork &work) noexcept;

        /** @brief Retain one exact Character result for the next strictly advancing owner tick.
         * @param feedback Sole movement authority's finite displacement and blockage evidence.
         * @return Success or stale/malformed failure leaving accepted feedback unchanged.
         */
        [[nodiscard]] Result<void> ObserveFeedback(const NavigationVelocityFeedback &feedback);

        /** @brief Reset on teleport/Character replacement before producing another intent, forcing a fresh corridor.
         * @param current Fresh committed post-reset observation with no feedback.
         * @param characterRevision Strictly greater host reset/incarnation revision.
         * @return Success or failure preserving pipeline state.
         */
        [[nodiscard]] Result<void> Reset(const NavigationMovementObservation &current, std::uint64_t characterRevision);
        /** @brief Permanently reject work/feedback and discard retained history; movement retirement remains host-owned. */
        void Retire() noexcept;

    private:
        /** @brief Feedback projection staged until real movement advancement succeeds. */
        struct PreparedFeedback {
            NavigationMovementObservation observation;
            std::uint32_t blockedTicks{};
            bool repath{};
        };

        /** @brief Retire history whose operation has been replaced, preserving Character incarnation and tick ordering. */
        void RetirePreviousOperation() noexcept;
        /** @brief Project exact prior-tick feedback without committing counters or consuming accepted evidence.
         * @param current Fresh owner observation.
         * @return Observation and bounded hysteresis transition, or stale feedback error with history unchanged.
         */
        [[nodiscard]] Result<PreparedFeedback> PrepareFeedback(const NavigationMovementObservation &current) const;
        NavigationMovementController *movement_;
        NavigationAgentSceneBinding binding_;
        CrowdAgentHandle agent_;
        std::uint64_t characterRevision_{};
        NavigationVelocityPolicy policy_;
        std::optional<NavigationVelocityWork> pending_;
        std::optional<NavigationCrowdSnapshot> pendingCapture_;
        std::optional<NavigationVelocityPublication> publication_;
        std::optional<NavigationVelocityFeedback> feedback_;
        std::optional<std::uint64_t> lastTick_;
        std::optional<std::uint64_t> lastRepathTick_;
        std::uint64_t sequence_{};
        std::uint32_t blockedTicks_{};
        float publishedStepSeconds_{};
        bool retired_{};
        std::thread::id ownerThread_;
    };
}  // namespace Horo::Navigation
