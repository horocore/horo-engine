#pragma once

/**
 * @file UiBindingStore.h
 * @brief Owner-thread versioned binding publication and exact retained-target invalidation.
 */

#include "Horo/Runtime/Ui/UiBinding.h"
#include "Horo/Runtime/Ui/UiControls.h"
#include "Horo/Runtime/Ui/UiLayout.h"

#include <memory>

namespace Horo::Runtime::Ui {
    struct UiBindingProviderInstanceTag;
    struct UiBindingSnapshotRevisionTag;
    struct UiBindingUpdateRevisionTag;
    /** @brief Host-issued exact provider incarnation; never serialized or reused within its owner generation. */
    using UiBindingProviderInstanceId = UiRuntimeHandle<UiBindingProviderInstanceTag>;
    /** @brief Monotonic committed provider snapshot revision. */
    using UiBindingSnapshotRevision = UiRevision<UiBindingSnapshotRevisionTag>;
    /** @brief Monotonic publication revision of one retained binding store. */
    using UiBindingUpdateRevision = UiRevision<UiBindingUpdateRevisionTag>;

    /** @brief Admitted write cutoff; read cadence never selects a commit trigger. */
    enum class UiBindingCommitTrigger : std::uint8_t {
        Change,
        Blur,
        Submit,
        Count
    };
    /** @brief Deterministic optimistic concurrency policy; conflicts require a fresh edit. */
    enum class UiBindingConflictPolicy : std::uint8_t {
        RejectStale,
        Count
    };

    /** @brief Exact host-issued permission for one schema property and owner incarnation. */
    struct UiBindingWriteFence final {
        UiBindingProviderInstanceId provider;
        UiBindingProviderScopeKind scope{UiBindingProviderScopeKind::Count};
        UiBindingSchemaVersion schema;
        std::uint16_t property{};
        std::uint64_t signature{};
        std::uint64_t capability{}; /**< Non-zero host-issued permission incarnation, never reused. */
        [[nodiscard]] bool operator==(const UiBindingWriteFence &) const noexcept = default;
    };

    /** @brief Immutable bounded command; no provider, module, control or execution pointer crosses the queue. */
    struct UiBindingWriteCommand final {
        UiBindingWriteFence fence;
        UiBindingId binding;
        UiActionSource source;
        UiActionRequestId request;
        UiActionOperationId operation;
        UiBindingSnapshotRevision expected;
        UiBindingCommitTrigger trigger{UiBindingCommitTrigger::Count};
        UiActionValue value;
    };

    /** @brief Provider-owned preparation outcome; Pending keeps execution in the provider, never in the control. */
    enum class UiBindingWriteDisposition : std::uint8_t {
        Ready,
        Pending,
        Rejected,
        Cancelled,
        Count
    };

    /** @brief Exact owner/provider cancellation evidence without allocating a synthetic error. */
    enum class UiBindingWriteCancellationReason : std::uint8_t {
        ProviderUnavailable,
        PresentationChanged,
        OwnerRetired,
        Shutdown,
        ProviderCancelled,
        Count
    };

    /** @brief Correlated outcome retaining original Foundation error evidence without exposing property values. */
    struct UiBindingWriteResult final {
        UiActionRequestId request;
        UiActionOperationId operation;
        UiBindingWriteDisposition disposition{UiBindingWriteDisposition::Count};
        std::optional<Error> error;
        UiBindingWriteCancellationReason cancellation{UiBindingWriteCancellationReason::Count};
    };

