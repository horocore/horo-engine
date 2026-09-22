#pragma once

/**
 * @file UiScreenStack.h
 * @brief Owner-scoped transactional Runtime UI route-stack operations.
 */

#include "Horo/Foundation/Result.h"
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

        /** @brief Creates a push request for one stable route definition. */
        [[nodiscard]] static UiRouteOperationRequest Push(UiRouteId route, UiRouteStackGuard guard = {});
        /** @brief Creates a pop request. */
        [[nodiscard]] static UiRouteOperationRequest Pop(UiRouteStackGuard guard = {});
        /** @brief Creates a replace-top request for one stable route definition. */
        [[nodiscard]] static UiRouteOperationRequest Replace(UiRouteId route, UiRouteStackGuard guard = {});
        /** @brief Creates a replace-top request using the route-action vocabulary. */
        [[nodiscard]] static UiRouteOperationRequest ReplaceTop(UiRouteId route, UiRouteStackGuard guard = {});
        /** @brief Creates a back request. */
        [[nodiscard]] static UiRouteOperationRequest Back(UiRouteStackGuard guard = {});
        /** @brief Creates a clear request. */
        [[nodiscard]] static UiRouteOperationRequest Clear(UiRouteStackGuard guard = {});
        /** @brief Creates a clear request using the route-action vocabulary. */
        [[nodiscard]] static UiRouteOperationRequest Reset(UiRouteStackGuard guard = {});
        /** @brief Creates an explicit guarded navigation request. */
        [[nodiscard]] static UiRouteOperationRequest Navigate(UiRouteId route, UiRouteStackGuard guard);

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
        /** @brief Performs guarded navigation to one stable route definition. */
        [[nodiscard]] Result<UiRouteOperationResult> Navigate(UiRouteId route, UiRouteStackGuard guard);
        /** @brief Pushes one route instance. */
        [[nodiscard]] Result<UiRouteOperationResult> Push(UiRouteId route, UiRouteStackGuard guard = {});
        /** @brief Pops the current top route instance. */
        [[nodiscard]] Result<UiRouteOperationResult> Pop(UiRouteStackGuard guard = {});
        /** @brief Replaces the current top route instance. */
        [[nodiscard]] Result<UiRouteOperationResult> Replace(UiRouteId route, UiRouteStackGuard guard = {});
        /** @brief Replaces the current top route instance using the route-action vocabulary. */
        [[nodiscard]] Result<UiRouteOperationResult> ReplaceTop(UiRouteId route, UiRouteStackGuard guard = {});
        /** @brief Applies back navigation to the current top route instance. */
        [[nodiscard]] Result<UiRouteOperationResult> Back(UiRouteStackGuard guard = {});
        /** @brief Clears every route instance in this stack. */
        [[nodiscard]] Result<UiRouteOperationResult> Clear(UiRouteStackGuard guard = {});
        /** @brief Clears every route instance using the route-action vocabulary. */
        [[nodiscard]] Result<UiRouteOperationResult> Reset(UiRouteStackGuard guard = {});

        /** @brief Returns the exact stack identity. @return Owner-issued stack handle. */
        [[nodiscard]] UiRouteStackId Stack() const noexcept;
        /** @brief Returns the current committed revision. @return Monotonic stack revision. */
        [[nodiscard]] UiRouteStackRevision Revision() const noexcept;
        /** @brief Returns the current lifecycle state. */
        [[nodiscard]] UiScreenStackState State() const noexcept;
        /** @brief Returns the current route count. @return Bounded live instance count. */
        [[nodiscard]] std::size_t Size() const noexcept;
        /** @brief Reports whether no route instances are committed. */
        [[nodiscard]] bool Empty() const noexcept;
        /** @brief Returns the top route without transferring ownership. */
        [[nodiscard]] std::optional<UiRouteInstance> Top() const;
        /** @brief Returns the immutable stack in bottom-to-top order. */
        [[nodiscard]] std::span<const UiRouteInstance> Routes() const noexcept;
        /** @brief Creates a guard against the current stack revision and top instance. */
        [[nodiscard]] Result<UiRouteStackGuard> Guard() const;

        /** @brief Closes new operation admission while retaining committed routes for drain/inspection. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently stops the stack and releases route instances and definitions. */
        void Shutdown() noexcept;

    private:
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
