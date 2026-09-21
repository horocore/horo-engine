#pragma once

/**
 * @file UiPointerCapture.h
 * @brief Bounded Runtime UI pointer capture, cancellation, and lifetime ownership.
 */

#include "Horo/Runtime/Ui/UiEventDispatch.h"
#include "Horo/Runtime/Ui/UiPresentationReceipt.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>

namespace Horo::Runtime::Ui {
    inline constexpr std::uint32_t MaximumUiPointerCaptures = 64;

    /** @brief Closed normalized pointer-button vocabulary owned by Runtime UI. */
    enum class UiPointerButton : std::uint8_t {
        Primary,
        Secondary,
        Middle,
        Auxiliary1,
        Auxiliary2,
        Count,
    };

    /** @brief Non-zero normalized pointer identity within one Runtime UI input owner. */
    class UiPointerId final {
    public:
        /** @brief Constructs the reserved invalid pointer identity. */
        UiPointerId() = default;

        /**
         * @brief Validates one normalized pointer identity.
         * @param value Non-zero owner-assigned pointer value.
         * @return Typed pointer identity or UiErrors::PointerCaptureInvalid.
         */
        [[nodiscard]] static Result<UiPointerId> Create(std::uint32_t value);

        /** @brief Returns the owner-assigned normalized value. @return Non-zero value for a valid identity. */
        [[nodiscard]] constexpr std::uint32_t Value() const noexcept {
            return value_;
        }

        /** @brief Checks representation validity without consulting ambient input state. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value_ != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiPointerId &) const noexcept = default;

    private:
        explicit constexpr UiPointerId(const std::uint32_t value) noexcept : value_(value) {}

        std::uint32_t value_{};
    };

    /** @brief Typed reason why an active pointer capture stopped receiving delivery. */
    enum class UiPointerCaptureCancellationReason : std::uint8_t {
        Explicit,
        PointerReleased,
        Escape,
        FocusLost,
        ModalOpened,
        DeviceDisconnected,
        OwnerDestroyed,
        ContextRemoved,
        RouteReplaced,
        TargetDestroyed,
        InteractionRevisionLost,
        ScopeDestroyed,
        ViewportDestroyed,
        Suspended,
        Reload,
        Shutdown,
        Count,
    };

    /** @brief Observable state of one move-only pointer-capture lease. */
    enum class UiPointerCaptureState : std::uint8_t {
        Inactive,
        Active,
        Cancelled,
    };

    /** @brief Exact current owner and generation evidence used to reconcile pointer captures. */
    struct UiPointerCaptureContext final {
        RuntimeUiInputContextId context;   /**< Exact Runtime UI input audience/viewport context. */
        UiRenderViewId view;               /**< Exact presented view incarnation. */
        RuntimeUiInstanceId instance;      /**< Exact mutable Runtime UI instance. */
        UiCanvasInstanceId canvas;         /**< Exact Runtime UI canvas incarnation. */
        UiDocumentId document;             /**< Stable source document identity. */
        UiRuntimeTreeRevision tree;        /**< Current retained-tree revision. */
        UiInteractionRevision interaction; /**< Last successfully presented interaction revision. */

        /** @brief Validates all owner and revision identities without consulting live state. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Complete capture admission evidence copied into one lease. */
    struct UiPointerCaptureRequest final {
        RuntimeUiInputContextId context;                /**< Exact input context owning this pointer gesture. */
        UiPointerId pointer;                            /**< Exact normalized pointer source. */
        UiPointerButton button{UiPointerButton::Count}; /**< Button that initiated the gesture. */
        UiRenderViewId view;                            /**< Exact view receiving continued pointer delivery. */
        UiEventRoute route;                             /**< Frozen target route and tree/interaction lineage. */

