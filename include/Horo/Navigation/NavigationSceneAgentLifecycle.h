#pragma once

/** @file NavigationSceneAgentLifecycle.h
 * @brief Scene-qualified owner of real movement, avoidance and discontinuity retirement.
 */

#include "Horo/Navigation/NavigationVelocityPipeline.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

namespace Horo::Navigation {
    /** @brief Exact host-published identity used to admit one Scene navigation lifecycle owner. */
    struct NavigationSceneAgentLifecycleDescriptor final {
        Runtime::EntityRef entity;       /**< Exact live Scene entity. */
        CrowdAgentHandle agent;          /**< Its enabled logical registration. */
        NavigationPathCaller caller;     /**< Unique host caller incarnation. */
        std::uint64_t characterRevision; /**< Nonzero Character reset incarnation. */
    };

    /** @brief Explicit host-composed navigation owner for one exact live Scene entity.
     * @details Registry, world and coordinator outlive this owner. All calls run on the Scene/navigation owner thread
     * after CommitDeferredLifecycleChanges and before Character locomotion. Scene views are borrowed only for calls;
     * workers retain immutable query/avoidance inputs, never this owner or a Scene pointer. The host reconciles every
     * owner before NavIntentCommit, including owners with no pending movement; null Scene means committed unload.
     * Disabled, destroyed or replaced entities retire permanently. Re-enabling requires a fresh registration/owner.
     * Runtime profile changes use explicit compatible registration rebind, not Scene component mutation. Character
     * remains the sole transform/velocity authority. No callback, provider discovery or extra worker pool is installed.
     */
    class NavigationSceneAgentLifecycle final {
        struct ConstructionKey {
        private:
            friend class NavigationSceneAgentLifecycle;
            ConstructionKey() = default;
        };

    public:
        /** @brief Factory-only construction after Scene and movement admission.
         * @param registry Borrowed logical registration authority.
         * @param entity Validated exact Scene entity.
         * @param movement Owned admitted movement controller.
         * @param velocity Owned admitted avoidance pipeline.
         * @param revision Validated Character reset incarnation.
         */
        NavigationSceneAgentLifecycle(ConstructionKey, NavigationAgentRegistry &registry, Runtime::EntityRef entity,
                                      std::unique_ptr<NavigationMovementController> movement,
                                      std::unique_ptr<NavigationVelocityPipeline> velocity, std::uint64_t revision) noexcept;
        /** @brief Validate the exact Scene entity and compose real movement/pipeline owners outside fixed-tick work.
         * @param registry Logical registration authority. @param world Committed navigation world owner.
         * @param coordinator Canonical query/JobSystem owner. @param scene Current committed Scene view.
         * @param descriptor Exact entity, registration, caller and Character reset identity.
         * @return Owned lifecycle or typed descriptor/currentness failure; no registry mutation on failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<NavigationSceneAgentLifecycle>> Create(
            NavigationAgentRegistry &registry, NavigationWorldLifecycle &world, NavigationCoordinator &coordinator,
            Runtime::RuntimeSceneView scene, const NavigationSceneAgentLifecycleDescriptor &descriptor);
        NavigationSceneAgentLifecycle(const NavigationSceneAgentLifecycle &) = delete;
        NavigationSceneAgentLifecycle &operator=(const NavigationSceneAgentLifecycle &) = delete;
        /** @brief Retire logical registration and transfer remaining query retirement without waiting. */
        ~NavigationSceneAgentLifecycle();