    /**
     * @brief Explicit host-owned provider transaction capability leased through writes and module retirement.
     * @details All calls run at the provider owner safe point on the Runtime UI owner thread. The adapter owns authoritative state,
     * jobs and any module callback/image lease. Revoke closes admission and stops producers before host unregister/unload.
     * Prepare must validate the complete fence, expected committed revision, permission and domain rules on every call, including
     * pending polls. It reserves private work without publishing state. Ready promises Commit cannot fail or reenter UI; it commits
     * exactly the command value at expected.Next(). Abandon cancels private work and drains/releases execution leases safely.
     * Preparation is exception-free: the owned adapter converts private library failures to Error before returning its Result.
     * Command borrows end when each call returns; asynchronous work copies its typed inputs and retains its execution leases.
     * All authority methods are non-reentrant; Fence/Active are inert queries. Calls are bounded and nonblocking;
     * jobs never capture controls or call back into UI. A retained shared lease delays adapter/image
     * destruction, never revocation. Hosts must not unmap module code while a lease or producer remains alive.
     */
    class UiBindingWriteAuthority {
    public:
        virtual ~UiBindingWriteAuthority() = default;
        /** @brief Returns immutable exact permission evidence. @return Borrow valid for the authority lifetime. */
        [[nodiscard]] virtual const UiBindingWriteFence &Fence() const noexcept = 0;
        /** @brief Reports admission after permission loss/unload/shutdown. @return Whether permission remains active. */
        [[nodiscard]] virtual bool Active() const noexcept = 0;
        /** @brief Validates/reserves or polls provider-owned work. @param command Exact command. @return Outcome or original error. */
        [[nodiscard]] virtual Result<UiBindingWriteDisposition> Prepare(const UiBindingWriteCommand &command) noexcept = 0;
        /** @brief Publishes a Ready reservation without failure. @param command Same prepared command. */
        virtual void Commit(const UiBindingWriteCommand &command) noexcept = 0;
        /** @brief Idempotently cancels/releases a reservation. @param command Old exact command, including operation correlation. */
        virtual void Abandon(const UiBindingWriteCommand &command) noexcept = 0;
    };

    /** @brief Explicit load-time host grant, independent of inert descriptor validation. */
    struct UiBindingWriteAdmission final {
        UiBindingId binding;
        UiActionOwnerContext owner;
        UiActionId action;
        UiBindingCommitTrigger trigger{UiBindingCommitTrigger::Submit};
        UiBindingConflictPolicy conflict{UiBindingConflictPolicy::RejectStale};
        std::shared_ptr<UiBindingWriteAuthority> authority; /**< Keeps old adapter/module lifetime; grants only this exact property. */
    };

    /** @brief Store-issued edit session; captures committed revision before input changes the draft. */
    struct UiBindingEditId final {
        UiOwnershipGeneration ownership;
        std::uint64_t sequence{};
        UiElementHandle element; /**< Disjoint owner-wide retained slot prevents cross-canvas session aliasing. */
        [[nodiscard]] bool operator==(const UiBindingEditId &) const noexcept = default;
    };

    inline constexpr std::size_t MaximumUiBindingProviders = 64;
    inline constexpr std::size_t MaximumUiBindingChanges = 1'024;
    inline constexpr std::size_t MaximumUiBindingStorageBytes = 4U << 20U;
    inline constexpr std::size_t MaximumUiBindingChangeBytes = 1U << 20U;

    /** @brief One owned typed property value borrowed only during synchronous publication. */
    struct UiBindingPropertyUpdate final {
        std::uint16_t property{}; /**< Zero-based slot in the exact identity-sorted provider schema. */
        UiBindingValue value;
    };

    /** @brief Exact owner-admitted provider with copied metadata and an initial coherent snapshot. */
    struct UiBindingProviderRegistration final {
        UiBindingProviderInstanceId instance;
        UiBindingProviderScopeKind scope{UiBindingProviderScopeKind::GameInstance};
        const UiBindingProviderSchema *schema{}; /**< Borrowed only by Create; no contributor pointer survives. */
        UiBindingSnapshotRevision revision;
        std::span<const UiBindingPropertyUpdate> values; /**< Strictly increasing slots; absent optional targets use fallback. */
    };

    /** @brief Host-resolved binding; no nearest-provider lookup or scope inference occurs. */
    struct UiResolvedBindingDescriptor final {
        UiBindingProviderInstanceId provider;
        UiBindingDescriptor binding;
        std::optional<UiBindingValue> initialTarget; /**< Authored UI value required only for TargetToSource; never a provider read. */
    };

