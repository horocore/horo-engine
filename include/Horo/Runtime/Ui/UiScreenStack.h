#pragma once

/**
 * @file UiScreenStack.h
 * @brief Owner-scoped transactional Runtime UI route-stack operations.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Ui/UiActions.h"
#include "Horo/Runtime/Ui/UiDocument.h"
#include "Horo/Runtime/Ui/UiIdentity.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime::Ui {
    /** @brief Maximum route definitions or live route instances admitted by one screen stack. */
    inline constexpr std::size_t MaximumUiScreenStackRoutes = MaximumUiDocumentRoutes;

    /** @brief Route visibility state independent from route-instance ownership and lifetime. */
    enum class UiRouteVisibilityState : std::uint8_t {
        Entering,
        Visible,
        Covered,
        Suppressed,
        Suspended,
        Exiting,
        Retiring,
        Count,
    };

    /** @brief Closed transactional route-operation vocabulary. */
    enum class UiRouteOperationKind : std::uint8_t {
        Push,
        Pop,
        ReplaceTop,
        Replace = ReplaceTop,
        Back,
        Clear,
        Reset = Clear,
        Navigate,
        Count,
    };

    /** @brief Terminal state of one route operation transaction. */
    enum class UiRouteOperationOutcome : std::uint8_t {
        Committed,
        Completed = Committed,
        Rejected,
        Count,
    };

    /** @brief Expected refusal reason that leaves the previous stack unchanged. */
    enum class UiRouteOperationRejection : std::uint8_t {
        None,
        Empty,
        NotFound,
        Capacity,
        GuardMismatch,
        Stale = GuardMismatch,
        Cancelled,
        Count,
    };

    /** @brief Explicit lifecycle of one owner-thread route stack. */
    enum class UiScreenStackState : std::uint8_t {
        Active,
        Retiring,
        Stopped,
    };

    /** @brief Exact expected stack state used to guard a navigation request. */
    struct UiRouteStackGuard final {
        UiRouteStackId stack;
        UiRouteStackRevision revision;
        std::optional<UiRouteInstanceId> top;

        /** @brief Creates a complete guard for one observed stack state. */
        [[nodiscard]] static Result<UiRouteStackGuard> Create(UiRouteStackId stack, UiRouteStackRevision revision,
                                                              std::optional<UiRouteInstanceId> top = {});
        /** @brief Reports whether no guard was supplied. @return True when every field is invalid or absent. */
        [[nodiscard]] bool IsEmpty() const noexcept;
        /** @brief Reports whether the guard representation is complete. @return True for a usable guard. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const UiRouteStackGuard &) const noexcept = default;
    };

    /** @brief Owner-local operation identity used to correlate exactly one terminal result. */
    struct UiRouteOperationId final {
        UiOwnershipGeneration ownership;
        UiRouteOperationSequence sequence;

        /** @brief Checks both owner and sequence evidence. @return True for a non-zero operation identity. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const UiRouteOperationId &) const noexcept = default;
    };

    /** @brief Typed request prepared against one route-stack owner and optional observed guard. */
    struct UiRouteOperationRequest final {
        UiRouteOperationKind kind{UiRouteOperationKind::Count};
        std::optional<UiRouteId> route;
        UiRouteStackGuard guard;

        /**
         * @brief Creates a push request for one stable route definition.
         * @param route Stable route definition to activate.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Typed request owning its correlation guard.
         */
        [[nodiscard]] static UiRouteOperationRequest Push(UiRouteId route, const UiRouteStackGuard &guard = {});
        /**
         * @brief Creates a pop request.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Typed request owning its correlation guard.
         */
        [[nodiscard]] static UiRouteOperationRequest Pop(const UiRouteStackGuard &guard = {});
        /**
         * @brief Creates a replace-top request for one stable route definition.
         * @param route Stable route definition to activate.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Typed request owning its correlation guard.
         */
        [[nodiscard]] static UiRouteOperationRequest Replace(UiRouteId route, const UiRouteStackGuard &guard = {});
        /**
         * @brief Creates a replace-top request using the route-action vocabulary.
         * @param route Stable route definition to activate.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Typed request owning its correlation guard.
         */
        [[nodiscard]] static UiRouteOperationRequest ReplaceTop(UiRouteId route, const UiRouteStackGuard &guard = {});
        /**
         * @brief Creates a back request.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Typed request owning its correlation guard.
         */
        [[nodiscard]] static UiRouteOperationRequest Back(const UiRouteStackGuard &guard = {});
        /**
         * @brief Creates a clear request.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Typed request owning its correlation guard.
         */
        [[nodiscard]] static UiRouteOperationRequest Clear(const UiRouteStackGuard &guard = {});
        /**
         * @brief Creates a clear request using the route-action vocabulary.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Typed request owning its correlation guard.
         */
        [[nodiscard]] static UiRouteOperationRequest Reset(const UiRouteStackGuard &guard = {});
        /**
         * @brief Creates an explicit guarded navigation request.
         * @param route Stable route definition to activate.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Typed request owning its correlation guard.
         */
        [[nodiscard]] static UiRouteOperationRequest Navigate(UiRouteId route, const UiRouteStackGuard &guard);

        /** @brief Validates only request shape; owner and route-catalog checks occur at preparation. */
        [[nodiscard]] Result<void> Validate() const;
    };

    /** @brief One committed route activation retained by a screen stack. */
    struct UiRouteInstance final {
        UiRouteInstanceId id;
        UiRouteMetadata metadata;
        UiRouteVisibilityState visibility{UiRouteVisibilityState::Entering};

        [[nodiscard]] constexpr auto operator<=>(const UiRouteInstance &) const noexcept = default;
    };

    /** @brief Exactly one terminal result produced by a prepared route operation. */
    struct UiRouteOperationResult final {
        UiRouteOperationId operation;
        UiRouteOperationKind kind{UiRouteOperationKind::Count};
        UiRouteOperationOutcome outcome{UiRouteOperationOutcome::Count};
        UiRouteOperationRejection rejection{UiRouteOperationRejection::None};
        UiRouteStackRevision revision;
        std::optional<UiRouteInstanceId> route;

        /** @brief Creates a committed result and the resulting stack revision. */
        [[nodiscard]] static Result<UiRouteOperationResult> Committed(UiRouteOperationId operation, UiRouteOperationKind kind,
                                                                      UiRouteStackRevision revision,
                                                                      std::optional<UiRouteInstanceId> route = {});
        /** @brief Creates a rejected terminal result without changing the stack. */
        [[nodiscard]] static Result<UiRouteOperationResult> Rejected(UiRouteOperationId operation, UiRouteOperationKind kind,
                                                                     UiRouteStackRevision revision, UiRouteOperationRejection rejection);

        /** @brief Validates operation correlation and state-specific fields. */
        [[nodiscard]] Result<void> Validate() const;
        /** @brief Reports whether this value is a terminal result. @return True for committed or rejected results. */
        [[nodiscard]] bool IsTerminal() const noexcept;
        /** @brief Reports whether the requested stack mutation was committed. */
        [[nodiscard]] bool IsCommitted() const noexcept;
    };

    /** @brief Complete route-stack owner configuration copied during creation. */
    struct UiScreenStackDescriptor final {
        UiOwnershipGeneration ownership;
        UiRouteStackId stack;
        std::span<const UiRouteMetadata> definitions;
        std::uint32_t maximumRoutes{};
        std::uint32_t previousRouteIncarnation{}; /**< Initial EVER-issued route high-water mark within the never-reused stack slot. */
        std::uint32_t
            maximumRetiredActionRouters{}; /**< Deferred retirement capacity; zero uses maximumRoutes. Drain explicitly at quiescence. */

        /** @brief Validates ownership, route definitions, and finite storage bounds. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Owns one bounded route stack and commits navigation atomically at an owner safe point.
     * @details Route definitions are copied at creation. Every operation first validates its route,
     * guard, capacity, and lifecycle without changing the live stack; commit then performs one bounded
     * mutation and publishes one new stack revision. The stack owns route instances and never owns a
     * renderer, platform, editor, gameplay, callback, or document-tree object.
     */
    class UiScreenStack final {
    private:
        struct Storage;
        class AnimationGate;
        class AnimationTerminalProof;

    public:
        /** @brief Move-only prepared transaction whose commit or cancel produces one terminal result. */
        class Transaction final {
        public:
            ~Transaction();
            Transaction(Transaction &&other) noexcept;
            Transaction &operator=(Transaction &&other) noexcept;
            Transaction(const Transaction &) = delete;
            Transaction &operator=(const Transaction &) = delete;

            /** @brief Commits the prepared mutation exactly once. */
            [[nodiscard]] Result<UiRouteOperationResult> Commit();
            /** @brief Cancels the prepared mutation exactly once without changing the stack. */
            [[nodiscard]] Result<UiRouteOperationResult> Cancel();
            /** @brief Returns the exact operation identity reserved by this transaction. */
            [[nodiscard]] UiRouteOperationId Operation() const noexcept;

        private:
            friend class UiScreenStack;
            friend class UiAnimationOwner;
            friend class AnimationTerminalProof;
            Transaction(std::shared_ptr<Storage> storage, UiRouteOperationRequest request, UiRouteOperationId operation,
                        std::optional<UiRouteMetadata> definition, UiRouteOperationRejection preparedRejection) noexcept;
            void Abandon() noexcept;

            std::shared_ptr<Storage> storage_;
            UiRouteOperationRequest request_;
            UiRouteOperationId operation_;
            std::optional<UiRouteMetadata> definition_;
            UiRouteOperationRejection preparedRejection_{UiRouteOperationRejection::None};
            bool terminal_{};
        };

        /** @brief Creates an active stack and copies its complete bounded route catalog. */
        [[nodiscard]] static Result<UiScreenStack> Create(const UiScreenStackDescriptor &descriptor);
        ~UiScreenStack();
        UiScreenStack(UiScreenStack &&) noexcept;
        UiScreenStack &operator=(UiScreenStack &&) noexcept;
        UiScreenStack(const UiScreenStack &) = delete;
        UiScreenStack &operator=(const UiScreenStack &) = delete;

        /** @brief Prepares one request without mutating the live stack. */
        [[nodiscard]] Result<Transaction> Prepare(UiRouteOperationRequest request);
        /** @brief Prepares and commits one request, returning exactly one terminal result. */
        [[nodiscard]] Result<UiRouteOperationResult> Navigate(UiRouteOperationRequest request);
        /**
         * @brief Performs guarded navigation to one stable route definition.
         * @param route Stable route definition to activate.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Terminal committed/rejected outcome, or typed preparation/lifecycle failure.
         */
        [[nodiscard]] Result<UiRouteOperationResult> Navigate(UiRouteId route, const UiRouteStackGuard &guard);
        /**
         * @brief Pushes one route instance.
         * @param route Stable route definition to activate.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Terminal committed/rejected outcome, or typed preparation/lifecycle failure.
         */
        [[nodiscard]] Result<UiRouteOperationResult> Push(UiRouteId route, const UiRouteStackGuard &guard = {});
        /**
         * @brief Pops the current top route instance.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Terminal committed/rejected outcome, or typed preparation/lifecycle failure.
         */
        [[nodiscard]] Result<UiRouteOperationResult> Pop(const UiRouteStackGuard &guard = {});
        /**
         * @brief Replaces the current top route instance.
         * @param route Stable route definition to activate.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Terminal committed/rejected outcome, or typed preparation/lifecycle failure.
         */
        [[nodiscard]] Result<UiRouteOperationResult> Replace(UiRouteId route, const UiRouteStackGuard &guard = {});
        /**
         * @brief Replaces the current top route instance using the route-action vocabulary.
         * @param route Stable route definition to activate.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Terminal committed/rejected outcome, or typed preparation/lifecycle failure.
         */
        [[nodiscard]] Result<UiRouteOperationResult> ReplaceTop(UiRouteId route, const UiRouteStackGuard &guard = {});
        /**
         * @brief Applies back navigation to the current top route instance.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Terminal committed/rejected outcome, or typed preparation/lifecycle failure.
         */
        [[nodiscard]] Result<UiRouteOperationResult> Back(const UiRouteStackGuard &guard = {});
        /**
         * @brief Clears every route instance in this stack.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Terminal committed/rejected outcome, or typed preparation/lifecycle failure.
         */
        [[nodiscard]] Result<UiRouteOperationResult> Clear(const UiRouteStackGuard &guard = {});
        /**
         * @brief Clears every route instance using the route-action vocabulary.
         * @param guard Borrowed observed stack state, copied into the request; empty disables the guard except for Navigate.
         * @return Terminal committed/rejected outcome, or typed preparation/lifecycle failure.
         */
        [[nodiscard]] Result<UiRouteOperationResult> Reset(const UiRouteStackGuard &guard = {});

        /** @brief Returns the exact stack identity. @return Owner-issued stack handle. */
        [[nodiscard]] UiRouteStackId Stack() const noexcept;
        /** @brief Returns the current committed revision. @return Monotonic stack revision. */
        [[nodiscard]] UiRouteStackRevision Revision() const noexcept;
        /** @brief Returns the incarnation high-water mark including popped/replaced route instances. @return Last issued value. */
        [[nodiscard]] std::uint32_t LastIssuedRouteIncarnation() const noexcept;
        /** @brief Returns the current lifecycle state. */
        [[nodiscard]] UiScreenStackState State() const noexcept;
        /** @brief Checks admission can close outside any held navigation transaction. @return Active and not transaction-busy. */
        [[nodiscard]] bool CanRetire() const noexcept;

        /** @brief Returns the current route count. @return Bounded live instance count. */
        [[nodiscard]] std::size_t Size() const noexcept;
        /** @brief Reports whether no route instances are committed. */
        [[nodiscard]] bool Empty() const noexcept;
        /** @brief Returns the top route without transferring ownership. */
        [[nodiscard]] std::optional<UiRouteInstance> Top() const;
        /** @brief Returns the immutable stack in bottom-to-top order. */
        [[nodiscard]] std::span<const UiRouteInstance> Routes() const noexcept;
        /** @brief Borrows the actual copied route catalog for typed composition validation. @return Immutable definitions. */
        [[nodiscard]] std::span<const UiRouteMetadata> Definitions() const noexcept;

        /** @brief Creates a guard against the current stack revision and top instance. */
        [[nodiscard]] Result<UiRouteStackGuard> Guard() const;

        /**
         * @brief Transfers one active router into the exact live route's lifetime.
         * @param route Exact committed route incarnation.
         * @param router Same-owner router, moved only on success; its source revisions stay immutable.
         * @return Success or typed stale/duplicate/lifecycle/capacity failure. Preparation failure preserves both active owners.
         * @details Attachment reserves replacement action storage before transfer; call it outside frame publication work.
         * @details Pop/back/clear cancel with OwnerRetired, replacement cancels with Superseded, stack shutdown
         * cancels with Shutdown. Failed/cancelled route transactions preserve pending operations.
         */
        [[nodiscard]] Result<void> AttachActions(UiRouteInstanceId route, UiActionRouter &&router);
        /** @brief Borrows a live route's action router for one owner-thread operation. @param route Exact route. @return Router or null;
         * never retain across route mutation. */
        [[nodiscard]] UiActionRouter *Actions(UiRouteInstanceId route) noexcept;

        /** @brief Closes new operation admission while retaining committed routes for drain/inspection. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently stops the stack and releases route instances and definitions. */
        void Shutdown() noexcept;
        /**
         * @brief Reclaims cancelled route-owned action routers outside frame publication.
         * @return Number reclaimed or typed busy/lifecycle failure.
         * @pre Owner-thread load-time/quiescent operation; no prepared route transaction or borrowed action router remains.
         * @details Route removal closes action admission immediately, but retains router storage in preallocated slots.
         *          Call this method before admitting more removals when deferred capacity is exhausted, including after Shutdown.
         */
        [[nodiscard]] Result<std::size_t> DrainRetiredActions();

    private:
        friend class UiAnimationOwner;

        /** @brief Actual stack-issued required-animation reservation; cancellation retains the last-good route stack. */
        class AnimationGate final {
        public:
            AnimationGate(AnimationGate &&) noexcept = default;
            AnimationGate &operator=(AnimationGate &&) noexcept = default;
            AnimationGate(const AnimationGate &) = delete;
            AnimationGate &operator=(const AnimationGate &) = delete;

        private:
            friend class UiScreenStack;
            friend class UiAnimationOwner;
            AnimationGate(Transaction transaction, UiRouteStackRevision expected, UiRouteStackRevision next,
                          std::optional<UiRouteInstanceId> instance, UiInteractionRevision interaction) noexcept;
            Transaction transaction_;
            UiRouteStackRevision expected_;
            UiRouteStackRevision next_;
            std::optional<UiRouteInstanceId> instance_;
            UiInteractionRevision admissionInteraction_;
        };

        /** @brief Private terminal evidence issued only from a successful actual aggregate timeline candidate. */
        class AnimationTerminalProof final {
        public:
            AnimationTerminalProof(const AnimationTerminalProof &) = delete;
            AnimationTerminalProof &operator=(const AnimationTerminalProof &) = delete;

        private:
            friend class UiScreenStack;
            friend class UiAnimationOwner;
            AnimationTerminalProof(const AnimationGate &gate, UiInteractionRevision interaction) noexcept;
            std::shared_ptr<Storage> stack_;
            UiRouteOperationId operation_;
            UiRouteStackRevision revision_;
            UiInteractionRevision interaction_;
        };

        /** @brief Reserves the exact operation and mutation identities before required route motion starts. */
        [[nodiscard]] Result<AnimationGate> PrepareAnimation(UiRouteOperationRequest request, UiInteractionRevision interaction);
        /** @brief Checks actual gate identity, unchanged stack, deferred retirement capacity and private completed-candidate evidence. */
        [[nodiscard]] Result<void> CanPublishAnimation(const AnimationGate &gate, const AnimationTerminalProof &proof) const;
        /** @brief Applies the already checked route mutation once without allocation, callbacks or reclamation. */
        [[nodiscard]] UiRouteOperationResult PublishAnimationValidated(AnimationGate &gate, const AnimationTerminalProof &proof) noexcept;
        /** @brief Applies one pre-issued mutation after all revision, capacity and instance checks have succeeded. */
        /** @brief Qualifies exact live reservation without granting terminal success. */
        [[nodiscard]] Result<void> CanCloseAnimation(const AnimationGate &gate) const;
        /** @brief Publishes prevalidated cancellation or original admission rejection without mutating routes. */
        [[nodiscard]] UiRouteOperationResult CloseAnimationValidated(AnimationGate &gate, UiRouteOperationRejection rejection) noexcept;
        [[nodiscard]] static std::optional<UiRouteInstanceId> ApplyIssuedMutation(Storage &storage, const Transaction &transaction,
                                                                                  std::optional<UiRouteInstanceId> instance) noexcept;
        /** @brief Load-time reservation for each actual route-owned action source. @return Reservation or failure. */
        [[nodiscard]] Result<void> ReserveActionInteractionReplacements();
        /** @brief Prepares new action source generations only after queues, pending operations and retained producers drain. */
        [[nodiscard]] Result<void> PrepareActionInteractionReplacements(const UiActionOwnerContext &owner);
        /** @brief Revalidates actual route-owned replacement sources before no-fail aggregate publication. */
        [[nodiscard]] Result<void> CanPublishActionInteractionReplacements(const UiActionOwnerContext &owner) const;
        /** @brief Swaps prepared actual route action generations without allocation, callbacks or releasing retained pools. */
        void PublishActionInteractionReplacements() noexcept;
        /** @brief Cancels every unpublished actual action source reservation. */
        void AbandonActionInteractionReplacements() noexcept;
        explicit UiScreenStack(std::shared_ptr<Storage> storage) noexcept;
        [[nodiscard]] static Result<std::optional<UiRouteInstanceId>> ApplyMutation(Storage &storage, Transaction &transaction);
        static void Finish(Transaction &transaction) noexcept;
        [[nodiscard]] static Result<UiRouteOperationResult> Commit(Transaction &transaction);
        [[nodiscard]] static Result<UiRouteOperationResult> Cancel(Transaction &transaction);
        static void Abandon(Transaction &transaction) noexcept;

        std::shared_ptr<Storage> storage_;
    };

    /** @brief Route-stack spelling used by presentation code that does not expose the screen-specific alias. */
    using UiRouteStack = UiScreenStack;
}  // namespace Horo::Runtime::Ui
