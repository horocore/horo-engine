#pragma once

/** @file RuntimeUiClient.h
 * @brief Revocable gameplay/script access to application-owned canonical Runtime UI owners.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Ui/UiBindingStore.h"
#include "Horo/Runtime/Ui/UiEventDispatch.h"
#include "Horo/Runtime/Ui/UiScreenStack.h"

#include <memory>

namespace Horo::Runtime::Ui {
    namespace Detail {
        struct RuntimeUiScope;
        struct RuntimeUiAccess;
    }  // namespace Detail

    /** @brief Independent explicit host permissions; admission never discovers a UI service. */
    enum class RuntimeUiPermission : std::uint8_t {
        Elements = 1,
        Screens = 2,
        ReadBindings = 4,
        PublishBindings = 8,
        Actions = 16,
        Events = 32
    };

    /** @brief Combines explicit grants. @param a Existing grants. @param b Additional grants. @return Union. */
    [[nodiscard]] constexpr RuntimeUiPermission operator|(RuntimeUiPermission a, RuntimeUiPermission b) noexcept {
        return static_cast<RuntimeUiPermission>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
    }

    /** @brief Exact principal and routing copied at host admission; zero/empty identities never select defaults. */
    struct RuntimeUiGrant final {
        std::string moduleId;
        std::uint64_t scene{};
        std::uint64_t sceneGeneration{};
        RuntimeUiInstanceId instance;
        UiCanvasId canvas;
        RuntimeUiPermission permissions{};
        std::vector<UiBindingProviderInstanceId> providers; /**< Explicit writable provider inventory, at most sixteen. */
    };

    /** @brief Scope-local never-reused command identity; not valid in another client or child scope. */
    struct RuntimeUiRequestId final {
        std::uint64_t value{};
        auto operator<=>(const RuntimeUiRequestId &) const = default;
    };

    /** @brief One complete owned provider delta; queued spans never borrow module or script storage. */
    struct RuntimeUiProviderUpdate final {
        UiBindingProviderInstanceId provider;
        UiBindingSchemaVersion schema;
        UiBindingSnapshotRevision expected;
        UiBindingSnapshotRevision revision;
        std::vector<UiBindingPropertyUpdate> changes; /**< At most sixteen entries and 4096 aggregate value bytes. */
    };

    /** @brief Closed terminal command result preserving canonical owner outcomes and errors. */
    using RuntimeUiCommandResult = std::variant<UiRouteOperationResult, UiBindingApplyResult, UiActionRequestId, Error>;

    /** @brief Copied target-phase notification; no handler, widget, input object or VM reference is retained. */
    struct RuntimeUiEvent final {
        UiElementHandle target;
        UiRoutedEvent event;
    };

    /** @brief Host-issued exact-generation capability shared by native gameplay and scripts.
     * @details Operations run on the UI owner thread and are bounded by sixteen pending commands and sixty-four events per scope.
     * Queues retain owned values only. Derive is load-time, at most eight levels; its immutable private issuer allocates both
     * object and control block in the host image even when invoked by native module code. No module-supplied allocator or
     * callback exists. Revoke is an any-thread atomic fence; descendants, queued commands and action dispatch fail closed.
     * Successful UI replacement invalidates exact-generation leases. Host/service shutdown never waits for retained clients.
     */
    class RuntimeUiClient final {
    public:
        /** @brief Nonaggregate factory key; callers cannot construct an admitted client. */
        class ConstructionKey final {
            friend struct Detail::RuntimeUiAccess;

            ConstructionKey() noexcept {}
        };

        /** @brief Host-issuer-only construction; opaque state never exposes a mutable owner. */
        RuntimeUiClient(ConstructionKey, std::shared_ptr<Detail::RuntimeUiScope> scope) noexcept;
        ~RuntimeUiClient();
        RuntimeUiClient(const RuntimeUiClient &) = delete;
        RuntimeUiClient &operator=(const RuntimeUiClient &) = delete;
        RuntimeUiClient(RuntimeUiClient &&) = delete;
        RuntimeUiClient &operator=(RuntimeUiClient &&) = delete;
        /** @brief Returns copied-at-admission immutable principal/routing evidence. @return Exact grant. */
        [[nodiscard]] const RuntimeUiGrant &Grant() const noexcept;
        /** @brief Checks owner thread, generation and cancellation without mutation. @return Success or typed failure. */
        [[nodiscard]] Result<void> Validate() const;
        /** @brief Creates an independently revocable descendant through the original host issuer.
         * @return Host-owned child or typed capacity/revocation failure.
         * @details Cancellation is inherited from the immutable host-issued parent. Module callers cannot supply a token,
         * allocator, callback or other module-owned control block that could survive native image retirement.
         */
        [[nodiscard]] Result<std::shared_ptr<const RuntimeUiClient>> Derive() const;
        /** @brief Permanently closes this scope and its descendants before callbacks or image retirement. */
        void Revoke() const noexcept;
        /** @brief Resolves an authored element in the admitted canvas. @param id Stable identity. @return Exact handle or canonical error.
         */
        [[nodiscard]] Result<UiElementHandle> Find(UiElementId id) const;
        /** @brief Copies the committed binding value without invoking any provider.
         * @param id Exact binding identity. @return Owned value or typed missing/permission/stale failure; text copies are bounded.
         */
        [[nodiscard]] Result<UiBindingValue> Read(UiBindingId id) const;
        /** @brief Captures the canonical route guard for guarded navigation. @return Exact guard or typed unavailable/permission failure.
         */
        [[nodiscard]] Result<UiRouteStackGuard> ScreenGuard() const;
        /** @brief Queues screen control at the next lifecycle cutoff. @param request Canonical route command.
         * @return Scope-local request identity or typed failure. Caller supplies exact stack guards, never a raw owner.
         */
        [[nodiscard]] Result<RuntimeUiRequestId> Screen(UiRouteOperationRequest request) const;
        /** @brief Queues a complete owned provider delta for the before-VariableUpdate cutoff.
         * @param update Existing host-admitted provider and owned bounded values. @return Request identity or typed failure.
         * @details Schema/authority registration remains inert host composition in the canonical UiBindingStore.
         */
        [[nodiscard]] Result<RuntimeUiRequestId> Publish(RuntimeUiProviderUpdate update) const;
        /** @brief Creates copied action-source evidence from this exact canvas without exposing its owner.
         * @param element Exact resident handle. @param interaction Expected presented interaction revision.
         * @return Canonical source or typed stale/permission failure; caller cannot manufacture presentation admission.
         */
        [[nodiscard]] Result<UiActionSource> Source(UiElementHandle element, UiInteractionRevision interaction) const;
        /** @brief Queues a canonical action; revocation is checked again before consumer dispatch.
         * @param source Exact presented-source evidence. @param command Typed bounded command. @return Request identity or typed failure.
         */
        [[nodiscard]] Result<RuntimeUiRequestId> Action(UiActionSource source, UiActionCommand command) const;
        /** @brief Nonblocking consume-on-terminal command observation.
         * @param request Identity returned by this exact scope. @return Empty while queued, terminal value/error, or typed stale failure.
         */
        [[nodiscard]] Result<std::optional<RuntimeUiCommandResult>> Poll(RuntimeUiRequestId request) const;
        /** @brief Removes one copied target-phase event without allocation. @return Empty or notification; typed revoked/capacity errors.
         */
        [[nodiscard]] Result<std::optional<RuntimeUiEvent>> NextEvent() const;

    private:
        friend struct Detail::RuntimeUiAccess;
        std::shared_ptr<Detail::RuntimeUiScope> scope_;
    };
}  // namespace Horo::Runtime::Ui
