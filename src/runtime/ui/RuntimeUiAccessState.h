#pragma once

#include "Horo/Runtime/Ui/RuntimeUiService.h"

#include <array>
#include <atomic>
#include <functional>
#include <thread>

namespace Horo::Runtime::Ui::Detail {
    inline constexpr std::size_t ScopeLimit = 256;
    inline constexpr std::size_t CommandLimit = 16;
    inline constexpr std::size_t EventLimit = 64;
    inline constexpr std::size_t ActionLimit = 128;
    struct RuntimeUiServiceState;
    struct RuntimeUiScope;
    using ClientResult = Result<std::shared_ptr<const RuntimeUiClient>>;
    using ScopeIssuer = std::function<ClientResult(const std::shared_ptr<RuntimeUiServiceState> &, const RuntimeUiGrant &,
                                                   std::shared_ptr<const RuntimeUiScope>, const CancellationToken &)>;

    struct ActionCommand final {
        UiActionSource source;
        UiActionCommand command;
    };

    using Command = std::variant<UiRouteOperationRequest, RuntimeUiProviderUpdate, ActionCommand>;

    struct PendingCommand final {
        RuntimeUiRequestId id;
        std::optional<Command> command;
        std::optional<RuntimeUiCommandResult> terminal;
    };

    /** @brief Host-owned per-principal mailbox; only revoked is cross-thread mutable. No VM/module callback is stored. */
    struct RuntimeUiScope final {
        RuntimeUiScope(std::shared_ptr<RuntimeUiServiceState> owner, RuntimeUiGrant grant, UiReloadLease lease,
                       std::shared_ptr<const RuntimeUiScope> parent, CancellationToken cancellation)
            : owner(std::move(owner)), grant(std::move(grant)), lease(std::move(lease)), parent(std::move(parent)),
              cancellation(std::move(cancellation)) {}

        std::weak_ptr<RuntimeUiServiceState> owner;
        const RuntimeUiGrant grant;
        UiReloadLease lease;
        const std::shared_ptr<const RuntimeUiScope> parent;
        const CancellationToken cancellation;
        std::atomic_bool revoked{};
        std::uint64_t sequence{};
        std::array<PendingCommand, CommandLimit> commands;
        std::array<RuntimeUiEvent, EventLimit> events;
        std::size_t eventBegin{};
        std::size_t eventCount{};
    };

    struct Dispatcher final {
        RuntimeUiInstanceId instance;
        UiCanvasId canvas;
        UiCanvasInstanceId incarnation;
        UiReloadLease lease;
        std::optional<UiEventDispatcher> events;
    };

    struct PendingAction final {
        UiActionRequestId id;
        RuntimeUiInstanceId instance;
        UiCanvasId canvas;
        std::weak_ptr<RuntimeUiScope> scope;
        bool occupied{};
    };

    /** @brief Sole application registry owner; mutation and inventories are owner-thread-only. */
    struct RuntimeUiServiceState final {
        RuntimeUiServiceState(UiSceneReconciliation registry, ScopeIssuer issue) : registry(std::move(registry)), issue(std::move(issue)) {}

        UiSceneReconciliation registry;
        const std::thread::id thread{std::this_thread::get_id()};
        const ScopeIssuer issue;
        bool active{true};
        bool dispatching{};
        bool shutdownRequested{};
        std::array<std::weak_ptr<RuntimeUiScope>, ScopeLimit> scopes;
        std::array<Dispatcher, 64> dispatchers;
        std::array<PendingAction, ActionLimit> actions;
    };

    /** @brief The only factory-key authority, defined solely inside the host implementation target. */
    struct RuntimeUiAccess final {
        [[nodiscard]] static ClientResult Issue(const std::shared_ptr<RuntimeUiServiceState> &owner, const RuntimeUiGrant &grant,
                                                std::shared_ptr<const RuntimeUiScope> parent, const CancellationToken &cancellation);
    };

    /** @brief Releases internal generation leases after the final borrowed dispatch has unwound; external leases still fence reclaim. */
    void FinishShutdown(RuntimeUiServiceState &owner) noexcept;

    /** @brief Pins the host state across a borrowed callback and defers recursive service destruction until dispatch returns. */
    class RuntimeUiDispatchGuard final {
    public:
        explicit RuntimeUiDispatchGuard(std::shared_ptr<RuntimeUiServiceState> owner) noexcept : owner_(std::move(owner)) {
            owner_->dispatching = true;
        }

        ~RuntimeUiDispatchGuard() {
            owner_->dispatching = false;
            if (owner_->shutdownRequested)
                FinishShutdown(*owner_);
        }

        RuntimeUiDispatchGuard(const RuntimeUiDispatchGuard &) = delete;
        RuntimeUiDispatchGuard &operator=(const RuntimeUiDispatchGuard &) = delete;

    private:
        std::shared_ptr<RuntimeUiServiceState> owner_;
    };

    [[nodiscard]] Result<void> CheckOwner(const RuntimeUiServiceState &owner);
    [[nodiscard]] Result<void> CheckOwner(const std::shared_ptr<RuntimeUiServiceState> &owner);
    [[nodiscard]] bool Cancelled(const RuntimeUiScope &scope) noexcept;
    [[nodiscard]] Result<std::shared_ptr<RuntimeUiServiceState>> CheckScope(const RuntimeUiScope &scope,
                                                                            RuntimeUiPermission permission = {});
    [[nodiscard]] UiReloadCanvas *Canvas(RuntimeUiServiceState &owner, const RuntimeUiGrant &grant) noexcept;
    [[nodiscard]] Result<RuntimeUiRequestId> Queue(RuntimeUiScope &scope, RuntimeUiPermission permission, Command command);
    [[nodiscard]] RuntimeUiCommandResult Execute(RuntimeUiServiceState &owner, RuntimeUiScope &scope, Command &command);
}  // namespace Horo::Runtime::Ui::Detail
