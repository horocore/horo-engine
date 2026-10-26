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
        /** @brief Exact one-use Scene publication prepared before the generic Scene aggregate claim.
         * @details All operations/destruction use the original UI owner thread. Seal validates and temporarily fences owner
         * reentry; abandoning it preserves old roots. Publish is allowed only after the Scene service's existing commit claim,
         * consumes already-reserved state without failure/allocation, and never commits Scene itself. The service state is pinned,
         * not its movable facade. Shutdown revokes admission and defers canonical teardown until this token is consumed/abandoned.
         */
        class ScenePublication final {
        public:
            ~ScenePublication();
            ScenePublication(ScenePublication &&) noexcept;
            ScenePublication &operator=(ScenePublication &&) noexcept;
            ScenePublication(const ScenePublication &) = delete;
            ScenePublication &operator=(const ScenePublication &) = delete;
            /** @brief Revalidates exact source/reservation/Scene stamps and seals the owner at the aggregate validation cutoff.
             * @param point Actual owner structural cutoff. @return Success or original failure leaving every root unchanged.
             * @pre No UI/Scene work may yield or invoke application callbacks between this seal and Scene publication/rollback.
             */
            [[nodiscard]] Result<void> Seal(UiStructuralCommitPoint point);
            /** @brief Rechecks a sealed token immediately before the owning Scene claim, without changing publication state.
             * @return Success or typed revoked/cancelled/stale evidence; failure requires abandonment before the Scene claim.
             * @pre Owner thread; no callback or yield may intervene after this check and the no-fail Scene publication.
             */
            [[nodiscard]] Result<void> CanPublish() const;
            /** @brief Publishes the already sealed source roots once after the generic Scene commit claim.
             * @pre Seal succeeded; original owner thread, no intervening callbacks or yield; called exactly once.
             */
            void Publish() noexcept;

        private:
            struct Storage;
            friend class RuntimeUiService;
            explicit ScenePublication(std::unique_ptr<Storage> storage) noexcept;
            std::unique_ptr<Storage> storage_;
        };

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
                                                             const CancellationToken &cancellation = {}) const;
        /** @brief Pins presentation data from the sole publisher without granting gameplay mutable owner access.
         * @param instance Exact host-admitted incarnation. @return Immutable generation lease or canonical failure.
         */
        [[nodiscard]] Result<UiReloadLease> Acquire(RuntimeUiInstanceId instance) const;
        /** @brief Pins one exact private Scene candidate for trusted source/native preparation, never normal extraction.
         * @param prepared Same-service unconsumed canonical transition. @param instance Exact incoming/rebound source.
         * @return Immutable generation pin or typed foreign/revoked/lifecycle failure. Abandonment revokes later admission;
         * retaining this pin only preserves bytes. The host must not issue input, receipt or normal frame work from this lease.
         */
        [[nodiscard]] Result<UiReloadLease> AcquirePrepared(const UiSceneReconciliation::Prepared &prepared,
                                                            RuntimeUiInstanceId instance) const;
        /** @brief Immediately revokes only the exact outgoing Scene generation without fallible preparation.
         * @param scene Actual never-reused runtime Scene identity, not a durable document or logical source identity.
         * @pre Serialized original owner thread; abandon that Scene's unpublished activation before forced teardown.
         * @details Idempotent forced-retirement boundary for application Scene adapters. Matching grants/animation sources close
         * immediately. Publisher transfers use reserved retirement slots after any borrowed dispatch, collection or sealed
         * publication has unwound. No allocation, wait, provider callback or publisher destruction occurs. Persistent semantic
         * owners survive; providers bound to this Scene become typed unavailable until explicit canonical rebinding.
         * Foreign-thread or invalid requests do nothing and never mutate another authority.
         */
        void RevokeScene(SceneRuntimeId scene) noexcept;
        /** @brief Prepares the canonical aggregate Scene transition without publishing any owner.
         * @param request Exact outgoing and incoming Scene identity. @param bindings Complete surviving-provider replacements.
         * @param incoming Complete private incoming Scene UI. @param cancellation Host preparation fence.
         * @return Canonical private candidate or original typed error; admission remains unchanged on failure.
         */
        [[nodiscard]] Result<UiSceneReconciliation::Prepared> PrepareTransition(const UiSceneTransitionRequest &request,
                                                                                std::vector<UiSceneBindingReplacement> bindings,
                                                                                std::vector<UiSceneActivation> incoming = {},
                                                                                const CancellationToken &cancellation = {});
        /** @brief Attaches derived animation to an exact unpublished incoming canonical Scene publisher.
         * @param prepared Private candidate issued by this service. @param instance Exact incoming instance.
         * @param allocator Actual source issuer, borrowed synchronously. @param registry Owned style schema.
         * @param styles Canonical style resolver. @param definition Complete authored derived policy.
         * @param limits Finite owner inventories. @return Prepared derived owner or typed failure without publishing any scope.
         * @details The owner pins the same private publisher entry. Abandoning the transition revokes access even when leases
         * retain bytes; successful publication transfers that entry to the active registry without a second publisher or Acquire.
         * This host-only method uses the same HoroRuntimeUi owning target and canonical publisher as coordinated animation Reload.
         */
        [[nodiscard]] Result<UiAnimationOwner> PrepareAnimation(UiSceneReconciliation::Prepared &prepared, RuntimeUiInstanceId instance,
                                                                UiElementSlotAllocator &allocator, RuntimeStyleRegistry registry,
                                                                UiStyleResolver styles, UiAnimationCanvasDefinition definition,
                                                                UiAnimationLimits limits = {});
        /** @brief Prepares the same canonical transition while retaining exact affected derived-source reservations.
         * @param request Exact previous/next Scene or explicit initial activation. @param bindings Persistent provider replacements.
         * @param incoming Complete private incoming publishers. @param coordinatedSources Exact affected active instances owned by this
         * host; all are validated against actual live reservations, not trusted as arbitrary authority. @param cancellation Load fence.
         * @return Private canonical candidate or typed failure preserving active source/derived/client state.
         */
        [[nodiscard]] Result<UiSceneReconciliation::Prepared> PrepareTransition(const UiSceneTransitionRequest &request,
                                                                                std::vector<UiSceneBindingReplacement> bindings,
                                                                                std::vector<UiSceneActivation> incoming,
                                                                                std::span<const RuntimeUiInstanceId> coordinatedSources,
                                                                                const CancellationToken &cancellation = {});
        /** @brief Reserves a one-use no-fail publication after all native/derived preparation, before Scene validation.
         * @param prepared Exact private canonical candidate moved into this token. @return Token or typed owner/capacity failure.
         * @details This allocates only during load-time preparation. Seal does not create storage or acquire source generations.
         */
        [[nodiscard]] Result<ScenePublication> PreparePublication(UiSceneReconciliation::Prepared prepared);
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
        [[nodiscard]] static Result<void> CanCommitPrepared(const std::shared_ptr<Detail::RuntimeUiServiceState> &state,
                                                            const UiSceneReconciliation::Prepared &prepared, UiStructuralCommitPoint point);
        [[nodiscard]] static Result<void> SealPublication(ScenePublication::Storage &storage, UiStructuralCommitPoint point);
        static void PublishScene(ScenePublication::Storage &storage) noexcept;
        static void ReleasePublication(ScenePublication::Storage &storage) noexcept;
        static void DiscardExpiredRoutes(Detail::RuntimeUiServiceState &state) noexcept;
        explicit RuntimeUiService(std::shared_ptr<Detail::RuntimeUiServiceState> state) noexcept;
        std::shared_ptr<Detail::RuntimeUiServiceState> state_;
    };
}  // namespace Horo::Runtime::Ui
