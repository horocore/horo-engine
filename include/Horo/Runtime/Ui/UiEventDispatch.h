#pragma once

/**
 * @file UiEventDispatch.h
 * @brief Bounded Runtime UI capture, target, bubble, and default-action dispatch.
 */

#include "Horo/Runtime/Ui/UiElementTree.h"
#include "Horo/Runtime/Ui/UiLayout.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace Horo::Runtime::Ui {
    /** @brief Closed normalized event vocabulary consumed by shared Runtime UI controls. */
    enum class UiEventKind : std::uint8_t {
        PointerMove,
        PointerPress,
        PointerRelease,
        Submit,
        Cancel,
        Gesture,
        Count,
    };

    /** @brief Closed UI-local gesture vocabulary; semantic effects still require a routed default decision. */
    enum class UiGestureKind : std::uint8_t {
        Press,
        Release,
        Tap,
        DoubleTap,
        LongPress,
        PanBegin,
        PanUpdate,
        PanEnd,
        PinchRotate,
        DragBegin,
        DragUpdate,
        Drop,
        Cancel,
        HoverEnter,
        HoverMove,
        HoverLeave,
        Count
    };

    /** @brief Owned bounded gesture evidence, with no payload callback, device handle or application object. */
    struct UiGestureEvent final {
        UiGestureKind kind{UiGestureKind::Count};
        std::uint32_t pointer{}; /**< Owner-local pointer, or zero for the keyboard/accessibility alternative. */
        UiElementHandle source;  /**< Exact original presented source; a drop's routed target may differ. */
        UiLogicalPoint delta;
        double scale{1.0}; /**< Finite positive relative two-contact scale. */
        double rotation{}; /**< Finite relative angle in radians. */
        bool accessible{}; /**< Explicit focused alternative, not synthetic physical input. */
        /** @brief Checks closed kinds, finite geometry and exact source representation. @return Whether dispatch may inspect it. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Ordered routed-event phase. */
    enum class UiEventPhase : std::uint8_t {
        Capture,
        Target,
        Bubble,
        Count,
    };

    /** @brief Immutable event payload already normalized and targeted by the Runtime UI owner. */
    struct UiRoutedEvent final {
        UiEventKind kind{};                    /**< Typed normalized event kind. */
        std::uint64_t sequence{};              /**< Non-zero owner-ordered event sequence. */
        UiLogicalPoint logicalPosition{};      /**< Canvas-space point when hasLogicalPosition is true. */
        bool hasLogicalPosition{};             /**< Whether the event carries pointer position. */
        std::optional<UiGestureEvent> gesture; /**< Present only for Gesture; copied, never a lazy query. */
    };

    /** @brief Exact immutable source lineage and route boundary for one dispatch. */
    struct UiEventRoute final {
        RuntimeUiInstanceId instance;             /**< Exact runtime document instance. */
        UiCanvasInstanceId canvas;                /**< Exact runtime canvas incarnation. */
        UiDocumentId document;                    /**< Stable source document. */
        UiRuntimeTreeRevision tree;               /**< Exact tree revision used for targeting. */
        UiInteractionRevision interaction;        /**< Exact presented interaction revision. */
        UiElementHandle target;                   /**< Exact targeted element. */
        std::optional<UiElementHandle> modalRoot; /**< Inclusive modal boundary, when active. */
    };

    /** @brief Per-phase handler decision with independent handled, propagation, and default semantics. */
    struct UiEventResponse final {
        bool handled{};         /**< Records semantic handling without implicitly stopping routing. */
        bool stopPropagation{}; /**< Stops later capture/target/bubble callbacks. */
        bool preventDefault{};  /**< Suppresses the target's default action. */
    };

    /** @brief Bounded dispatch outcome suitable for a later input-consumption ledger. */
    struct UiEventDispatchResult final {
        std::uint32_t phasesVisited{}; /**< Number of handler callbacks invoked. */
        bool handled{};                /**< Whether any handler marked the event handled. */
        bool propagationStopped{};     /**< Whether a handler stopped the remaining route. */
        bool defaultPrevented{};       /**< Whether any handler suppressed the default action. */
        bool defaultApplied{};         /**< Whether the target default action completed. */
    };

    /** @brief Borrowed synchronous handler; implementations retain neither event nor element references. */
    class UiEventHandler {
    public:
        virtual ~UiEventHandler() = default;

        /**
         * @brief Handles one frozen route entry.
         * @param element Exact element handle for this callback.
         * @param phase Current capture, target, or bubble phase.
         * @param event Immutable normalized event.
         * @return Explicit routing response or typed handler failure.
         */
        [[nodiscard]] virtual Result<UiEventResponse> Handle(UiElementHandle element, UiEventPhase phase, const UiRoutedEvent &event) = 0;

        /**
         * @brief Applies the target's default action after routing when not prevented.
         * @param target Exact still-resident target.
         * @param event Immutable normalized event.
         * @return Success or typed handler failure.
         */
        [[nodiscard]] virtual Result<void> ApplyDefault(UiElementHandle target, const UiRoutedEvent &event) = 0;
    };

    /** @brief Lifecycle of one preallocated owner-thread dispatcher. */
    enum class UiEventDispatcherState : std::uint8_t {
        Active,
        Retiring,
        Stopped,
    };

    /** @brief Fixed owner and route-depth limits for one dispatcher. */
    struct UiEventDispatcherDescriptor final {
        RuntimeUiInstanceId instance;      /**< Exact runtime document instance. */
        UiCanvasInstanceId canvas;         /**< Exact runtime canvas incarnation. */
        UiDocumentId document;             /**< Stable source document. */
        std::uint32_t maximumRouteDepth{}; /**< Positive root-inclusive route bound. */

        /** @brief Validates identities and the route bound. @return Whether creation can reserve safely. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Preallocated non-reentrant capture/target/bubble dispatch authority. */
    class UiEventDispatcher final {
    public:
        /**
         * @brief Creates an active dispatcher and reserves its complete route capacity.
         * @param descriptor Exact owner identities and finite route bound.
         * @return Active dispatcher or typed validation/allocation failure.
         */
        [[nodiscard]] static Result<UiEventDispatcher> Create(const UiEventDispatcherDescriptor &descriptor);
        ~UiEventDispatcher();
        UiEventDispatcher(UiEventDispatcher &&) noexcept;
        UiEventDispatcher &operator=(UiEventDispatcher &&) noexcept;
        UiEventDispatcher(const UiEventDispatcher &) = delete;
        UiEventDispatcher &operator=(const UiEventDispatcher &) = delete;

        /**
         * @brief Routes one event over a frozen handle path while revalidating tree residency after every callback.
         * @param tree Current retained tree owned by the same runtime/canvas/document.
         * @param route Exact target, presented interaction, tree revision, and optional modal root.
         * @param event Immutable normalized event.
         * @param handler Borrowed synchronous handler and default-action boundary.
         * @return Bounded outcome or typed malformed, stale, modal, mutation, reentrancy, lifecycle, or handler failure.
         */
        [[nodiscard]] Result<UiEventDispatchResult> Dispatch(const UiElementTree &tree, const UiEventRoute &route,
                                                             const UiRoutedEvent &event, UiEventHandler &handler);

        /** @brief Closes new dispatch admission. @return Success or lifecycle failure. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently stops dispatch and releases route storage. */
        void Shutdown() noexcept;
        /** @brief Returns the explicit lifecycle state. @return Active, Retiring, or Stopped. */
        [[nodiscard]] UiEventDispatcherState State() const noexcept;

    private:
        struct Storage;
        explicit UiEventDispatcher(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
