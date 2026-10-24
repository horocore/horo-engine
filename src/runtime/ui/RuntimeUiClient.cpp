#include "Horo/Runtime/Ui/RuntimeUiClient.h"

#include "RuntimeUiAccessState.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Runtime::Ui::Detail {
    namespace {
        /** @brief Counts only the closed value alternatives that own variable storage before frame admission/copy. */
        std::size_t ValueBytes(const UiBindingValue &value) noexcept {
            if (const auto *text = std::get_if<std::string>(&value))
                return text->size();
            if (const auto *message = std::get_if<UiBindingLocalizedMessage>(&value))
                return message->key.size();
            if (const auto *reference = std::get_if<UiBindingReference>(&value))
                return reference->value.size();
            return 0;
        }
    }  // namespace

    /** @brief Reject foreign threads before reading owner-lane state, including during synchronous callbacks. */
    Result<void> CheckOwner(const RuntimeUiServiceState &owner) {
        if (owner.thread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(UiErrors::CapabilityWrongThread));
        if (!owner.active || owner.shutdownRequested)
            return Result<void>::Failure(MakeError(UiErrors::CapabilityRevoked));
        if (owner.dispatching || owner.scenePublication)
            return Result<void>::Failure(MakeError(UiErrors::CapabilityBusy));
        return Result<void>::Success();
    }

    /** @brief Retains typed missing-service behavior for a valid moved-from application facade. */
    Result<void> CheckOwner(const std::shared_ptr<RuntimeUiServiceState> &owner) {
        if (!owner)
            return Result<void>::Failure(MakeError(UiErrors::CapabilityUnavailable));
        return CheckOwner(*owner);
    }

    /** @brief Walk at most eight immutable host-issued scope ancestors; cancellation invokes no callback. */
    bool Cancelled(const RuntimeUiScope &scope) noexcept {
        for (auto current = &scope; current; current = current->parent.get())
            if (current->revoked.load() || current->cancellation.IsCancellationRequested())
                return true;
        return false;
    }

    /** @brief Synchronous borrow only; the caller has already validated owner lane and exact publication. */
    UiReloadCanvas *Canvas(RuntimeUiServiceState &owner, const RuntimeUiGrant &grant) noexcept {
        auto *publisher = owner.registry.Publisher(grant.instance);
        auto *generation = publisher ? publisher->Current() : nullptr;
        return generation ? generation->Canvas(grant.canvas) : nullptr;
    }

    /** @brief Preserve canonical owner failures while fencing stale scopes before any mutable borrow. */
    Result<std::shared_ptr<RuntimeUiServiceState>> CheckScope(const RuntimeUiScope &scope, const RuntimeUiPermission permission) {
        auto owner = scope.owner.lock();
        if (!owner)
            return Result<std::shared_ptr<RuntimeUiServiceState>>::Failure(MakeError(UiErrors::CapabilityRevoked));
        if (auto checked = CheckOwner(*owner); checked.HasError())
            return Result<std::shared_ptr<RuntimeUiServiceState>>::Failure(checked.ErrorValue());
        if (const auto *publisher = owner->registry.Publisher(scope.grant.instance);
            Cancelled(scope) || !publisher || !publisher->IsCurrent(scope.lease))
            return Result<std::shared_ptr<RuntimeUiServiceState>>::Failure(MakeError(UiErrors::CapabilityRevoked));
        if (const auto mask = static_cast<unsigned>(permission); (static_cast<unsigned>(scope.grant.permissions) & mask) != mask)
            return Result<std::shared_ptr<RuntimeUiServiceState>>::Failure(MakeError(UiErrors::CapabilityDenied));
        return Result<std::shared_ptr<RuntimeUiServiceState>>::Success(std::move(owner));
    }

    /** @brief Admit a command into a fixed mailbox; terminal slots stay reserved until explicitly consumed. */
    Result<RuntimeUiRequestId> Queue(RuntimeUiScope &scope, const RuntimeUiPermission permission, Command command) {
        if (auto owner = CheckScope(scope, permission); owner.HasError())
            return Result<RuntimeUiRequestId>::Failure(owner.ErrorValue());
        const auto slot = std::ranges::find_if(scope.commands, [](const auto &entry) {
            return entry.id.value == 0;
        });
        if (slot == scope.commands.end())
            return Result<RuntimeUiRequestId>::Failure(MakeError(UiErrors::CapacityExceeded));
        if (scope.sequence == std::numeric_limits<std::uint64_t>::max())
            return Result<RuntimeUiRequestId>::Failure(MakeError(UiErrors::GenerationExhausted));
        slot->command.emplace(std::move(command));
        slot->id = {++scope.sequence};
        return Result<RuntimeUiRequestId>::Success(slot->id);
    }
}  // namespace Horo::Runtime::Ui::Detail

