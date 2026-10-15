#include "Horo/Runtime/Ui/RuntimeUiService.h"

#include "RuntimeUiAccessState.h"

#include <algorithm>
#include <new>

namespace Horo::Runtime::Ui::Detail {
    namespace {
        /** @brief Validate finite explicit policy before allocating host-owned scope state. */
        bool ValidGrant(const RuntimeUiGrant &grant) noexcept {
            const auto mask = static_cast<unsigned>(grant.permissions);
            for (std::size_t index = 0; index < grant.providers.size(); ++index) {
                if (!grant.providers[index].IsValid())
                    return false;
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (grant.providers[prior] == grant.providers[index])
                        return false;
            }
            return grant.moduleId.starts_with("game.") && grant.moduleId.size() <= 160 && grant.scene && grant.sceneGeneration &&
                   grant.instance.IsValid() && grant.canvas.IsValid() && mask && mask <= 63 && grant.providers.size() <= 16 &&
                   std::ranges::all_of(grant.moduleId, [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
            });
        }

        /** @brief Reserve one dispatcher once per exact canvas incarnation, never in the frame path. */
        Result<void> PrepareDispatcher(RuntimeUiServiceState &owner, const RuntimeUiGrant &grant, const UiReloadCanvas &canvas,
                                       const UiReloadLease &lease) {
            for (const auto &entry : owner.dispatchers) {
                if (entry.events && entry.instance == grant.instance && entry.canvas == grant.canvas &&
                    entry.incarnation == canvas.tree.Canvas() && entry.lease.Get() == lease.Get())
                    return Result<void>::Success();
            }
            auto slot = std::ranges::find_if(owner.dispatchers, [](const auto &entry) {
                return !entry.events;
            });
            if (slot == owner.dispatchers.end())
                return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
            auto events =
                UiEventDispatcher::Create({grant.instance, canvas.tree.Canvas(), canvas.tree.SourceDocument(), canvas.tree.Size()});
            if (events.HasError())
                return Result<void>::Failure(events.ErrorValue());
            *slot = {grant.instance, grant.canvas, canvas.tree.Canvas(), lease, std::move(events).Value()};
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc FinishShutdown */
    void FinishShutdown(RuntimeUiServiceState &owner) noexcept {
        owner.active = false;
        owner.registry.Shutdown();
        for (auto &dispatcher : owner.dispatchers)
            dispatcher = {};
        for (auto &action : owner.actions)
            action = {};
    }

    /** @brief Immutable host issuer route: module-emitted Derive frontends can never select its allocator or deleter. */
    ClientResult RuntimeUiAccess::Issue(const std::shared_ptr<RuntimeUiServiceState> &owner, const RuntimeUiGrant &grant,
                                        std::shared_ptr<const RuntimeUiScope> parent, const CancellationToken &cancellation) {
        if (auto valid = CheckOwner(*owner); valid.HasError())
            return ClientResult::Failure(valid.ErrorValue());
        if (!ValidGrant(grant))
            return ClientResult::Failure(MakeError(UiErrors::CapabilityDenied));
        if (cancellation.IsCancellationRequested())
            return ClientResult::Failure(MakeError(UiErrors::CapabilityRevoked));
        std::size_t depth{};
        for (auto p = parent.get(); p; p = p->parent.get()) {
            if (Cancelled(*p))
                return ClientResult::Failure(MakeError(UiErrors::CapabilityRevoked));
            if (++depth >= 8)
                return ClientResult::Failure(MakeError(UiErrors::CapacityExceeded));
        }
        auto slot = std::ranges::find_if(owner->scopes, [](const auto &entry) {
            return entry.expired();
        });
        if (slot == owner->scopes.end())
            return ClientResult::Failure(MakeError(UiErrors::CapacityExceeded));
        auto descriptor = owner->registry.Describe(grant.instance);
        if (descriptor.HasError())
            return ClientResult::Failure(descriptor.ErrorValue());
        if (descriptor.Value().owner.scene && descriptor.Value().owner.scene->value != grant.scene)
            return ClientResult::Failure(MakeError(UiErrors::HandleOwnerMismatch));
        if (descriptor.Value().providerScene && descriptor.Value().providerScene->value != grant.scene)
            return ClientResult::Failure(MakeError(UiErrors::HandleOwnerMismatch));
        const auto *publisher = owner->registry.Publisher(grant.instance);
        auto lease = publisher ? publisher->Acquire() : Result<UiReloadLease>::Failure(MakeError(UiErrors::CapabilityUnavailable));
        if (lease.HasError())
            return ClientResult::Failure(lease.ErrorValue());
        const auto *canvas = Canvas(*owner, grant);
        if (!canvas)
            return ClientResult::Failure(MakeError(UiErrors::CapabilityUnavailable));
        try {
            if (auto prepared = PrepareDispatcher(*owner, grant, *canvas, lease.Value()); prepared.HasError())
                return ClientResult::Failure(prepared.ErrorValue());
            auto scope = std::make_shared<RuntimeUiScope>(owner, grant, std::move(lease).Value(), std::move(parent), cancellation);
            auto client = std::make_shared<RuntimeUiClient>(RuntimeUiClient::ConstructionKey{}, scope);
            *slot = scope;
            return ClientResult::Success(std::move(client));
        } catch (const std::bad_alloc &) {
            return ClientResult::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Runtime::Ui::Detail

namespace Horo::Runtime::Ui {
    /** @copydoc RuntimeUiService::Create */
    Result<RuntimeUiService> RuntimeUiService::Create(UiSceneReconciliation registry) {
        try {
            return Result<RuntimeUiService>::Success(
                RuntimeUiService{std::make_shared<Detail::RuntimeUiServiceState>(std::move(registry), &Detail::RuntimeUiAccess::Issue)});
        } catch (const std::bad_alloc &) {
            return Result<RuntimeUiService>::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }

    RuntimeUiService::RuntimeUiService(std::shared_ptr<Detail::RuntimeUiServiceState> state) noexcept : state_(std::move(state)) {}

    RuntimeUiService::~RuntimeUiService() {
        Shutdown();
    }

    RuntimeUiService::RuntimeUiService(RuntimeUiService &&) noexcept = default;

    RuntimeUiService &RuntimeUiService::operator=(RuntimeUiService &&other) noexcept {
        if (this != &other) {
            Shutdown();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    /** @copydoc RuntimeUiService::Admit */
    Result<void> RuntimeUiService::Admit(  // NOSONAR(cpp:S5817) Transfers publisher ownership into the registry.
        const UiSceneInstanceDescriptor &descriptor, UiHotReload &&publisher, const UiStructuralCommitPoint point) {
        if (auto check = Detail::CheckOwner(state_); check.HasError())
            return check;
        return state_->registry.Admit(descriptor, std::move(publisher), point);
    }

    /** @copydoc RuntimeUiService::Issue */
    Result<std::shared_ptr<const RuntimeUiClient>> RuntimeUiService::Issue(  // NOSONAR(cpp:S5817) Reserves host scope and dispatcher
                                                                             // inventory.
        const RuntimeUiGrant &grant, const CancellationToken &cancellation) {
        if (auto check = Detail::CheckOwner(state_); check.HasError())
            return Detail::ClientResult::Failure(check.ErrorValue());
        return state_->issue(state_, grant, {}, cancellation);
    }

    /** @copydoc RuntimeUiService::Reload */
    Result<UiReloadReconciliation> RuntimeUiService::Reload(  // NOSONAR(cpp:S5817) Commits a generation and invalidates attribution.
        const RuntimeUiInstanceId instance, UiReloadGeneration replacement, const UiStructuralCommitPoint point,
        const CancellationToken &cancellation) {
        if (auto check = Detail::CheckOwner(state_); check.HasError())
            return Result<UiReloadReconciliation>::Failure(check.ErrorValue());
        auto *publisher = state_->registry.Publisher(instance);
        if (!publisher)
            return Result<UiReloadReconciliation>::Failure(MakeError(UiErrors::CapabilityUnavailable));
        auto candidate = publisher->Prepare(std::move(replacement), cancellation);
        if (candidate.HasError())
            return Result<UiReloadReconciliation>::Failure(candidate.ErrorValue());
        auto prepared = std::move(candidate).Value();
        auto committed = publisher->Commit(prepared, point);
        if (committed.HasError())
            return committed;
        for (auto &entry : state_->dispatchers)
            if (entry.instance == instance)
                entry = {};
        for (auto &entry : state_->actions)
            if (entry.instance == instance)
                entry = {};
        return committed;
    }

    /** @copydoc RuntimeUiService::CollectRetired */
    Result<std::size_t> RuntimeUiService::CollectRetired() {  // NOSONAR(cpp:S5817) Destroys retired canonical generations.
        if (!state_)
            return Result<std::size_t>::Failure(MakeError(UiErrors::CapabilityUnavailable));
        if (state_->thread != std::this_thread::get_id())
            return Result<std::size_t>::Failure(MakeError(UiErrors::CapabilityWrongThread));
        if (state_->dispatching)
            return Result<std::size_t>::Failure(MakeError(UiErrors::CapabilityBusy));
        return state_->registry.CollectRetired();
    }

    /** @copydoc RuntimeUiService::Shutdown */
    void RuntimeUiService::Shutdown() noexcept {  // NOSONAR(cpp:S5817) Revokes grants and retires publisher ownership.
        if (!state_ || !state_->active || state_->thread != std::this_thread::get_id())
            return;
        for (const auto &entry : state_->scopes)
            if (auto scope = entry.lock())
                scope->revoked.store(true);
        state_->shutdownRequested = true;
        if (state_->dispatching)
            return;
        Detail::FinishShutdown(*state_);
    }
}  // namespace Horo::Runtime::Ui