        /** @brief Validates typed identities, button vocabulary, and same-owner lineage. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    namespace Detail {
        struct UiPointerCaptureStorage;
    }

    class UiPointerCaptureStore;

    /**
     * @brief Move-only RAII lease for one pointer capture.
     * @details The lease owns no tree, handler, callback, or platform object. Its storage lease keeps cancellation state
     *          valid when the capture store is retired or destroyed before the caller releases this token.
     */
    class UiPointerCaptureToken final {
    public:
        UiPointerCaptureToken() = default;
        ~UiPointerCaptureToken();
        UiPointerCaptureToken(UiPointerCaptureToken &&other) noexcept;
        UiPointerCaptureToken &operator=(UiPointerCaptureToken &&other) noexcept;
        UiPointerCaptureToken(const UiPointerCaptureToken &) = delete;
        UiPointerCaptureToken &operator=(const UiPointerCaptureToken &) = delete;

        /** @brief Releases this lease without reporting a cancellation. */
        void Release() noexcept;
        /** @brief Reports whether the lease still owns an active capture. */
        [[nodiscard]] bool IsActive() const noexcept;
        /** @brief Returns the current observable lease state. */
        [[nodiscard]] UiPointerCaptureState State() const noexcept;
        /** @brief Returns the exact cancellation reason after automatic or explicit cancellation. */
        [[nodiscard]] std::optional<UiPointerCaptureCancellationReason> CancellationReason() const noexcept;
        /**
         * @brief Returns the copied capture request.
         * @return Borrowed immutable request valid until this token is released or moved from.
         */
        [[nodiscard]] const UiPointerCaptureRequest &Request() const noexcept;

    private:
        friend class UiPointerCaptureStore;

        UiPointerCaptureToken(std::shared_ptr<Detail::UiPointerCaptureStorage> storage, std::uint32_t slot, std::uint64_t generation,
                              UiPointerCaptureRequest request) noexcept;

        std::shared_ptr<Detail::UiPointerCaptureStorage> storage_;
        std::uint32_t slot_{};
        std::uint64_t generation_{};
        UiPointerCaptureRequest request_{};
    };

    /** @brief Explicit lifecycle of one bounded pointer-capture authority. */
    enum class UiPointerCaptureStoreState : std::uint8_t {
        Active,
        Retiring,
        Stopped,
    };

    /** @brief Owner generation and finite capture-slot policy for one Runtime UI service. */
    struct UiPointerCaptureStoreDescriptor final {
        UiOwnershipGeneration ownership; /**< Exact Runtime UI service generation. */
        std::uint32_t maximumCaptures{}; /**< Fixed number of simultaneous capture leases. */

        /** @brief Validates owner identity and the supported finite capture bound. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Sole owner of bounded Runtime UI pointer-capture leases.
     * @details Admission validates the last-presented interaction and resident target. Capture, release, cancellation, and
     * reconciliation are owner-thread operations with no frame-hot allocation or blocking work. A cancelled token retains
     * its slot until the caller releases it, preventing a stale token from observing a reused capture.
     */
    class UiPointerCaptureStore final {
    public:
        /**
         * @brief Creates an active store with all capture slots preallocated.
         * @param descriptor Exact Runtime UI owner generation and finite capture capacity.
         * @return Active store or typed validation/allocation failure.
         */
        [[nodiscard]] static Result<UiPointerCaptureStore> Create(const UiPointerCaptureStoreDescriptor &descriptor);
        ~UiPointerCaptureStore();
        UiPointerCaptureStore(UiPointerCaptureStore &&) noexcept;
        UiPointerCaptureStore &operator=(UiPointerCaptureStore &&) noexcept;
        UiPointerCaptureStore(const UiPointerCaptureStore &) = delete;
        UiPointerCaptureStore &operator=(const UiPointerCaptureStore &) = delete;

        /**
         * @brief Captures one resident target for one context/pointer gesture.
         * @param request Typed context, pointer, initiating button, view, and frozen route evidence.
         * @param tree Active retained tree that owns the target route.
         * @param presented Last-presentation tracker that must contain the request interaction revision.
         * @return Move-only capture lease or typed malformed, stale, interaction, busy, capacity, or lifecycle failure.
         */
        [[nodiscard]] Result<UiPointerCaptureToken> Capture(const UiPointerCaptureRequest &request, const UiElementTree &tree,
                                                            const UiPresentedInteractionState &presented);

