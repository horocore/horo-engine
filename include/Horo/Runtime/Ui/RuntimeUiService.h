#pragma once

/** @file RuntimeUiService.h
 * @brief Explicit application authority composing existing Runtime UI publishers and scoped gameplay access.
 */

#include "Horo/Runtime/Ui/RuntimeUiClient.h"
#include "Horo/Runtime/Ui/UiAnimationOwner.h"
#include "Horo/Runtime/Ui/UiSceneReconciliation.h"

namespace Horo::Runtime::Ui {
    namespace Detail {
        struct RuntimeUiServiceState;
    }

    /** @brief Separate lifecycle and committed-binding publication cutoffs. */
    enum class RuntimeUiCommandCutoff : std::uint8_t {
        OwnerCommands,
        BeforeVariableUpdate
    };

    /** @brief Application-owned facade over the canonical semantic registry; never a second tree/binding/action authority.
     * @details Create/admit/issue/reload/collect are load-time operations. All mutable operations are serialized on the creating
     * thread; only client Revoke may run elsewhere. Client commands are applied only at named ADR-073 cutoffs. No callback is
     * stored: action/event handlers are borrowed for one non-reentrant owner dispatch. Shutdown fences clients before retiring
     * publishers; retained client/result destruction runs entirely in host-owned storage after native image unload.
     */
    class RuntimeUiService final {
    public:
        /** @brief Takes the sole canonical semantic registry and reserves fixed client/dispatch inventories.
         * @param registry Existing explicit application owner, moved into this facade. @return Owned service or typed capacity failure.
         */
        [[nodiscard]] static Result<RuntimeUiService> Create(UiSceneReconciliation registry);
        ~RuntimeUiService();
        RuntimeUiService(RuntimeUiService &&) noexcept;
        RuntimeUiService &operator=(RuntimeUiService &&) noexcept;
        RuntimeUiService(const RuntimeUiService &) = delete;
        RuntimeUiService &operator=(const RuntimeUiService &) = delete;
        /** @brief Delegates exact semantic admission to the sole canonical registry.
         * @param descriptor Host scope evidence. @param publisher Private actual publisher. @param point Lifecycle cutoff.
         * @return Original typed admission result; no clients can observe private candidates.
         */
        [[nodiscard]] Result<void> Admit(const UiSceneInstanceDescriptor &descriptor, UiHotReload &&publisher,
                                         UiStructuralCommitPoint point);
        /** @brief Issues one exact canvas grant without ambient lookup.
         * @param grant Explicit module/scene/canvas/permission policy. @param cancellation Host module-generation fence.
         * @return Host-owned opaque client or typed missing, denied, stale, thread or capacity failure.
         * @pre Only the explicit application composition owns this service and supplies host-owned cancellation storage;
         * a native module receives a client, never this factory or registry. Module-facing Derive inherits this fence.
         */
        [[nodiscard]] Result<std::shared_ptr<const RuntimeUiClient>> Issue(const RuntimeUiGrant &grant,
                                                                           const CancellationToken &cancellation = {});
        /** @brief Applies at most 256 times sixteen pending commands; cancelled scopes cannot mutate owners.
         * @param point Explicit owner cutoff; provider deltas require BeforeVariableUpdate. @return Number completed or typed cutoff
         * failure.
         */
        [[nodiscard]] Result<std::size_t> ApplyQueuedCommands(RuntimeUiCommandCutoff point);
        /** @brief Dispatches at most maximum queued actions through a synchronous borrowed consumer.
         * @param instance Exact instance. @param canvas Authored canvas. @param handler Borrowed host/module-lease-protected handler.
         * @param maximum Positive bound at most 128. @return Dispatched count or canonical error; revoked requests are discarded.
         */
        [[nodiscard]] Result<std::size_t> DispatchActions(RuntimeUiInstanceId instance, UiCanvasId canvas, UiActionHandler &handler,
                                                          std::size_t maximum = 128);
        /** @brief Routes through the canonical dispatcher and copies successful target notifications into bounded client mailboxes.
         * @param canvas Authored canvas. @param route Exact presented tree evidence. @param event Normalized event.
         * @param handler Synchronously borrowed canonical handler. @return Canonical dispatch result or typed capacity/lifetime error.
         */
        [[nodiscard]] Result<UiEventDispatchResult> DispatchEvent(UiCanvasId canvas, const UiEventRoute &route, const UiRoutedEvent &event,
                                                                  UiEventHandler &handler);
        /** @brief Atomically replaces actual owners; old exact-generation clients and queues expire only on successful publication.
         * @param instance Exact publisher. @param replacement Private validated generation. @param point Lifecycle cutoff.
         * @param cancellation Preparation/commit fence. @return Original reconciliation result; failure preserves prior admission.
         * @note An attached animation source requires the coordinated overload and complete trusted derived preparation;
         * this overload returns an explicit policy failure rather than replacing source ahead of its derived owner.
         */
        [[nodiscard]] Result<UiReloadReconciliation> Reload(RuntimeUiInstanceId instance, UiReloadGeneration replacement,
                                                            UiStructuralCommitPoint point, const CancellationToken &cancellation = {});
        /** @brief Atomically reloads the same service publisher and its exclusively attached derived animation owner.
         * @param instance Exact service-admitted instance. @param animation Actual host-owned attached animation owner.
         * @param preparation Complete private generation, source issuer, style resources and trusted replacement policy.
         * @param cancellation Preparation cancellation. @return Source reconciliation and timeline outcomes or original failure.
         * @details Host-only synchronous borrowing; native modules keep existing scoped client grants. All source/derived
         * preparation precedes canonical commit and one attribution invalidation. Failure retains old grants/actions/frame.
         * Foreign service, stale instance/reservation and retired animation owners reject before preparation. */
        [[nodiscard]] Result<UiAnimationReloadResult> Reload(RuntimeUiInstanceId instance, UiAnimationOwner &animation,
                                                             UiAnimationReloadPreparation preparation,
                                                             const CancellationToken &cancellation = {});
        /** @brief Pins presentation data from the sole publisher without granting gameplay mutable owner access.
         * @param instance Exact host-admitted incarnation. @return Immutable generation lease or canonical failure.
         */
        [[nodiscard]] Result<UiReloadLease> Acquire(RuntimeUiInstanceId instance) const;
        /** @brief Prepares the canonical aggregate Scene transition without publishing any owner.
         * @param request Exact outgoing and incoming Scene identity. @param bindings Complete surviving-provider replacements.
         * @param incoming Complete private incoming Scene UI. @param cancellation Host preparation fence.
         * @return Canonical private candidate or original typed error; admission remains unchanged on failure.
         */
        [[nodiscard]] Result<UiSceneReconciliation::Prepared> PrepareTransition(const UiSceneTransitionRequest &request,
                                                                                std::vector<UiSceneBindingReplacement> bindings,
                                                                                std::vector<UiSceneActivation> incoming = {},
                                                                                const CancellationToken &cancellation = {});
        /** @brief Checks the existing aggregate candidate before authoritative host Scene activation.
         * @param prepared Exact canonical candidate. @param point Owner lifecycle cutoff.
         * @return Canonical admission result without publication.
         */
        [[nodiscard]] Result<void> CanCommitTransition(const UiSceneReconciliation::Prepared &prepared,
                                                       UiStructuralCommitPoint point) const;
        /** @brief Publishes the canonical aggregate and expires only replaced generation routes.
         * @param prepared Complete canonical candidate. @param point Owner lifecycle cutoff.
         * @return Canonical publication evidence or original error preserving prior UI and grants.
         */
        [[nodiscard]] Result<UiSceneReconciliationResult> CommitTransition(UiSceneReconciliation::Prepared &prepared,
                                                                           UiStructuralCommitPoint point);
        /** @brief Withdraws an explicitly stopped provider using the canonical binding/layout owners.
         * @param instance Exact host instance. @param canvas Authored canvas. @param provider Exact provider incarnation.
         * @param point Must be BeforeVariableUpdate; the host has already stopped provider admission and producers.
         * @return Canonical publication/required-unavailable evidence or original error; host owns required UI failure policy.
         */
        [[nodiscard]] Result<UiBindingApplyResult> WithdrawProvider(RuntimeUiInstanceId instance, UiCanvasId canvas,
                                                                    UiBindingProviderInstanceId provider, RuntimeUiCommandCutoff point);
        /** @brief Checks canonical producer/render/generation drain after shutdown without blocking.
         * @return Whether the application can reclaim its owner, or typed wrong-thread/missing failure.
         */
        [[nodiscard]] Result<bool> CanReclaim() const;
        /** @brief Collects canonical retired generations outside frame work, including after shutdown. @return Reclaimed count or original
         * error. */
        [[nodiscard]] Result<std::size_t> CollectRetired();
        /** @brief Fences external clients before canonical shutdown, idempotently; owner-thread only. */
        void Shutdown() noexcept;

    private:
        friend class UiAnimationOwner;
        explicit RuntimeUiService(std::shared_ptr<Detail::RuntimeUiServiceState> state) noexcept;
        std::shared_ptr<Detail::RuntimeUiServiceState> state_;
    };
}  // namespace Horo::Runtime::Ui
