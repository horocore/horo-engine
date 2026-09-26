#pragma once

/**
 * @file DestructionDamageRuntime.h
 * @brief Fixed-tick damage admission and detached semantic transitions for one destructible.
 */

#include "Horo/Destruction/DestructionCommand.h"

#include <cstdint>
#include <optional>

namespace Horo::Destruction {
    /** @brief Owner preparation phase; collision evidence is consumed only after the Physics step. */
    enum class DestructionDamageSafePoint : std::uint8_t {
        PrePhysics,
        PostPhysics,
    };

    /** @brief Current owner evidence supplied at each preparation or commit safe point. */
    struct DestructionDamageContext final {
        DestructionHandle target{};                                                   /**< Current world and destructible generation. */
        FractureArtifactContentIdentity content{};                                    /**< Current cooked content publication. */
        DestructionConfigurationRevision configurationRevision{};                     /**< Current authored policy publication. */
        DestructionCapabilityRevision capabilityRevision{};                           /**< Current command capability publication. */
        DestructionAuthorityGrant authority{};                                        /**< Current grant for the submitting issuer. */
        DestructionCommandLimits limits{};                                            /**< Current finite per-command bounds. */
        std::uint64_t simulationTick{};                                               /**< Non-zero owner safe-point tick. */
        DestructionDamageSafePoint safePoint{DestructionDamageSafePoint::PrePhysics}; /**< Current preparation phase. */
    };

    /** @brief Detached command and canonical state-machine candidate, including its fixed-tick decision. */
    class DestructionDamageTransition final {
    public:
        /** @brief Returns the original typed command. @return Immutable command. */
        [[nodiscard]] const DestructionCommand &Command() const noexcept;
        /** @brief Returns the canonical semantic candidate. @return Immutable state-machine transition. */
        [[nodiscard]] const DestructionStateTransition &StateTransition() const noexcept;
        /** @brief Returns the tick at which damage policy was evaluated. @return Non-zero fixed tick. */
        [[nodiscard]] std::uint64_t Tick() const noexcept;
        /** @brief Returns the preparation phase. @return Exact owner safe point. */
        [[nodiscard]] DestructionDamageSafePoint SafePoint() const noexcept;
        /** @brief Cancels detached work without changing the active state. @return Cancelled candidate. */
        [[nodiscard]] DestructionDamageTransition Cancel() const noexcept;

    private:
        friend class DestructionDamageRuntime;
        DestructionDamageTransition(DestructionCommand command, DestructionStateTransition stateTransition, std::uint64_t tick,
                                    DestructionDamageSafePoint safePoint) noexcept;

        DestructionCommand command_;
        DestructionStateTransition stateTransition_;
        std::uint64_t tick_{};
        DestructionDamageSafePoint safePoint_{DestructionDamageSafePoint::PrePhysics};
    };

    /**
     * @brief Immutable, allocation-free owner value for damage decisions at one destructible's safe point.
     * @details The host serializes Commit and publishes its returned value only with the aggregate Scene/Physics/Render
     * transaction. Physics callbacks provide later typed collision commands; they never call this owner directly.
     */
    class DestructionDamageRuntime final {
    public:
        /**
         * @brief Creates one generation from a validated descriptor and canonical state machine.
         * @param descriptor Exact immutable damage, threshold, trigger, and cooldown policy.
         * @param target Current owner generation.
         * @param initialRevision Non-zero first semantic revision.
         * @return Runtime value or typed state-machine identity failure.
         */
        [[nodiscard]] static Result<DestructionDamageRuntime> Create(const DestructibleDescriptor &descriptor, DestructionHandle target,
                                                                     DestructionStateRevision initialRevision);

        /** @brief Returns the current semantic state. @return Borrowed immutable snapshot. */
        [[nodiscard]] const DestructionStateSnapshot &Snapshot() const noexcept;
        /** @brief Returns the last successful command result, if any. @return Borrowed optional typed result. */
        [[nodiscard]] const std::optional<DestructionCommandResult> &LastResult() const noexcept;
        /** @brief Reports whether new mutation is admitted. @return False after shutdown begins. */
        [[nodiscard]] bool IsAdmissionOpen() const noexcept;

        /**
         * @brief Validates authority, exact revisions, ordered command identity, tick, and cooldown before preparation.
         * @param command Immutable gameplay, network, script, or post-step collision command.
         * @param context Current owner evidence at the preparation safe point.
         * @return Detached canonical candidate, exact duplicate, or typed rejection.
         * @post The active runtime value is unchanged on both success and failure.
         */
        [[nodiscard]] Result<DestructionDamageTransition> Prepare(const DestructionCommand &command,
                                                                  const DestructionDamageContext &context) const;

        /**
         * @brief Revalidates a detached candidate and advances the canonical state machine once.
         * @param transition Exact candidate from Prepare, possibly cancelled or prepared asynchronously.
         * @param context Current owner evidence at the aggregate publication safe point.
         * @return Successor runtime with typed success result, idempotent current value, or typed failure.
         * @post The caller publishes the returned value only with the complete aggregate transition.
         */
        [[nodiscard]] Result<DestructionDamageRuntime> Commit(const DestructionDamageTransition &transition,
                                                              const DestructionDamageContext &context) const;

        /**
         * @brief Starts the exact next generation with fresh health and cooldown history.
         * @param replacement Next handle for the same world and authored destructible.
         * @param descriptor Validated replacement descriptor.
         * @return Fresh runtime or typed stale-generation, owner, or shutdown failure.
         */
        [[nodiscard]] Result<DestructionDamageRuntime> Replace(DestructionHandle replacement,
                                                               const DestructibleDescriptor &descriptor) const;

        /** @brief Closes preparation, commit, and replacement admission. @return Shutdown-fenced value. */
        [[nodiscard]] DestructionDamageRuntime BeginShutdown() const noexcept;

    private:
        DestructionDamageRuntime(DestructibleDescriptor descriptor, DestructionStateMachine machine,
                                 std::optional<DestructionCommand> lastCommand, std::optional<std::uint64_t> lastCommittedTick,
                                 std::optional<std::uint64_t> lastDamageTick, std::optional<DestructionCommandResult> lastResult) noexcept;

        DestructibleDescriptor descriptor_;
        DestructionStateMachine machine_;
        std::optional<DestructionCommand> lastCommand_;
        std::optional<std::uint64_t> lastCommittedTick_;
        std::optional<std::uint64_t> lastDamageTick_;
        std::optional<DestructionCommandResult> lastResult_;
    };
}  // namespace Horo::Destruction
