#pragma once

/** @file GameplayAnimationContext.h
 * @brief Scene-scoped animation commands and committed observations for gameplay clients.
 */

#include "Horo/Animation/AnimationComponents.h"
#include "Horo/Animation/AnimationGraphGameplay.h"
#include "Horo/Gameplay/Behavior.h"

namespace Horo::Gameplay {
    /** @brief Value identity of an animated actor, fenced by entity and animation generations. */
    struct GameplayAnimationActor final {
        GameplayEntityRef entity;
        Animation::AnimationInstanceHandle instance;
    };

    /** @brief Copied committed motion result in the starting root frame. */
    struct GameplayRootMotionObservation final {
        Animation::RootMotionRequestId request;
        Animation::AnimationRootMotionDisposition disposition{};
        Animation::AnimationRootMotionDelta requested;
        Animation::AnimationRootMotionDelta applied;
        Animation::AnimationRootMotionDelta remaining;
    };

    /** @brief Last completed animation state for one exact active actor. */
    struct GameplayAnimationSnapshot final {
        GameplayAnimationActor actor;
        Animation::AnimationPlaybackState state{};
        Animation::PoseHandle previous;
        Animation::PoseHandle current;
        std::optional<GameplayRootMotionObservation> rootMotion;
    };

    /** @brief Action intent whose runtime cancellation follows the issued module grant. */
    struct GameplayAnimationAction final {
        Animation::GraphActionId action;
        Animation::GraphActionSectionId startSection;
        Animation::GraphActionQueuePolicy queue{Animation::GraphActionQueuePolicy::Queue};
    };

    /** @brief Retainable owner-lane module grant over exactly one Scene incarnation.
     * @details Successful calls use bounded graph storage. Retaining the grant does not keep a Scene active.
     * Commands are admitted between fixed attempts. Reads expose completed observations as copied values.
     */
    class GameplayAnimationContext {
    public:
        virtual ~GameplayAnimationContext() = default;

        /** @brief Identifies the admitted Scene. @return Exact runtime Scene identity. */
        [[nodiscard]] virtual std::uint64_t Scene() const noexcept = 0;
        /** @brief Identifies the admitted principal. @return Borrowed module ID for the grant's lifetime. */
        [[nodiscard]] virtual std::string_view ModuleId() const noexcept = 0;

        /** @brief Resolves an actor and its latest completed observations.
         * @param entity Exact entity in this grant's Scene. @return Snapshot, or typed scope or residency error.
         */
        [[nodiscard]] virtual Result<GameplayAnimationSnapshot> Observe(GameplayEntityRef entity) const = 0;
        /** @brief Queues an override for the actor's next committed tick.
         * @param actor Exact observed actor. @param parameter Scalar identity and typed value. @return Admission outcome.
         */
        [[nodiscard]] virtual Result<void> QueueParameter(GameplayAnimationActor actor,
                                                          const Animation::GraphParameterInput &parameter) const = 0;
        /** @brief Queues a one-tick trigger.
         * @param actor Exact observed actor. @param trigger Declared trigger. @return Admission outcome.
         */
        [[nodiscard]] virtual Result<void> QueueTrigger(GameplayAnimationActor actor, Animation::GraphParameterId trigger) const = 0;
        /** @brief Queues a declared action under this module's lifetime.
         * @param actor Exact observed actor. @param request Action intent. @return Actual retained action identity or typed error.
         */
        [[nodiscard]] virtual Result<Animation::GraphActionHandle> QueueAction(GameplayAnimationActor actor,
                                                                               const GameplayAnimationAction &request) const = 0;
        /** @brief Queues cancellation of an actor action.
         * @param actor Exact observed actor. @param action Retained action. @param stop Handoff policy. @return Admission outcome.
         */
        [[nodiscard]] virtual Result<void> CancelAction(GameplayAnimationActor actor, const Animation::GraphActionHandle &action,
                                                        Animation::GraphActionStopPolicy stop) const = 0;
        /** @brief Reads the owner fact for a retained action.
         * @param actor Exact observed actor. @param action Retained action. @return Copied fact or typed residency error.
         */
        [[nodiscard]] virtual Result<Animation::GraphActionObservation> ObserveAction(GameplayAnimationActor actor,
                                                                                      const Animation::GraphActionHandle &action) const = 0;
        /** @brief Releases a retained terminal action record.
         * @param actor Exact observed actor. @param action Retained terminal action. @return Admission outcome.
         */
        [[nodiscard]] virtual Result<void> ReleaseAction(GameplayAnimationActor actor,
                                                         const Animation::GraphActionHandle &action) const = 0;

        /** @brief Copies the complete typed parameter block.
         * @param actor Exact observed actor. @param pose Resident committed pose. @param output Exact parameter-sized buffer.
         * @return Success or typed error, preserving output on failure.
         */
        [[nodiscard]] virtual Result<void> CopyParameters(GameplayAnimationActor actor, const Animation::PoseHandle &pose,
                                                          std::span<Animation::GraphParameterInput> output) const = 0;
        /** @brief Copies the complete state-machine event block.
         * @param actor Exact observed actor. @param pose Resident committed pose. @param output Caller-owned storage.
         * @return Copied event count or typed error, preserving output on failure.
         */
        [[nodiscard]] virtual Result<std::uint32_t> CopyStateEvents(GameplayAnimationActor actor, const Animation::PoseHandle &pose,
                                                                    std::span<Animation::GraphStateEvent> output) const = 0;
        /** @brief Copies the complete clip occurrence block.
         * @param actor Exact observed actor. @param pose Resident committed pose. @param output Caller-owned storage.
         * @return Copied occurrence count or typed error, preserving output on failure.
         */
        [[nodiscard]] virtual Result<std::uint32_t> CopyClipEvents(GameplayAnimationActor actor, const Animation::PoseHandle &pose,
                                                                   std::span<Animation::AnimationEventOccurrence> output) const = 0;
        /** @brief Reads an authored socket in the actor's model space.
         * @param actor Exact observed actor. @param pose Resident committed pose. @param socket Stable skeleton-local socket identity.
         * @return Composed matrix or typed residency or missing-socket error.
         */
        [[nodiscard]] virtual Result<Math::Mat4> SocketModelTransform(GameplayAnimationActor actor, const Animation::PoseHandle &pose,
                                                                      Animation::SkeletonSocketId socket) const = 0;
    };
}  // namespace Horo::Gameplay
