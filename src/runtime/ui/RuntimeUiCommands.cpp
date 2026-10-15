#include "RuntimeUiAccessState.h"

#include <algorithm>

namespace Horo::Runtime::Ui::Detail {
    namespace {
        /** @brief Store original canonical outcomes without flattening committed rejections or error identity. */
        template <class T> RuntimeUiCommandResult Terminal(Result<T> result) {
            return result.HasError() ? RuntimeUiCommandResult{result.ErrorValue()} : RuntimeUiCommandResult{std::move(result).Value()};
        }

        /** @brief Reserve attribution before canonical enqueue so revoked requests can never invoke a later consumer. */
        RuntimeUiCommandResult EnqueueAction(RuntimeUiServiceState &owner, const RuntimeUiScope &scope, UiReloadCanvas &canvas,
                                             ActionCommand &command) {
            if (!canvas.actions)
                return MakeError(UiErrors::CapabilityUnavailable);
            const auto slot = std::ranges::find_if(owner.actions, [](const auto &entry) {
                return !entry.occupied;
            });
            if (slot == owner.actions.end())
                return MakeError(UiErrors::CapacityExceeded);
            auto admitted = canvas.actions->Enqueue(command.source, std::move(command.command));
            if (admitted.HasError())
                return admitted.ErrorValue();
            for (const auto &entry : owner.scopes) {
                auto pin = entry.lock();
                if (pin.get() == &scope) {
                    *slot = {admitted.Value(), scope.grant.instance, scope.grant.canvas, pin, true};
                    break;
                }
            }
            return admitted.Value();
        }

        /** @brief Select the next admitted command at this cutoff; revocation drains every family in original admission order. */
        PendingCommand *NextCommand(RuntimeUiScope &scope, const RuntimeUiCommandCutoff point, const bool revoked) noexcept {
            PendingCommand *next{};
            for (auto &pending : scope.commands) {
                if (!pending.command)
                    continue;
                if (const bool binding = std::holds_alternative<RuntimeUiProviderUpdate>(*pending.command);
                    !revoked && binding != (point == RuntimeUiCommandCutoff::BeforeVariableUpdate))
                    continue;
                if (!next || pending.id.value < next->id.value)
                    next = &pending;
            }
            return next;
        }
    }  // namespace

    /** @brief Execute one command at its named cutoff; all spans are borrowed only for this canonical owner call. */
    RuntimeUiCommandResult Execute(RuntimeUiServiceState &owner, const RuntimeUiScope &scope, Command &command) {
        auto *canvas = Canvas(owner, scope.grant);
        if (!canvas)
            return MakeError(UiErrors::CapabilityRevoked);
        if (const auto *screen = std::get_if<UiRouteOperationRequest>(&command)) {
            if (!canvas->routes)
                return MakeError(UiErrors::CapabilityUnavailable);
            return Terminal(canvas->routes->Navigate(*screen));
        }
        if (auto *update = std::get_if<RuntimeUiProviderUpdate>(&command)) {
            if (!canvas->bindings || !canvas->layoutEngine)
                return MakeError(UiErrors::CapabilityUnavailable);
            const UiBindingChangeBatch batch{update->provider, update->schema, update->expected, update->revision, update->changes};
            return Terminal(canvas->bindings->Apply(canvas->tree, std::span{&batch, 1}, *canvas->layoutEngine));
        }
        return EnqueueAction(owner, scope, *canvas, std::get<ActionCommand>(command));
    }
}  // namespace Horo::Runtime::Ui::Detail

namespace Horo::Runtime::Ui {
    /** @copydoc RuntimeUiService::ApplyQueuedCommands */
    Result<std::size_t> RuntimeUiService::ApplyQueuedCommands(  // NOSONAR(cpp:S5817) Consumes mailboxes and publishes owner commands.
        const RuntimeUiCommandCutoff point) {
        if (auto check = Detail::CheckOwner(state_); check.HasError())
            return Result<std::size_t>::Failure(check.ErrorValue());
        if (point != RuntimeUiCommandCutoff::OwnerCommands && point != RuntimeUiCommandCutoff::BeforeVariableUpdate)
            return Result<std::size_t>::Failure(MakeError(UiErrors::CapabilityUnsupported));
        std::size_t completed{};
        for (const auto &entry : state_->scopes) {
            auto scope = entry.lock();
            if (!scope)
                continue;
            const auto valid = Detail::CheckScope(*scope);
            // Admission order, rather than reusable storage-slot order, is the command ordering authority.
            for (std::size_t count = 0; count < Detail::CommandLimit; ++count) {
                auto *next = Detail::NextCommand(*scope, point, valid.HasError());
                if (!next)
                    break;
                next->terminal =
                    valid.HasError() ? RuntimeUiCommandResult{valid.ErrorValue()} : Detail::Execute(*state_, *scope, *next->command);
                next->command.reset();
                ++completed;
            }
        }
        return Result<std::size_t>::Success(completed);
    }

    /** @copydoc RuntimeUiService::DispatchActions */
    Result<std::size_t> RuntimeUiService::DispatchActions(  // NOSONAR(cpp:S5817) Consumes canonical actions and attribution slots.
        const RuntimeUiInstanceId instance, const UiCanvasId canvasId, UiActionHandler &handler, const std::size_t maximum) {
        const auto state = state_;
        if (auto check = Detail::CheckOwner(state); check.HasError())
            return Result<std::size_t>::Failure(check.ErrorValue());
        if (!maximum || maximum > Detail::ActionLimit)
            return Result<std::size_t>::Failure(MakeError(UiErrors::CapacityExceeded));
        auto *canvas = Detail::Canvas(*state, RuntimeUiGrant{.instance = instance, .canvas = canvasId});
        if (!canvas || !canvas->actions)
            return Result<std::size_t>::Failure(MakeError(UiErrors::CapabilityUnavailable));
        std::size_t dispatched{};
        for (std::size_t count = 0; count < maximum && state->active; ++count) {
            auto request = canvas->actions->TryDequeue();
            if (request.HasError())
                return Result<std::size_t>::Failure(request.ErrorValue());
            if (!request.Value())
                break;
            auto attribution = std::ranges::find_if(state->actions, [&request, instance, canvasId](const auto &entry) {
                return entry.occupied && entry.instance == instance && entry.canvas == canvasId && entry.id == request.Value()->id;
            });
            if (attribution == state->actions.end())
                continue;
            auto scope = attribution->scope.lock();
            attribution->occupied = false;
            if (!scope || Detail::CheckScope(*scope, RuntimeUiPermission::Actions).HasError())
                continue;
            if (auto result = [&canvas, &request, &handler, &state] {
                Detail::RuntimeUiDispatchGuard dispatch{state};
                return canvas->actions->Dispatch(*request.Value(), handler);
            }(); result.HasError())
                return Result<std::size_t>::Failure(result.ErrorValue());
            ++dispatched;
        }
        return Result<std::size_t>::Success(dispatched);
    }
}  // namespace Horo::Runtime::Ui