    /** @brief Delta from one coherent provider revision to a newer revision. */
    struct UiBindingChangeBatch final {
        UiBindingProviderInstanceId provider;
        UiBindingSchemaVersion schema;
        UiBindingSnapshotRevision expected;
        UiBindingSnapshotRevision revision;
        std::span<const UiBindingPropertyUpdate> changes; /**< Strictly increasing slots; borrowed until Apply returns. */
    };

    /** @brief Independent downstream work categories for one exact semantic target. */
    enum class UiBindingDirty : std::uint8_t {
        None = 0,
        Layout = 1U << 0U,
        Paint = 1U << 1U,
        Accessibility = 1U << 2U,
        Actions = 1U << 3U,
    };

    /**
     * @brief Combines independent downstream work.
     * @param left First set of dirty categories.
     * @param right Additional dirty categories.
     * @return Union of both category sets.
     */
    [[nodiscard]] constexpr UiBindingDirty operator|(const UiBindingDirty left, const UiBindingDirty right) noexcept {
        return static_cast<UiBindingDirty>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
    }

    /**
     * @brief Tests downstream work without changing target state.
     * @param flags Dirty categories to inspect.
     * @param flag Category mask to test.
     * @return True when any category in flag is present in flags.
     */
    [[nodiscard]] constexpr bool HasFlag(const UiBindingDirty flags, const UiBindingDirty flag) noexcept {
        return (static_cast<unsigned>(flags) & static_cast<unsigned>(flag)) != 0;
    }

    /** @brief Explicit source availability; an unavailable required binding has no readable stale value. */
    enum class UiBindingValueOrigin : std::uint8_t {
        Provider,
        Fallback,
        Unavailable,
        UiLocal /**< Authored initial value of a write-only target before its first accepted provider commit. */
    };

    /** @brief One retained typed target, borrowed only until the next store mutation or destruction. */
    struct UiBoundTarget final {
        UiBindingId binding;
        UiElementHandle element;
        UiBindingTargetProperty property{};
        UiBindingValue value;
        UiBindingValueOrigin origin{UiBindingValueOrigin::Unavailable};
    };

    /** @brief Exact downstream notification; copied before layout/render/action consumers run. */
    struct UiBindingTargetDirty final {
        UiBindingId binding;
        UiElementHandle element;
        UiBindingTargetProperty property{};
        UiBindingDirty dirty{UiBindingDirty::None};
        UiBindingUpdateRevision revision;
        UiRuntimeTreeRevision tree;
    };

    /** @brief Explicit load-time and per-update capacity limits. */
    struct UiBindingStoreLimits final {
        std::size_t providers{MaximumUiBindingProviders};
        std::size_t bindings{MaximumUiBindingDescriptors};
        std::size_t changes{MaximumUiBindingChanges};
        std::size_t valueBytes{MaximumUiBindingStorageBytes}; /**< Aggregate target and fallback text reservation budget. */
        std::size_t changeBytes{MaximumUiBindingChangeBytes}; /**< Aggregate variable-sized input bytes per Apply. */
    };

    /** @brief Result of atomic binding publication, including required source loss on unregister. */
    struct UiBindingApplyResult final {
        UiBindingUpdateRevision revision;
        UiLayoutContentRevision content;
        std::size_t targetsChanged{};
        std::size_t requiredUnavailable{};
    };

