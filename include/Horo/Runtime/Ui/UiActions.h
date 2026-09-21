#pragma once

/**
 * @file UiActions.h
 * @brief Typed Runtime UI action commands, bounded payloads, results, and navigation evidence.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Ui/UiIdentity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace Horo::Runtime::Ui {
    inline constexpr std::size_t MaximumUiActionTextBytes = 256;
    inline constexpr std::size_t MaximumUiActionArguments = 8;
    inline constexpr std::uint32_t MaximumUiActionCommands = 1'024;

    /** @brief Closed typed origin for an action crossing the Runtime UI command boundary. */
    enum class UiActionOrigin : std::uint8_t {
        Button,
        Form,
        Route,
        Gameplay,
        Navigation,
        Count,
    };

    /** @brief Closed command family carried by one Runtime UI action request. */
    enum class UiActionCommandKind : std::uint8_t {
        Button,
        Form,
        Route,
        Gameplay,
        Navigation,
        Count,
    };

    /** @brief Closed result state for one admitted Runtime UI action request. */
    enum class UiActionResultKind : std::uint8_t {
        Handled,
        Rejected,
        Pending,
        Completed,
        Cancelled,
        Count,
    };

    /** @brief Expected, non-error rejection reason for an action command. */
    enum class UiActionRejectionReason : std::uint8_t {
        Unavailable,
        NotAuthorized,
        Busy,
        Stale,
        Unsupported,
        NoTarget,
        Capacity,
        Retiring,
        Count,
    };

    /** @brief Typed cancellation reason for a pending action operation. */
    enum class UiActionCancellationReason : std::uint8_t {
        Requested,
        OwnerRetired,
        Reload,
        Shutdown,
        Superseded,
        Count,
    };

    /** @brief Typed default-navigation request direction. */
    enum class UiNavigationDirection : std::uint8_t {
        Next,
        Previous,
        Up,
        Down,
        Left,
        Right,
        Submit,
        Cancel,
        Count,
    };

    /** @brief Typed result of resolving a default navigation request. */
    enum class UiDefaultNavigationOutcome : std::uint8_t {
        FocusMoved,
        SubmitDispatched,
        CancelDispatched,
        NoTarget,
        Count,
    };

    /** @brief Bounded UTF-8 action text copied into an owned payload. */
    struct UiActionText final {
        std::array<char, MaximumUiActionTextBytes> bytes{}; /**< Copied UTF-8 bytes; unused tail is zero-initialized. */
        std::uint16_t size{};                               /**< Number of copied bytes in `bytes`. */

        /**
         * @brief Copies one bounded UTF-8 value.
         * @param value Source bytes; the caller retains ownership.
         * @return Owned text or UiErrors::ActionPayloadInvalid when the value is invalid or too large.
         */
        [[nodiscard]] static Result<UiActionText> Create(std::string_view value);

        /** @brief Returns the copied bytes. @return Borrowed UTF-8 view valid for this value's lifetime. */
        [[nodiscard]] std::string_view View() const noexcept;

        /** @brief Checks the representation bound. @return Whether the stored byte count is admissible. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Closed allocation-free scalar and Runtime UI identity value vocabulary for action arguments. */
    using UiActionValue =
        std::variant<bool, std::int64_t, std::uint64_t, double, UiActionText, UiActionId, UiDocumentId, UiCanvasId, UiElementId>;

    /**
     * @brief Fixed-capacity owned typed action arguments.
     * @details The payload never owns a string map, callback, provider, script object, renderer handle, or gameplay pointer.
     */
    class UiActionPayload final {
    public:
        UiActionPayload() = default;

        /**
         * @brief Copies a complete bounded argument list.
         * @param values Caller-owned typed values in canonical order.
         * @return Owned payload or a typed validation/capacity failure.
         */
        [[nodiscard]] static Result<UiActionPayload> Create(std::span<const UiActionValue> values);

        /**
         * @brief Appends one typed value without allocating.
         * @param value Owned value copied into the next slot.
         * @return Success or UiErrors::ActionPayloadInvalid/ActionPayloadCapacityExceeded.
         */
        [[nodiscard]] Result<void> Add(UiActionValue value);

        /** @brief Returns copied values in insertion order. @return Borrowed bounded value span. */
        [[nodiscard]] std::span<const UiActionValue> Values() const noexcept;
        /** @brief Returns the number of copied values. @return Count in [0, MaximumUiActionArguments]. */
        [[nodiscard]] std::size_t Size() const noexcept;
        /** @brief Validates every typed value and the fixed bound. @return Success or a typed payload failure. */
        [[nodiscard]] Result<void> Validate() const;

    private:
        std::array<UiActionValue, MaximumUiActionArguments> values_{};
        std::uint8_t count_{};
    };

    /** @brief Exact immutable owner and revision evidence captured when an action is targeted. */
    struct UiActionOwnerContext final {
        RuntimeUiInstanceId instance;        /**< Exact mutable Runtime UI instance. */
        UiCanvasInstanceId canvas;           /**< Exact canvas incarnation receiving the action. */
        UiDocumentId document;               /**< Stable source document identity. */
        UiDocumentRevision documentRevision; /**< Exact authored revision used by the active tree. */
        UiRuntimeTreeRevision treeRevision;  /**< Exact retained-tree revision used for targeting. */
        UiInteractionRevision interaction;   /**< Exact last-presented interaction revision. */

        /** @brief Validates owner identities and revisions without consulting ambient state. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Compares all owner and revision evidence. @return Structural equality. */
        [[nodiscard]] bool operator==(const UiActionOwnerContext &) const noexcept = default;
    };

    /** @brief Exact target element and owner evidence for one UI-originated action. */
    struct UiActionSource final {
        UiActionOwnerContext owner; /**< Exact instance/canvas/document/revision evidence. */
        UiElementHandle element;    /**< Exact target or focused element. */

        /** @brief Validates the source and same-owner element handle. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Compares all source evidence. @return Structural equality. */
        [[nodiscard]] bool operator==(const UiActionSource &) const noexcept = default;
    };

    /** @brief Owner-local request identity; never serialized or reused across owner generations. */
    struct UiActionRequestId final {
        UiOwnershipGeneration ownership; /**< Exact Runtime UI owner incarnation. */
        UiActionSequence sequence;       /**< Non-zero monotonic owner-local sequence. */

        /** @brief Checks both owner and sequence representation. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const UiActionRequestId &) const noexcept = default;
    };

    /** @brief Owner-local identity for correlating a pending operation with one terminal result. */
    struct UiActionOperationId final {
        UiOwnershipGeneration ownership;    /**< Exact Runtime UI owner incarnation. */
        UiActionOperationSequence sequence; /**< Non-zero operation sequence supplied by the action owner. */

        /** @brief Checks both owner and operation sequence representation. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const UiActionOperationId &) const noexcept = default;
    };

    /** @brief Typed button command with stable authored action identity and bounded arguments. */
    struct UiButtonActionCommand final {
        UiActionId action;       /**< Stable authored action identity. */
        UiActionPayload payload; /**< Bounded typed command arguments. */
    };

    /** @brief Closed form interaction operation represented by one stable form action. */
    enum class UiFormActionKind : std::uint8_t {
        Submit,
        Cancel,
        Validate,
        Reset,
        Count,
    };

    /** @brief Typed form command with stable authored action identity and explicit operation. */
    struct UiFormActionCommand final {
        UiActionId action;                               /**< Stable authored action identity. */
        UiFormActionKind kind{UiFormActionKind::Submit}; /**< Explicit form operation. */
        UiActionPayload payload;                         /**< Bounded typed command arguments. */
    };

    /** @brief Closed route mutation vocabulary admitted at an owner safe point. */
    enum class UiRouteActionKind : std::uint8_t {
        Push,
        Pop,
        ReplaceTop,
        Reset,
        ShowOverlay,
        Dismiss,
        Count,
    };

    /** @brief Typed route command; route ownership remains outside Runtime UI action values. */
    struct UiRouteActionCommand final {
        UiActionId action;                               /**< Stable authored action identity. */
        UiRouteActionKind kind{UiRouteActionKind::Push}; /**< Explicit route operation. */
        UiActionPayload payload;                         /**< Bounded typed route arguments. */
    };

    /** @brief Typed gameplay-facing command; it carries no gameplay object or callback ownership. */
    struct UiGameplayActionCommand final {
        UiActionId action;       /**< Stable authored action identity. */
        UiActionPayload payload; /**< Bounded typed command arguments. */
    };

    /** @brief Typed default navigation command resolved against the presented focus graph by its owner. */
    struct UiNavigationCommand final {
        UiNavigationDirection direction{UiNavigationDirection::Next};
        UiElementHandle focused; /**< Exact focused element in the presented interaction generation. */
    };

    /** @brief Closed command/payload boundary crossing from Runtime UI to an owning consumer. */
    using UiActionCommand =
        std::variant<UiButtonActionCommand, UiFormActionCommand, UiRouteActionCommand, UiGameplayActionCommand, UiNavigationCommand>;

    /**
     * @brief Returns the closed command family without inspecting string or backend state.
     * @param command Typed command to inspect.
     * @return Exact command family.
     */
    [[nodiscard]] UiActionCommandKind UiActionCommandKindOf(const UiActionCommand &command) noexcept;

    /**
     * @brief Returns the semantic origin corresponding to a typed command family.
     * @param command Typed command to inspect.
     * @return Closed origin value used by provider/application command authority.
     */
    [[nodiscard]] UiActionOrigin UiActionOriginOf(const UiActionCommand &command) noexcept;

    /**
     * @brief Validates one typed command before it enters an owner queue.
     * @param command Command and bounded payload to validate.
     * @return Success or a stable action/payload/navigation error.
     */
    [[nodiscard]] Result<void> ValidateUiActionCommand(const UiActionCommand &command);

    /** @brief One frozen request delivered to a borrowed action consumer. */
    struct UiActionRequest final {
        UiActionRequestId id;                         /**< Owner-local request identity. */
        UiActionSource source;                        /**< Exact source and revision evidence. */
        UiActionOrigin origin{UiActionOrigin::Count}; /**< Semantic command origin derived at admission. */
        UiActionCommand command;                      /**< Typed command and owned bounded payload. */

        /** @brief Validates request identity, source evidence, and command payload. */
        [[nodiscard]] Result<void> Validate() const;
    };

    /** @brief Typed outcome of default focus/navigation resolution. */
    struct UiDefaultNavigationResult final {
        UiDefaultNavigationOutcome outcome{UiDefaultNavigationOutcome::Count};
        std::optional<UiElementHandle> from;
        std::optional<UiElementHandle> target;

        /** @brief Creates a focus-moved result with exact old and new targets. */
        [[nodiscard]] static Result<UiDefaultNavigationResult> FocusMoved(UiElementHandle from, UiElementHandle target);
        /** @brief Creates a submit-dispatched result for the focused target. */
        [[nodiscard]] static Result<UiDefaultNavigationResult> SubmitDispatched(UiElementHandle target);
        /** @brief Creates a cancel-dispatched result for the focused target. */
        [[nodiscard]] static Result<UiDefaultNavigationResult> CancelDispatched(UiElementHandle target);
        /** @brief Creates a no-target result that preserves the focused source. */
        [[nodiscard]] static Result<UiDefaultNavigationResult> NoTarget(UiElementHandle from);

        /** @brief Validates the outcome and its exact optional handle shape. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Immutable action outcome with explicit admission, pending, and terminal semantics.
     * @details `Handled` records routing consumption, `Rejected` records an expected non-error refusal,
     * `Pending` carries an owner-local operation identity, and `Completed`/`Cancelled` are terminal.
     */
    struct UiActionResult final {
        UiActionResultKind kind{UiActionResultKind::Count};
        UiActionRequestId request;
        UiActionOperationId operation;
        bool hasOperation{};
        UiActionRejectionReason rejection{UiActionRejectionReason::Count};
        UiActionCancellationReason cancellation{UiActionCancellationReason::Count};
        UiActionPayload payload;
        std::optional<UiDefaultNavigationResult> navigation;

        /** @brief Creates a routing-handled result. */
        [[nodiscard]] static Result<UiActionResult> Handled(UiActionRequestId request);
        /** @brief Creates an expected rejected result. */
        [[nodiscard]] static Result<UiActionResult> Rejected(UiActionRequestId request, UiActionRejectionReason reason);
        /** @brief Creates a pending result with an owner-matching operation identity. */
        [[nodiscard]] static Result<UiActionResult> Pending(UiActionRequestId request, UiActionOperationId operation);
        /** @brief Creates a completed result with optional terminal payload and operation correlation. */
        [[nodiscard]] static Result<UiActionResult> Completed(UiActionRequestId request, UiActionPayload payload = {},
                                                              std::optional<UiActionOperationId> operation = {});
        /** @brief Creates a completed default-navigation result without a separate operation. */
        [[nodiscard]] static Result<UiActionResult> CompletedNavigation(UiActionRequestId request, UiDefaultNavigationResult navigation);
        /** @brief Creates a cancelled terminal result for a pending operation. */
        [[nodiscard]] static Result<UiActionResult> Cancelled(UiActionRequestId request, UiActionOperationId operation,
                                                              UiActionCancellationReason reason);

        /** @brief Validates state-specific fields and owner correlation. */
        [[nodiscard]] Result<void> Validate() const;
        /** @brief Reports whether this result is terminal. @return True for Rejected, Completed, or Cancelled. */
        [[nodiscard]] bool IsTerminal() const noexcept;
    };

    /** @brief Borrowed synchronous boundary for a typed action consumer. */
    class UiActionHandler {
    public:
        virtual ~UiActionHandler() = default;

        /**
         * @brief Consumes one frozen action request.
         * @param request Exact owner/revision-fenced request and typed command.
         * @return A state-valid result for the same request, or a typed handler failure.
         * @note The handler retains neither the request nor any Runtime UI storage.
         */
        [[nodiscard]] virtual Result<UiActionResult> Handle(const UiActionRequest &request) = 0;
    };

    /** @brief Explicit lifecycle of one preallocated owner-thread action router. */
    enum class UiActionRouterState : std::uint8_t {
        Active,
        Retiring,
        Stopped,
    };

    /** @brief Exact owner evidence and finite queue capacity for one router generation. */
    struct UiActionRouterDescriptor final {
        UiActionOwnerContext owner;
        std::uint32_t maximumQueuedCommands{};

        /** @brief Validates owner identities and the bounded command queue size. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Preallocated Runtime UI action queue and synchronous dispatch boundary.
     * @details Queue admission and dispatch perform no allocation. Reload creates a new owner context and router;
     *          retirement closes admission, and shutdown clears queued requests without invoking a consumer.
     */
    class UiActionRouter final {
    public:
        /**
         * @brief Creates a router and reserves its complete command queue.
         * @param descriptor Exact owner/revision evidence and finite queue bound.
         * @return Active router or typed validation/allocation failure.
         */
        [[nodiscard]] static Result<UiActionRouter> Create(const UiActionRouterDescriptor &descriptor);
        ~UiActionRouter();
        UiActionRouter(UiActionRouter &&) noexcept;
        UiActionRouter &operator=(UiActionRouter &&) noexcept;
        UiActionRouter(const UiActionRouter &) = delete;
        UiActionRouter &operator=(const UiActionRouter &) = delete;

        /**
         * @brief Admits one typed action from an exact source without allocating.
         * @param source Target/focus evidence from the last presented interaction generation.
         * @param command Typed command and bounded payload.
         * @return Owner-local request identity or typed stale/capacity/lifecycle failure.
         */
        [[nodiscard]] Result<UiActionRequestId> Enqueue(UiActionSource source, UiActionCommand command);

        /**
         * @brief Removes at most one queued request in FIFO order without allocating.
         * @return Empty optional when the queue is empty, or a lifecycle failure after shutdown.
         */
        [[nodiscard]] Result<std::optional<UiActionRequest>> TryDequeue();

        /**
         * @brief Dispatches one exact request through a borrowed handler.
         * @param request Request previously admitted by this owner generation.
         * @param handler Synchronous consumer that does not retain Runtime UI values.
         * @return Validated result correlated to the same request, or a typed boundary failure.
         */
        [[nodiscard]] Result<UiActionResult> Dispatch(const UiActionRequest &request, UiActionHandler &handler);

        /** @brief Dequeues and dispatches one request, returning empty when no work is queued. */
        [[nodiscard]] Result<std::optional<UiActionResult>> DispatchNext(UiActionHandler &handler);

        /** @brief Returns the exact owner/revision evidence. @return Borrowed immutable context. */
        [[nodiscard]] const UiActionOwnerContext &Owner() const noexcept;
        /** @brief Returns current queued request count. @return Bounded count. */
        [[nodiscard]] std::size_t QueuedCount() const noexcept;
        /** @brief Closes admission while allowing already queued values to be drained or discarded. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently stops dispatch and releases queued requests. */
        void Shutdown() noexcept;
        /** @brief Returns the explicit lifecycle state. */
        [[nodiscard]] UiActionRouterState State() const noexcept;

    private:
        struct Storage;
        explicit UiActionRouter(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
