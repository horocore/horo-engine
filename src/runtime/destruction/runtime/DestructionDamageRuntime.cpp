#include "Horo/Destruction/DestructionDamageRuntime.h"

#include "Horo/Destruction/DestructionErrors.h"

#include <utility>

namespace Horo::Destruction {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Checks the current owner evidence before duplicate or fresh command admission. */
        [[nodiscard]] Result<void> ValidateContext(const DestructionCommand &command, const DestructionDamageContext &context,
                                                   const DestructionStateSnapshot &snapshot) {
            if (context.simulationTick == 0 || !context.capabilityRevision.IsValid() || !context.authority.IsValid() ||
                !context.limits.IsValid() ||
                (context.safePoint != DestructionDamageSafePoint::PrePhysics &&
                 context.safePoint != DestructionDamageSafePoint::PostPhysics))
                return Failure<void>(DestructionErrors::CommandInvalid);
            if (command.Kind() == DestructionCommandKind::Collision && context.safePoint != DestructionDamageSafePoint::PostPhysics)
                return Failure<void>(DestructionErrors::CommandInvalid);
            if (const auto access = ValidateDestructionCommandAccess(command.Header().id, context.target); access.HasError())
                return access;
            if (const auto access = ValidateDestructionCommandAccess(command.Header().id, snapshot.target); access.HasError())
                return access;
            if (context.content != snapshot.content)
                return Failure<void>(DestructionErrors::StaleContent);
            if (context.configurationRevision != snapshot.configurationRevision)
                return Failure<void>(DestructionErrors::StaleConfiguration);
            if (context.simulationTick < command.Header().eligibleSimulationTick)
                return Failure<void>(DestructionErrors::CommandInvalid);
            if (command.Header().capabilityRevision != context.capabilityRevision)
                return Failure<void>(DestructionErrors::CommandUnsupported);
            if (command.Header().authority != context.authority)
                return Failure<void>(DestructionErrors::CommandAuthorityDenied);
            return Result<void>::Success();
        }

        /** @brief Checks whether a committed command is the exact latest retry. */
        [[nodiscard]] Result<bool> IsExactRetry(const std::optional<DestructionCommand> &lastCommand, const DestructionCommand &command) {
            if (!lastCommand.has_value())
                return Result<bool>::Success(false);
            const auto incoming = command.Header().id.value.Value();
            const auto committed = lastCommand->Header().id.value.Value();
            if (incoming > committed)
                return Result<bool>::Success(false);
            if (incoming == committed && command == *lastCommand)
                return Result<bool>::Success(true);
            return Failure<bool>(DestructionErrors::DuplicateCommand);
        }

        /** @brief Applies the descriptor's minimum separation only to new damage decisions. */
        [[nodiscard]] Result<void> ValidateCooldown(const DestructionStateCommand &command, const DestructibleDescriptor &descriptor,
                                                    const std::optional<std::uint64_t> lastDamageTick, const std::uint64_t tick) {
            if (command.Kind() != DestructionStateCommandKind::ApplyDamage || !lastDamageTick.has_value())
                return Result<void>::Success();
            if (tick < *lastDamageTick)
                return Failure<void>(DestructionErrors::CommandInvalid);
            if (tick - *lastDamageTick < descriptor.Data().behavior.minimumDamageIntervalTicks)
                return Failure<void>(DestructionErrors::DamageCooldownActive);
            return Result<void>::Success();
        }