    /**
     * @brief Sole mutable binding owner for one exact retained canvas generation.
     * @details Create is load-time and copies schema, descriptors and initial values. Apply and Unregister run only on the Runtime UI
     * owner thread at the VariableUpdate snapshot cutoff, before layout/input/extraction consumers. They validate the entire transaction,
     * then atomically enqueue precise layout invalidations and copy target values into preallocated storage. No provider callback, I/O,
     * polling, lock or allocation occurs on successful Apply calls. Unregister abandons explicitly admitted write reservations at the
     * shared owner safe point; the authority's cancellation must remain bounded and nonblocking. Empty updates do no target work.
     * Target borrows cannot escape a synchronous owner phase; immutable text/layout/render snapshots own their derived copies.
     * Reload/structural replacement prepares a new store against the replacement tree; stale batches never reconcile by slot alone.
     * SourceToTarget, TwoWay and TargetToSource are supported; converters require separate conversion capability and fail explicitly.
     * Writes require explicit AdmitWrites. TargetToSource requires a typed authored initialTarget and does not subscribe to provider
     * deltas.
     */
    class UiBindingStore final {
    public:
        /**
         * @brief Prepares complete target state against one active retained tree.
         * @param tree Exact active canvas; no tree pointer is retained.
         * @param providers Exact host-resolved provider registrations; all spans are copied or consumed before return.
         * @param bindings Conflict-free resolved descriptors; TargetToSource requires initialTarget, readable directions forbid it.
         * @param limits Hard-bounded lifetime capacities.
         * @return Complete private candidate or typed identity, schema, availability, value or capacity failure.
         */
        [[nodiscard]] static Result<UiBindingStore> Create(const UiElementTree &tree,
                                                           std::span<const UiBindingProviderRegistration> providers,
                                                           std::span<const UiResolvedBindingDescriptor> bindings,
                                                           const UiBindingStoreLimits &limits = {});
        /** @brief Abandons pending reservations before releasing Horo-owned storage and admitted authority leases. */
        ~UiBindingStore();
        /** @brief Transfers unique binding ownership and invalidates other. @param other Store to transfer. */
        UiBindingStore(UiBindingStore &&other) noexcept;
        /** @brief Replaces this owner with other. @param other Store to transfer. @return This store. */
        UiBindingStore &operator=(UiBindingStore &&other) noexcept;
        UiBindingStore(const UiBindingStore &) = delete;
        UiBindingStore &operator=(const UiBindingStore &) = delete;

        /**
         * @brief Atomically consumes bounded deltas and queues exact measure invalidations in the existing layout owner.
         * @param tree Exact tree captured at creation; replacement revisions reject old batches.
         * @param batches At most one batch per provider, strictly increasing provider identity, with increasing property slots.
         * @param layout Exact canvas layout owner. Capacity/lifecycle failure leaves values, revisions and queued invalidations unchanged.
         * @return Publication summary or typed stale/schema/conflict/value/capacity/lifecycle failure.
         * @post Equal values advance provider evidence but do not dirty or rebuild targets. Content advances only for layout work.
         */
        [[nodiscard]] Result<UiBindingApplyResult> Apply(const UiElementTree &tree, std::span<const UiBindingChangeBatch> batches,
                                                         UiLayoutEngine &layout);

        /**
         * @brief Atomically revokes a provider, removes readable required values and publishes optional fallbacks.
         * @param tree Exact live tree.
         * @param provider Exact provider incarnation to close permanently in this store.
         * @param layout Exact layout owner receiving affected-target invalidations.
         * @return Publication summary; repeated unregister succeeds without work. Failure leaves the registration intact for retry.
         * @pre The host stops provider admission/producers before unregister and retires required UI on requiredUnavailable.
         */
        [[nodiscard]] Result<UiBindingApplyResult> Unregister(const UiElementTree &tree, UiBindingProviderInstanceId provider,
                                                              UiLayoutEngine &layout);

