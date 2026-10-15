#include "RuntimeUiAccessState.h"

#include <algorithm>

namespace Horo::Runtime::Ui {
    /** @copydoc RuntimeUiService::DispatchEvent */
    Result<UiEventDispatchResult> RuntimeUiService::DispatchEvent(  // NOSONAR(cpp:S5817) Dispatches consumers and fills client mailboxes.
        const UiCanvasId canvasId, const UiEventRoute &route, const UiRoutedEvent &event, UiEventHandler &handler) {
        using Output = Result<UiEventDispatchResult>;
        const auto state = state_;
        if (auto check = Detail::CheckOwner(state); check.HasError())
            return Output::Failure(check.ErrorValue());
        const auto *canvas = Detail::Canvas(*state, RuntimeUiGrant{.instance = route.instance, .canvas = canvasId});
        if (!canvas)
            return Output::Failure(MakeError(UiErrors::CapabilityUnavailable));
        const auto dispatcher = std::ranges::find_if(state->dispatchers, [&route, canvasId](const auto &entry) {
            return entry.events && entry.instance == route.instance && entry.canvas == canvasId && entry.incarnation == route.canvas;
        });
        if (dispatcher == state->dispatchers.end())
            return Output::Failure(MakeError(UiErrors::CapabilityUnavailable));
        // Preflight every mailbox before dispatch: backpressure never drops a notification after a consumer side effect.
        std::array<std::shared_ptr<Detail::RuntimeUiScope>, Detail::ScopeLimit> recipients;
        std::size_t size{};
        for (const auto &entry : state->scopes) {
            auto scope = entry.lock();
            if (!scope || scope->grant.instance != route.instance || scope->grant.canvas != canvasId ||
                Detail::CheckScope(*scope, RuntimeUiPermission::Events).HasError())
                continue;
            if (scope->eventCount == Detail::EventLimit)
                return Output::Failure(MakeError(UiErrors::CapacityExceeded));
            recipients[size++] = std::move(scope);
        }
        auto dispatched = [&] {
            Detail::RuntimeUiDispatchGuard dispatch{state};
            return dispatcher->events->Dispatch(canvas->tree, route, event, handler);
        }();
        if (dispatched.HasError())
            return dispatched;
        for (std::size_t index = 0; index < size; ++index) {
            auto &scope = *recipients[index];
            if (Detail::Cancelled(scope))
                continue;
            scope.events[(scope.eventBegin + scope.eventCount) % Detail::EventLimit] = {route.target, event};
            ++scope.eventCount;
        }
        return dispatched;
    }
}  // namespace Horo::Runtime::Ui