namespace Horo::Runtime::Ui {
    /** @copydoc RuntimeUiClient::RuntimeUiClient */
    RuntimeUiClient::RuntimeUiClient(ConstructionKey, std::shared_ptr<Detail::RuntimeUiScope> scope) noexcept : scope_(std::move(scope)) {}

    RuntimeUiClient::~RuntimeUiClient() {
        Revoke();
    }

    /** @copydoc RuntimeUiClient::Grant */
    const RuntimeUiGrant &RuntimeUiClient::Grant() const noexcept {
        return scope_->grant;
    }

    /** @copydoc RuntimeUiClient::Validate */
    Result<void> RuntimeUiClient::Validate() const {
        if (auto owner = Detail::CheckScope(*scope_); owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiClient::Derive */
    Result<std::shared_ptr<const RuntimeUiClient>> RuntimeUiClient::Derive() const {
        auto owner = Detail::CheckScope(*scope_);
        if (owner.HasError())
            return Detail::ClientResult::Failure(owner.ErrorValue());
        return owner.Value()->issue(owner.Value(), scope_->grant, scope_, {});
    }

    /** @copydoc RuntimeUiClient::Revoke */
    void RuntimeUiClient::Revoke() const noexcept {
        scope_->revoked.store(true);
    }

    /** @copydoc RuntimeUiClient::Find */
    Result<UiElementHandle> RuntimeUiClient::Find(const UiElementId id) const {
        auto owner = Detail::CheckScope(*scope_, RuntimeUiPermission::Elements);
        if (owner.HasError())
            return Result<UiElementHandle>::Failure(owner.ErrorValue());
        return Detail::Canvas(*owner.Value(), scope_->grant)->tree.Find(id);
    }

    /** @copydoc RuntimeUiClient::Read */
    Result<UiBindingValue> RuntimeUiClient::Read(const UiBindingId id) const {
        auto owner = Detail::CheckScope(*scope_, RuntimeUiPermission::ReadBindings);
        if (owner.HasError())
            return Result<UiBindingValue>::Failure(owner.ErrorValue());
        auto *canvas = Detail::Canvas(*owner.Value(), scope_->grant);
        const auto *value = canvas->bindings ? canvas->bindings->Find(canvas->tree, id) : nullptr;
        if (!value)
            return Result<UiBindingValue>::Failure(MakeError(UiErrors::CapabilityUnavailable));
        if (Detail::ValueBytes(value->value) > 4096)
            return Result<UiBindingValue>::Failure(MakeError(UiErrors::CapabilityUnsupported));
        try {
            return Result<UiBindingValue>::Success(value->value);
        } catch (const std::bad_alloc &) {
            return Result<UiBindingValue>::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }

    /** @copydoc RuntimeUiClient::ScreenGuard */
    Result<UiRouteStackGuard> RuntimeUiClient::ScreenGuard() const {
        auto owner = Detail::CheckScope(*scope_, RuntimeUiPermission::Screens);
        if (owner.HasError())
            return Result<UiRouteStackGuard>::Failure(owner.ErrorValue());
        auto *canvas = Detail::Canvas(*owner.Value(), scope_->grant);
        return canvas->routes ? canvas->routes->Guard() : Result<UiRouteStackGuard>::Failure(MakeError(UiErrors::CapabilityUnavailable));
    }

    /** @copydoc RuntimeUiClient::Screen */
    Result<RuntimeUiRequestId> RuntimeUiClient::Screen(UiRouteOperationRequest request) const {
        return Detail::Queue(*scope_, RuntimeUiPermission::Screens, std::move(request));
    }

    /** @copydoc RuntimeUiClient::Publish */
    Result<RuntimeUiRequestId> RuntimeUiClient::Publish(RuntimeUiProviderUpdate update) const {
        if (auto owner = Detail::CheckScope(*scope_, RuntimeUiPermission::PublishBindings); owner.HasError())
            return Result<RuntimeUiRequestId>::Failure(owner.ErrorValue());
        if (std::ranges::find(scope_->grant.providers, update.provider) == scope_->grant.providers.end())
            return Result<RuntimeUiRequestId>::Failure(MakeError(UiErrors::CapabilityDenied));
        if (update.changes.empty() || update.changes.size() > 16)
            return Result<RuntimeUiRequestId>::Failure(MakeError(UiErrors::CapabilityUnsupported));
        std::size_t bytes{};
        for (const auto &change : update.changes) {
            const auto size = Detail::ValueBytes(change.value);
            if (size > 4096 - bytes)
                return Result<RuntimeUiRequestId>::Failure(MakeError(UiErrors::CapabilityUnsupported));
            bytes += size;
        }
        return Detail::Queue(*scope_, RuntimeUiPermission::PublishBindings, std::move(update));
    }

    /** @copydoc RuntimeUiClient::Source */
    Result<UiActionSource> RuntimeUiClient::Source(const UiElementHandle element, const UiInteractionRevision interaction) const {
        auto owner = Detail::CheckScope(*scope_, RuntimeUiPermission::Actions);
        if (owner.HasError())
            return Result<UiActionSource>::Failure(owner.ErrorValue());
        auto *canvas = Detail::Canvas(*owner.Value(), scope_->grant);
        if (!canvas->actions)
            return Result<UiActionSource>::Failure(MakeError(UiErrors::CapabilityUnavailable));
        if (auto resident = canvas->tree.Get(element); resident.HasError())
            return Result<UiActionSource>::Failure(resident.ErrorValue());
        if (canvas->actions->Owner().interaction != interaction)
            return Result<UiActionSource>::Failure(MakeError(UiErrors::RevisionStale));
        return Result<UiActionSource>::Success({canvas->actions->Owner(), element});
    }

    /** @copydoc RuntimeUiClient::Action */
    Result<RuntimeUiRequestId> RuntimeUiClient::Action(UiActionSource source, UiActionCommand command) const {
        if (auto valid = ValidateUiActionCommand(command); valid.HasError())
            return Result<RuntimeUiRequestId>::Failure(valid.ErrorValue());
        return Detail::Queue(*scope_, RuntimeUiPermission::Actions, Detail::ActionCommand{std::move(source), std::move(command)});
    }

    /** @copydoc RuntimeUiClient::Poll */
    Result<std::optional<RuntimeUiCommandResult>> RuntimeUiClient::Poll(const RuntimeUiRequestId request) const {
        using Output = Result<std::optional<RuntimeUiCommandResult>>;
        if (auto owner = Detail::CheckScope(*scope_); owner.HasError())
            return Output::Failure(owner.ErrorValue());
        for (auto &entry : scope_->commands) {
            if (entry.id != request || !request.value)
                continue;
            if (!entry.terminal)
                return Output::Success(std::nullopt);
            auto terminal = std::move(entry.terminal);
            entry = {};
            return Output::Success(std::move(terminal));
        }
        return Output::Failure(MakeError(UiErrors::HandleStale));
    }

    /** @copydoc RuntimeUiClient::NextEvent */
    Result<std::optional<RuntimeUiEvent>> RuntimeUiClient::NextEvent() const {
        using Output = Result<std::optional<RuntimeUiEvent>>;
        if (auto owner = Detail::CheckScope(*scope_, RuntimeUiPermission::Events); owner.HasError())
            return Output::Failure(owner.ErrorValue());
        if (!scope_->eventCount)
            return Output::Success(std::nullopt);
        const auto event = scope_->events[scope_->eventBegin];
        scope_->eventBegin = (scope_->eventBegin + 1) % Detail::EventLimit;
        --scope_->eventCount;
        return Output::Success(event);
    }
}  // namespace Horo::Runtime::Ui