        /**
         * @brief Reads retained target state without calling the provider.
         * @param tree Exact active tree; structural replacement, reload and retirement invalidate borrows.
         * @param binding Stable binding identity.
         * @return Synchronous borrow, or nullptr for absent/required-unavailable targets or after retirement.
         */
        [[nodiscard]] const UiBoundTarget *Find(const UiElementTree &tree, UiBindingId binding) const noexcept;
        /**
         * @brief Copies and acknowledges all accumulated downstream dirties in descriptor order, without allocation.
         * @param output Caller-owned storage; insufficient capacity leaves output and pending work unchanged.
         * @return Copied count or capacity/lifecycle failure. Layout work was already queued by publication.
         */
        [[nodiscard]] Result<std::size_t> DrainDirty(std::span<UiBindingTargetDirty> output);
        /** @brief Returns current publication/content evidence without polling providers. @return Latest committed summary. */
        [[nodiscard]] UiBindingApplyResult Current() const noexcept;
        /** @brief Validates the retained store's actual tree/source lineage without invoking a provider.
         * @param tree Exact active owning tree. @return Success or typed stale/lifecycle failure.
         */
        [[nodiscard]] Result<void> ValidateOwner(const UiElementTree &tree) const;
        /** @brief Qualifies a stable element's actual value-binding and admitted write contract for draft preservation.
         * @param source Old active store. @param sourceTree Its exact tree. @param tree Replacement tree.
         * @param element Stable authored control identity. @return True only when actual provider/schema/property,
         * committed revision, direction and write permission contracts match; no authority callback runs.
         */
        [[nodiscard]] bool ReloadCompatible(const UiBindingStore &source, const UiElementTree &sourceTree, const UiElementTree &tree,
                                            UiElementId element) const;

        /**
         * @brief Atomically admits a complete bounded write capability batch at load time, once per store.
         * @param tree Exact retained tree. @param admissions Exact bindings, presented owners and leased capabilities.
         * @return Success or schema/access/identity/conflict failure; only immutable authority metadata is inspected.
         */
        [[nodiscard]] Result<void> AdmitWrites(const UiElementTree &tree, std::span<const UiBindingWriteAdmission> admissions);
        /**
         * @brief Adopts a newer successfully presented interaction without rebuilding read bindings.
         * @param tree Exact live tree. @param owner New last-presented owner with otherwise identical evidence.
         * @return Success or stale/lifecycle failure. Old edits/reservations cancel once before the new presentation admits input.
         */
        [[nodiscard]] Result<void> UpdateWritePresentation(const UiElementTree &tree, const UiActionOwnerContext &owner);
        /**
         * @brief Captures expected committed revision and exact presented source before editing.
         * @param tree Exact tree. @param binding Admitted writable binding. @param source Last-presented source.
         * @return Store-issued session or access/stale/busy failure. One edit/write may be outstanding per binding.
         */
        [[nodiscard]] Result<UiBindingEditId> BeginEdit(const UiElementTree &tree, UiBindingId binding, const UiActionSource &source);
        /** @brief Ends an unqueued draft after cancel/rejection. @param edit Exact session. @return Success or stale/lifecycle failure. */
        [[nodiscard]] Result<void> CancelEdit(const UiBindingEditId &edit);
        /**
         * @brief Queues one action-routed draft at its admitted trigger without publishing target/provider/layout state.
         * @param tree Exact tree. @param edit Captured edit session. @param request Real action-router request.
         * @param trigger Owner-observed change/blur/submit. @return Pending correlated evidence or original validation failure.
         * @details The final argument is the control value; only bool, finite double and bounded UTF-8 text are admitted.
         */
        [[nodiscard]] Result<UiBindingWriteResult> QueueWrite(const UiElementTree &tree, const UiBindingEditId &edit,
                                                              const UiActionRequest &request, UiBindingCommitTrigger trigger);
        /**
         * @brief Previews an unsuppressed control default, admits its routed write, then applies its UI-local pending projection.
         * @param tree Exact tree. @param edit Captured session. @param control Exact active control.
         * @param request Request admitted by the action router from PeekDefault's action/payload/source.
         * @return Pending write or failure; failed admission suppresses the default and preserves committed control state.
         */
        [[nodiscard]] Result<UiBindingWriteResult> QueueControlDefault(const UiElementTree &tree, const UiBindingEditId &edit,
                                                                       UiControlStateMachine &control, const UiActionRequest &request);
        /**
         * @brief Processes at most one queued/pending command at the provider owner safe point in deterministic round-robin binding order.
         * @param tree Exact tree. @param layout Existing layout owner. @return Empty when idle or correlated provider outcome/error.
         * @post Ready publication and provider commit are atomic; any failure leaves values, revisions and layout dirties unchanged.
         * @details Terminal slots remain occupied until DrainWriteResults, preventing result loss and duplicate completion.
         */
        [[nodiscard]] Result<std::optional<UiBindingWriteResult>> ProcessWrite(const UiElementTree &tree, UiLayoutEngine &layout);
        /** @brief Copies/acknowledges terminal outcomes. @param output Bounded caller buffer. @return Count or capacity failure. */
        [[nodiscard]] Result<std::size_t> DrainWriteResults(std::span<UiBindingWriteResult> output);
        /**
         * @brief Restores a control from the committed target after terminal rejection/cancellation or accepted publication.
         * @param tree Exact tree. @param binding Binding to reconcile. @param control Exact presented control.
         * @return Success or typed stale/type/lifecycle failure; no provider callback occurs.
         */
        [[nodiscard]] Result<void> ReconcileControl(const UiElementTree &tree, UiBindingId binding, UiControlStateMachine &control) const;
        /** @brief Closes admission and target borrows; existing immutable downstream snapshots remain owned by their consumers. */
        void BeginRetirement() noexcept;
        /** @brief Closes reload admission and records cancellations without invoking an external write authority.
         * @return Success or reentrant/lifecycle failure; admitted authority/module pins stay retained until DrainReloadRetirement.
         * @pre Owner safe point, outside ProcessWrite/provider callbacks.
         */
        [[nodiscard]] Result<void> CloseReloadAdmission();
        /** @brief Abandons deferred reload reservations exactly once outside frame-hot publication.
         * @return Success or reentrant failure. The store retains all authority/module leases until this drain completes.
         */
        [[nodiscard]] Result<void> DrainReloadRetirement();

