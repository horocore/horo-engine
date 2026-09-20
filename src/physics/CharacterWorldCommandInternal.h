#pragma once

#include "CharacterWorldInternal.h"

namespace Horo::Character::Detail {
    /** @brief Validates immutable request evidence before attempting queue ownership. */
    [[nodiscard]] Result<void> ValidateAdmissionRequest(const auto &impl, const CharacterMovementRequest &request) {
        if (!impl.acceptingCommands.load())
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        if (const auto valid = ValidateCharacterMovementRequest(request, impl.descriptor.sceneGeneration, impl.descriptor.identity);
            valid.HasError())
            return valid;
        if (request.tick <= impl.closedTick.load())
            return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
        return Result<void>::Success();
    }

    /** @brief Tests whether one queued sequence makes a new request duplicate or globally stale. */
    [[nodiscard]] bool ConflictsWithQueuedCommand(const CharacterMovementRequest &queued,
                                                  const CharacterMovementRequest &request) noexcept {
        if (queued.controller != request.controller)
            return false;
        return (queued.tick == request.tick && queued.sequence == request.sequence) ||
               (queued.tick < request.tick && queued.sequence >= request.sequence) ||
               (queued.tick > request.tick && queued.sequence <= request.sequence);
    }

    /** @brief Revalidates lifecycle/order and exact duplication while queue ownership is held. */
    [[nodiscard]] Result<void> ValidateLockedAdmission(const auto &impl, const CharacterMovementRequest &request) {
        if (!impl.acceptingCommands.load())
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        const std::uint32_t slot = request.controller.slot.index;
        if (slot >= impl.controllerGenerations.size() || impl.controllerGenerations[slot] != request.controller.slot.generation)
            return Result<void>::Failure(MakeError(CharacterErrors::HandleStale));
        if (request.tick <= impl.closedTick.load() || request.sequence <= impl.closedSequences[slot])
            return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
        const auto record = impl.controllers.Resolve(request.controller);
        if (record.HasError())
            return Result<void>::Failure(record.ErrorValue());
        if (record.Value()->reservedTeleportTick == request.tick || record.Value()->lastTeleportTick == request.tick)
            return Result<void>::Failure(
                MakeError(CharacterErrors::CommandOrderInvalid, "Move and teleport cannot target one Character tick."));
        if (std::ranges::any_of(impl.commands, [&request](const CharacterMovementRequest &queued) {
            return ConflictsWithQueuedCommand(queued, request);
        }))
            return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
        return Result<void>::Success();
    }

    /** @brief Orders owned commands independently of producer arrival timing. */
    [[nodiscard]] bool CommandLess(const CharacterMovementRequest &left, const CharacterMovementRequest &right) noexcept {
        if (left.tick != right.tick)
            return left.tick < right.tick;
        if (left.controller != right.controller)
            return left.controller < right.controller;
        return left.sequence < right.sequence;
    }

    /** @brief Restores the non-reentrant tick guard and completes deferred owner shutdown. */
    template <typename Impl> struct TickGuard final {
        explicit TickGuard(Impl &impl) noexcept : impl_(impl), previous_(impl_.ticking.exchange(true)) {}

        ~TickGuard() noexcept {
            impl_.ticking.store(previous_);
            if (impl_.shutdownRequested) {
                const auto registryLock = impl_.synchronization.LockRegistry();
                impl_.controllers.Drain();
                impl_.shutdownRequested = false;
            }
        }

        TickGuard(const TickGuard &) = delete;
        TickGuard &operator=(const TickGuard &) = delete;

        Impl &impl_;
        bool previous_{};
    };

    /** @brief Validates a frozen frame completely before queue or controller publication changes. */
    [[nodiscard]] Result<void> ValidateFrozenCommands(auto &impl) {
        for (std::size_t index = 0; index < impl.scratch.size(); ++index) {
            const CharacterMovementRequest &command = impl.scratch[index];
            if (const auto valid = ValidateCharacterMovementRequest(command, impl.descriptor.sceneGeneration, impl.descriptor.identity);
                valid.HasError())
                return valid;
            const auto record = impl.controllers.ResolveMutable(command.controller);
            if (record.HasError())
                return Result<void>::Failure(record.ErrorValue());
            if (command.sequence <= record.Value()->lastSequence)
                return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
            if (index != 0 && impl.scratch[index - 1].controller == command.controller &&
                impl.scratch[index - 1].sequence == command.sequence)
                return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
        }
        return Result<void>::Success();
    }

    /** @brief Canonicalizes and removes one validated eligible frame while holding queue ownership. */
    [[nodiscard]] Result<void> FreezeCommandFrame(auto &impl, const CharacterFixedTickInput &input) {
        const auto queueLock = impl.synchronization.LockCommands();
        if (std::ranges::any_of(impl.commands, [&input](const CharacterMovementRequest &command) {
            return command.tick < input.tick;
        }))
            return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
        if (const auto eligible = static_cast<std::size_t>(std::ranges::count_if(impl.commands,
                                                                                 [&input](const CharacterMovementRequest &command) {
            return command.tick == input.tick;
        }));
            eligible > impl.settings.Values().work.maximumCommandsPerTick)
            return Result<void>::Failure(MakeError(CharacterErrors::CapacityExceeded));

        impl.scratch.clear();
        for (const CharacterMovementRequest &command : impl.commands) {
            if (command.tick == input.tick)
                impl.scratch.push_back(command);
        }
        std::ranges::sort(impl.scratch, CommandLess);
        if (const auto valid = ValidateFrozenCommands(impl); valid.HasError())
            return valid;
        for (const CharacterMovementRequest &command : impl.scratch)
            impl.closedSequences[command.controller.slot.index] = command.sequence;
        std::erase_if(impl.commands, [&input](const CharacterMovementRequest &command) {
            return command.tick == input.tick;
        });
        impl.pendingCommands.store(static_cast<std::uint32_t>(impl.commands.size()));
        impl.closedTick.store(input.tick);
        return Result<void>::Success();
    }

    /** @brief Applies only the final replacement for each controller from one frozen canonical frame. */
    [[nodiscard]] std::uint32_t ApplyCommandFrame(auto &impl, const CharacterFixedTickInput &input) noexcept {
        std::uint32_t applied{};
        for (std::size_t index = 0; index < impl.scratch.size(); ++index) {
            const CharacterMovementRequest &command = impl.scratch[index];
            if (index + 1 != impl.scratch.size() && impl.scratch[index + 1].controller == command.controller)
                continue;
            auto record = impl.controllers.ResolveMutable(command.controller);
            record.Value()->lastMovement = command;
            record.Value()->lastSequence = command.sequence;
            if (input.observer.movement)
                input.observer.movement(input.observer.context, command);
            ++applied;
            if (impl.state.load() != CharacterWorldState::Active)
                break;
        }
        return applied;
    }

    /** @brief Atomically replaces the coherent Character publication marker. */
    void PublishTick(auto &impl, const CharacterFixedTickInput &input, const std::uint32_t applied) noexcept {
        const auto publicationLock = impl.synchronization.LockPublication();
        impl.published.completedTick = input.tick;
        ++impl.published.publicationRevision;
        impl.published.appliedCommands = applied;
    }

    /** @brief Emits one optional phase observation without exposing pipeline storage. */
    void ObservePhase(const CharacterFixedTickInput &input, const CharacterTickPhase phase) noexcept {
        if (input.observer.phase)
            input.observer.phase(input.observer.context, phase, input.tick);
    }
}  // namespace Horo::Character::Detail