        /** @brief Check committed Scene/entity and registration authority, retiring missing/disabled replacements.
         * @param scene Fresh current view, or null after committed unload. A stale borrowed view is rejected, not inferred unload.
         * @return Success only while this exact entity/registration is active; otherwise no motion is authorized.
         * @details Invoke once per canonical owner lifecycle phase. Services obsolete request retirement even when
         * the host no longer calls Update after TargetLost. Ordinary operation guards do not poll global jobs.
         * The global coordinator owner also collects fences after disabled lifecycle owners have been destroyed.
         */
        [[nodiscard]] Result<void> ReconcileAtSafePoint(std::optional<Runtime::RuntimeSceneView> scene);
        /** @brief Start real MoveTo/Follow after Scene validation.
         * @param scene Current view. @param request Owned semantic request. @param current Fresh committed observation.
         * @return Real operation acceptance or typed failure with no motion.
         */
        [[nodiscard]] Result<NavigationMovementAcceptance> Start(Runtime::RuntimeSceneView scene, NavigationMovementRequest request,
                                                                 const NavigationMovementObservation &current);
        /** @brief Advance real path following without an avoidance provider.
         * @param scene Current view. @param current Fresh fixed-tick observation.
         * @return Preferred intent for an explicitly selected Physics-guarded policy, or failure requiring zero motion.
         */
        [[nodiscard]] Result<NavigationMovementIntent> Update(Runtime::RuntimeSceneView scene,
                                                              const NavigationMovementObservation &current);
        /** @brief Produce the exact input used by the canonical crowd snapshot builder; never derives velocity from positions.
         * @param scene Current view. @param current Fresh Character position/tick.
         * @param committedVelocity Authoritative finite Character velocity, not displacement across a teleport.
         * @param characterRevision Exact current reset revision.
         * @return Motion sample or typed failure. At a reset tick only zero velocity is admitted.
         */
        [[nodiscard]] Result<NavigationCrowdMotionSample> CaptureMotion(Runtime::RuntimeSceneView scene,
                                                                        const NavigationMovementObservation &current,
                                                                        Math::Vec3 committedVelocity, std::uint64_t characterRevision);
        /** @brief Reset corridor and avoidance at a committed teleport/Character replacement.
         * @param scene Current view. @param current Post-reset observation, without old feedback.
         * @param characterRevision Strictly newer revision.
         * @return Success with zero crowd history, or typed failure preserving motion state.
         */
        [[nodiscard]] Result<void> TeleportAtSafePoint(Runtime::RuntimeSceneView scene, const NavigationMovementObservation &current,
                                                       std::uint64_t characterRevision);
        /** @brief Rebind compatible published NavMesh/profile data, never a replacement Scene/entity.
         * @param scene Current view. @param agent Published compatible registration.
         * @param caller Newer caller incarnation. @param current Committed replacement root.
         * @param characterRevision Strictly newer revision.
         * @return Success preserving goal/deadline/query budget with zero motion history, or typed failure.
         */
        [[nodiscard]] Result<void> RebindAtSafePoint(Runtime::RuntimeSceneView scene, CrowdAgentHandle agent, NavigationPathCaller caller,
                                                     const NavigationMovementObservation &current, std::uint64_t characterRevision);
        /** @brief Prepare real movement/avoidance only after matching the current lifecycle-owned motion capture.
         * @param scene Current view. @param current Fresh observation. @param snapshot Exact immutable crowd capture.
         * @param request Finite authored solver envelope.
         * @return Immutable worker input or typed failure; workers use the host-selected backend/JobSystem.
         */
        [[nodiscard]] Result<NavigationVelocityWork> PrepareVelocity(Runtime::RuntimeSceneView scene,
                                                                     const NavigationMovementObservation &current,
                                                                     const NavigationCrowdSnapshot &snapshot,
                                                                     const NavigationAvoidanceRequest &request);
        /** @brief Recheck Scene lifecycle and pipeline currency immediately before publishing a worker result.
         * @param scene Fresh view. @param work Exact owner-issued input. @param outcome Worker result.
         * @param snapshot Fresh complete capture. @param current Same-tick observation.
         * @return Safe desired velocity or typed failure requiring zero motion.
         */
        [[nodiscard]] Result<NavigationVelocityPublication> CommitVelocity(Runtime::RuntimeSceneView scene,
                                                                           const NavigationVelocityWork &work,
                                                                           const NavigationAvoidanceOutcome &outcome,
                                                                           const NavigationCrowdSnapshot &snapshot,
                                                                           const NavigationMovementObservation &current);
        /** @brief Retire exact cancelled or failed avoidance work without authorizing motion.
         * @param work Latest owner-issued immutable input.
         * @return True only when that pending work was discarded; late completion remains stale.
         */
        [[nodiscard]] bool CancelVelocity(const NavigationVelocityWork &work) noexcept;
        /** @brief Accept exact Character feedback only while Scene authority remains active.
         * @param scene Current view. @param feedback Actual achieved movement, never teleport displacement.
         * @return Success or typed failure.
         */
        [[nodiscard]] Result<void> ObserveFeedback(Runtime::RuntimeSceneView scene, const NavigationVelocityFeedback &feedback);
        /** @brief Permanently disable, invalidate registration and relinquish all owned history/requests without waiting. */
        void DisableAtSafePoint() noexcept;

    private:
        /** @brief Validate live authority without globally polling coordinator partitions. */
        [[nodiscard]] Result<void> ValidateSceneAuthority(std::optional<Runtime::RuntimeSceneView> scene);
        /** @brief Validate exact live Scene entity/component without retaining its borrowed storage. */
        [[nodiscard]] static bool LiveEntity(Runtime::RuntimeSceneView scene, Runtime::EntityRef entity);
        /** @brief Validate the accepted goal entity against committed Scene storage, independently of host target facts. */
        [[nodiscard]] bool LiveTarget(Runtime::RuntimeSceneView scene) const;
        /** @brief Clear stale goal facts so the real movement controller terminates with TargetLost. */
        [[nodiscard]] NavigationMovementObservation QualifyTarget(Runtime::RuntimeSceneView scene,
                                                                  const NavigationMovementObservation &current) const;
        /** @brief Install a zero post-discontinuity motion sample while preserving publication tick fences. */
        void ResetMotionSample(const NavigationMovementObservation &current, std::uint64_t revision) noexcept;
        /** @brief Compare the solver's captured position/velocity to the current owner-supplied Character sample. */
        [[nodiscard]] bool MatchesMotion(const NavigationCrowdSnapshot &snapshot, std::size_t index, std::uint64_t tick) const noexcept;
        NavigationAgentRegistry &registry_;
        Runtime::EntityRef entity_;
        std::optional<NavigationAgentOwner> target_;
        std::unique_ptr<NavigationMovementController> movement_;
        std::unique_ptr<NavigationVelocityPipeline> velocity_;
        NavigationCrowdMotionSample motion_;
        std::optional<std::uint64_t> motionTick_;
        std::optional<std::uint64_t> resetTick_;
        std::uint64_t characterRevision_;
    };
}  // namespace Horo::Navigation