        /**
         * @brief Releases a matching active capture without requiring its token.
         * @param context Exact owning input context.
         * @param pointer Exact normalized pointer source.
         * @param button Initiating button used to identify the capture.
         * @return True when a capture was released, false when no active match exists, or a typed input/lifecycle failure.
         */
        [[nodiscard]] Result<bool> ReleasePointer(RuntimeUiInputContextId context, UiPointerId pointer, UiPointerButton button);

        /**
         * @brief Cancels one matching active capture and preserves its reason on the lease token.
         * @param context Exact owning input context.
         * @param pointer Exact normalized pointer source.
         * @param reason Typed cancellation reason to preserve on the lease.
         * @return Number of captures cancelled, or a typed input/lifecycle failure.
         */
        [[nodiscard]] Result<std::uint32_t> CancelPointer(RuntimeUiInputContextId context, UiPointerId pointer,
                                                          UiPointerCaptureCancellationReason reason);
        /**
         * @brief Cancels all captures owned by one exact input context.
         * @param context Exact input context to cancel.
         * @param reason Typed cancellation reason to preserve on each lease.
         * @return Number of captures cancelled, or a typed input/lifecycle failure.
         */
        [[nodiscard]] Result<std::uint32_t> CancelContext(RuntimeUiInputContextId context, UiPointerCaptureCancellationReason reason);
        /**
         * @brief Cancels all captures for one exact Runtime UI instance generation.
         * @param instance Exact runtime instance to cancel.
         * @param reason Typed cancellation reason to preserve on each lease.
         * @return Number of captures cancelled, or a typed input/lifecycle failure.
         */
        [[nodiscard]] Result<std::uint32_t> CancelInstance(RuntimeUiInstanceId instance, UiPointerCaptureCancellationReason reason);
        /**
         * @brief Cancels all captures for one exact canvas incarnation.
         * @param canvas Exact canvas to cancel.
         * @param reason Typed cancellation reason to preserve on each lease.
         * @return Number of captures cancelled, or a typed input/lifecycle failure.
         */
        [[nodiscard]] Result<std::uint32_t> CancelCanvas(UiCanvasInstanceId canvas, UiPointerCaptureCancellationReason reason);
        /**
         * @brief Cancels all captures for one exact rendered view incarnation.
         * @param view Exact rendered view to cancel.
         * @param reason Typed cancellation reason to preserve on each lease.
         * @return Number of captures cancelled, or a typed input/lifecycle failure.
         */
        [[nodiscard]] Result<std::uint32_t> CancelView(UiRenderViewId view, UiPointerCaptureCancellationReason reason);
        /**
         * @brief Cancels every active capture, preserving exactly-once reasons on their tokens.
         * @param reason Typed cancellation reason to preserve on each lease.
         * @return Number of captures cancelled, or a typed lifecycle failure.
         */
        [[nodiscard]] Result<std::uint32_t> CancelAll(UiPointerCaptureCancellationReason reason);

        /**
         * @brief Cancels captures whose copied route no longer matches current presented evidence.
         * @param current Current context, owner, tree, and presented interaction evidence.
         * @param tree Current active retained tree for the context.
         * @param presented Current last-presentation tracker for the context view/canvas.
         * @return Number of captures reconciled, or a typed malformed, stale, or lifecycle failure.
         * @post Tree replacement, target disappearance, or interaction replacement cancels the old lease before slot reuse.
         */
        [[nodiscard]] Result<std::uint32_t> Reconcile(const UiPointerCaptureContext &current, const UiElementTree &tree,
                                                      const UiPresentedInteractionState &presented);

        /** @brief Closes new capture admission while existing leases can be cancelled or released. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently cancels active captures with Shutdown and stops new admission. */
        void Shutdown() noexcept;
        /** @brief Returns the explicit store lifecycle state. */
        [[nodiscard]] UiPointerCaptureStoreState State() const noexcept;
        /** @brief Reports whether every capture slot is free or permanently retired. */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Returns the number of currently active leases. */
        [[nodiscard]] std::uint32_t ActiveCount() const noexcept;

    private:
        explicit UiPointerCaptureStore(std::shared_ptr<Detail::UiPointerCaptureStorage> storage) noexcept;

        std::shared_ptr<Detail::UiPointerCaptureStorage> storage_;
    };
}  // namespace Horo::Runtime::Ui