        /** @brief Compares detached and freshly prepared semantic evidence before publication. */
        [[nodiscard]] bool SameTransition(const DestructionStateTransition &left, const DestructionStateTransition &right) noexcept {
            return left.Command() == right.Command() && left.Source() == right.Source() && left.Successor() == right.Successor() &&
                   left.Status() == right.Status();
        }
    }  // namespace

    DestructionDamageTransition::DestructionDamageTransition(DestructionCommand command, DestructionStateTransition stateTransition,
                                                             const std::uint64_t tick, const DestructionDamageSafePoint safePoint) noexcept
        : command_(std::move(command)), stateTransition_(std::move(stateTransition)), tick_(tick), safePoint_(safePoint) {}

    /** @copydoc DestructionDamageTransition::Command */
    const DestructionCommand &DestructionDamageTransition::Command() const noexcept {
        return command_;
    }

    /** @copydoc DestructionDamageTransition::StateTransition */
    const DestructionStateTransition &DestructionDamageTransition::StateTransition() const noexcept {
        return stateTransition_;
    }

    /** @copydoc DestructionDamageTransition::Tick */
    std::uint64_t DestructionDamageTransition::Tick() const noexcept {
        return tick_;
    }

    /** @copydoc DestructionDamageTransition::SafePoint */
    DestructionDamageSafePoint DestructionDamageTransition::SafePoint() const noexcept {
        return safePoint_;
    }

    /** @copydoc DestructionDamageTransition::Cancel */
    DestructionDamageTransition DestructionDamageTransition::Cancel() const noexcept {
        auto cancelled = *this;
        cancelled.stateTransition_ = stateTransition_.Cancel();
        return cancelled;
    }

    DestructionDamageRuntime::DestructionDamageRuntime(DestructibleDescriptor descriptor, DestructionStateMachine machine,
                                                       std::optional<DestructionCommand> lastCommand,
                                                       std::optional<std::uint64_t> lastCommittedTick,
                                                       std::optional<std::uint64_t> lastDamageTick,
                                                       std::optional<DestructionCommandResult> lastResult) noexcept
        : descriptor_(std::move(descriptor)), machine_(std::move(machine)), lastCommand_(std::move(lastCommand)),
          lastCommittedTick_(lastCommittedTick), lastDamageTick_(lastDamageTick), lastResult_(std::move(lastResult)) {}

    /** @copydoc DestructionDamageRuntime::Create */
    Result<DestructionDamageRuntime> DestructionDamageRuntime::Create(const DestructibleDescriptor &descriptor,
                                                                      const DestructionHandle target,
                                                                      const DestructionStateRevision initialRevision) {
        auto machine = DestructionStateMachine::Create(descriptor, target, initialRevision);
        if (machine.HasError())
            return Result<DestructionDamageRuntime>::Failure(machine.ErrorValue());
        return Result<DestructionDamageRuntime>::Success(
            DestructionDamageRuntime{descriptor, std::move(machine).Value(), std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    }

    /** @copydoc DestructionDamageRuntime::Snapshot */
    const DestructionStateSnapshot &DestructionDamageRuntime::Snapshot() const noexcept {
        return machine_.Snapshot();
    }

    /** @copydoc DestructionDamageRuntime::LastResult */
    const std::optional<DestructionCommandResult> &DestructionDamageRuntime::LastResult() const noexcept {
        return lastResult_;
    }

    /** @copydoc DestructionDamageRuntime::IsAdmissionOpen */
    bool DestructionDamageRuntime::IsAdmissionOpen() const noexcept {
        return machine_.IsAdmissionOpen();
    }

    /** @copydoc DestructionDamageRuntime::Prepare */
    Result<DestructionDamageTransition> DestructionDamageRuntime::Prepare(const DestructionCommand &command,
                                                                          const DestructionDamageContext &context) const {
        if (!machine_.IsAdmissionOpen())
            return Failure<DestructionDamageTransition>(DestructionErrors::ShutdownInProgress);
        if (const auto validation = ValidateContext(command, context, Snapshot()); validation.HasError())
            return Result<DestructionDamageTransition>::Failure(validation.ErrorValue());
        const auto retry = IsExactRetry(lastCommand_, command);
        if (retry.HasError())
            return Result<DestructionDamageTransition>::Failure(retry.ErrorValue());
        if (!retry.Value()) {
            if (lastCommittedTick_.has_value() && context.simulationTick < *lastCommittedTick_)
                return Failure<DestructionDamageTransition>(DestructionErrors::CommandInvalid);
            if (const auto admission = AdmitDestructionCommand(command, Snapshot(), descriptor_, context.capabilityRevision,
                                                               context.authority, context.limits, true);
                admission.HasError())
                return Result<DestructionDamageTransition>::Failure(admission.ErrorValue());
        }
        auto stateCommand = ToDestructionStateCommand(command);
        if (stateCommand.HasError())
            return Result<DestructionDamageTransition>::Failure(stateCommand.ErrorValue());
        if (!retry.Value()) {
            if (const auto cooldown = ValidateCooldown(stateCommand.Value(), descriptor_, lastDamageTick_, context.simulationTick);
                cooldown.HasError())
                return Result<DestructionDamageTransition>::Failure(cooldown.ErrorValue());
        }
        auto prepared = machine_.Prepare(stateCommand.Value());
        if (prepared.HasError())
            return Result<DestructionDamageTransition>::Failure(prepared.ErrorValue());
        return Result<DestructionDamageTransition>::Success(
            DestructionDamageTransition{command, std::move(prepared).Value(), context.simulationTick, context.safePoint});
    }

    /** @copydoc DestructionDamageRuntime::Commit */
    Result<DestructionDamageRuntime> DestructionDamageRuntime::Commit(const DestructionDamageTransition &transition,
                                                                      const DestructionDamageContext &context) const {
        if (transition.StateTransition().Status() == DestructionTransitionStatus::Cancelled)
            return Failure<DestructionDamageRuntime>(DestructionErrors::CancelledBeforeCommit);
        if (!machine_.IsAdmissionOpen())
            return Failure<DestructionDamageRuntime>(DestructionErrors::ShutdownInProgress);
        if (context.safePoint != DestructionDamageSafePoint::PrePhysics && context.safePoint != DestructionDamageSafePoint::PostPhysics)
            return Failure<DestructionDamageRuntime>(DestructionErrors::CommandInvalid);
        if (context.simulationTick < transition.Tick())
            return Failure<DestructionDamageRuntime>(DestructionErrors::CommandInvalid);
        auto preparedContext = context;
        preparedContext.simulationTick = transition.Tick();
        preparedContext.safePoint = transition.SafePoint();
        auto canonical = Prepare(transition.Command(), preparedContext);
        if (canonical.HasError())
            return Result<DestructionDamageRuntime>::Failure(canonical.ErrorValue());
        if (canonical.Value().StateTransition().Status() == DestructionTransitionStatus::Duplicate) {
            if (lastCommand_.has_value() && transition.Command() == *lastCommand_)
                return Result<DestructionDamageRuntime>::Success(*this);
            return Failure<DestructionDamageRuntime>(DestructionErrors::StateInvalid);
        }
        if (transition.Tick() != canonical.Value().Tick() || transition.SafePoint() != canonical.Value().SafePoint() ||
            !SameTransition(transition.StateTransition(), canonical.Value().StateTransition()))
            return Failure<DestructionDamageRuntime>(DestructionErrors::StateInvalid);
        auto committed = machine_.Commit(transition.StateTransition());
        if (committed.HasError())
            return Result<DestructionDamageRuntime>::Failure(committed.ErrorValue());
        auto result = DestructionCommandResult::Succeeded(transition.Command().Header().id, transition.StateTransition().Source().revision,
                                                          committed.Value().Snapshot().revision);
        if (result.HasError())
            return Result<DestructionDamageRuntime>::Failure(result.ErrorValue());
        auto tick = lastDamageTick_;
        if (transition.StateTransition().Command().Kind() == DestructionStateCommandKind::ApplyDamage)
            tick = transition.Tick();
        return Result<DestructionDamageRuntime>::Success(DestructionDamageRuntime{descriptor_, std::move(committed).Value(),
                                                                                  transition.Command(), transition.Tick(), tick,
                                                                                  std::move(result).Value()});
    }

    /** @copydoc DestructionDamageRuntime::Replace */
    Result<DestructionDamageRuntime> DestructionDamageRuntime::Replace(const DestructionHandle replacement,
                                                                       const DestructibleDescriptor &descriptor) const {
        auto machine = machine_.Replace(replacement, descriptor);
        if (machine.HasError())
            return Result<DestructionDamageRuntime>::Failure(machine.ErrorValue());
        return Result<DestructionDamageRuntime>::Success(
            DestructionDamageRuntime{descriptor, std::move(machine).Value(), std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    }

    /** @copydoc DestructionDamageRuntime::BeginShutdown */
    DestructionDamageRuntime DestructionDamageRuntime::BeginShutdown() const noexcept {
        auto closed = *this;
        closed.machine_ = machine_.BeginShutdown();
        return closed;
    }
}  // namespace Horo::Destruction