        /** @brief Idempotently abandons pending provider reservations before releasing owned storage and authority leases. */
        void Shutdown() noexcept;

    private:
        struct Storage;
        friend class UiAnimationOwner;
        /** @brief Checks exact newer presentation and fully drained writes without calling authority callbacks.
         * @return Whether a source change is required, or typed busy/stale failure.
         */
        [[nodiscard]] Result<bool> CanAdoptAnimationPresentation(const UiElementTree &tree, const UiActionOwnerContext &owner) const;
        /** @brief Cancels old edit evidence before adopting a prevalidated successfully presented source, with no foreign callback. */
        void AdoptAnimationPresentationValidated(const UiActionOwnerContext &owner) noexcept;
        /** @brief Adopts a fully prepared private binding candidate. @param storage Unique candidate ownership. */
        explicit UiBindingStore(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };

    /**
     * @brief Production action-router adapter over one captured binding edit.
     * @details Borrows last only through synchronous Dispatch. The router queues values, never this handler or its borrows.
     * The host drains terminal write feedback and reconciles controls separately; UI does not own provider execution.
     */
    class UiBindingWriteActionHandler final : public UiActionHandler {
    public:
        /**
         * @brief Routes a form/change/blur draft through the admitted trigger.
         * @param store Binding owner. @param tree Exact tree. @param edit Captured session. @param trigger Observed trigger.
         */
        UiBindingWriteActionHandler(UiBindingStore &store, const UiElementTree &tree, const UiBindingEditId &edit,
                                    UiBindingCommitTrigger trigger) noexcept;
        /**
         * @brief Routes the real staged control default before applying its pending UI-local value.
         * @param store Binding owner. @param tree Exact tree. @param edit Captured session. @param control Borrowed exact control.
         */
        UiBindingWriteActionHandler(UiBindingStore &store, const UiElementTree &tree, const UiBindingEditId &edit,
                                    UiControlStateMachine &control) noexcept;
        /** @brief Admits one routed write. @param request Frozen router request. @return Pending correlation or original failure. */
        [[nodiscard]] Result<UiActionResult> Handle(const UiActionRequest &request) override;

    private:
        UiBindingStore &store_;
        const UiElementTree &tree_;
        UiBindingEditId edit_;
        UiBindingCommitTrigger trigger_;
        UiControlStateMachine *control_{};
    };
}  // namespace Horo::Runtime::Ui
