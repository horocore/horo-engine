#pragma once

/** @file UiPointerInteraction.h
 * @brief Owner-thread bounded pointer/gesture recognition over actual presented hit testing and routed defaults.
 */
#include "Horo/Runtime/Ui/UiHitTesting.h"
#include "Horo/Runtime/Ui/UiPointerCapture.h"

#include <array>
#include <span>

namespace Horo::Runtime::Ui {
    inline constexpr std::size_t MaximumUiInteractionPointers = 16;
    inline constexpr std::size_t MaximumUiInteractionSamples = 64;
    inline constexpr std::size_t MaximumUiInteractionTargets = 256;

    /** @brief Normalized physical contact edge after Input context admission; touch has no hover. */
    enum class UiPointerEdge : std::uint8_t {
        Move,
        Press,
        Release,
        Cancel,
        Count
    };
    /** @brief Physical interaction behavior, distinct from device, player and input context identities. */
    enum class UiPointerModality : std::uint8_t {
        Mouse,
        Touch,
        Pen,
        Count
    };
    /** @brief Explicit focused alternatives to spatial-only gestures. */
    enum class UiAccessibleGesture : std::uint8_t {
        Activate,
        ContextAction,
        PickUp,
        Drop,
        Cancel,
        Count
    };

    /** @brief One normalized viewport-pixel sample copied from admitted immutable input. */
    struct UiPointerSample final {
        UiPointerId pointer;
        UiPointerEdge edge{UiPointerEdge::Count};
        UiPointerModality modality{UiPointerModality::Count};
        UiPointerButton button{UiPointerButton::Primary};
        float pixelX{};
        float pixelY{};
    };

    /** @brief Exact-generation authored gesture eligibility; no callback or payload object is retained. */
    struct UiPointerTargetPolicy final {
        UiElementHandle element;
        bool draggable{};
        bool dropTarget{};
    };

    /** @brief Immutable construction policy; replacing the presented generation requires a new owner, never pointer migration. */
    struct UiPointerInteractionDescriptor final {
        UiPointerCaptureContext owner;
        std::optional<UiElementHandle> modalRoot;
        std::uint32_t pointerCapacity{MaximumUiInteractionPointers};
        std::int32_t dragThreshold{6 * 64}; /**< Positive logical 1/64-DIP distance, independent of viewport scale. */
        std::uint64_t longPressMilliseconds{500};
        std::uint64_t doubleTapMilliseconds{300};
    };

    /** @brief Synchronous actual-owner borrows; none escape recognition/dispatch. */
    struct UiPointerInteractionEnvironment final {
        const UiElementTree &tree;
        const UiHitTestSnapshot &hitTesting;
        const UiPresentedInteractionState &presented;
        UiPointerCaptureStore &captures;
        UiEventDispatcher &dispatcher;
        UiEventHandler &handler; /**< Existing route/default authority, not an immediate gameplay callback. */
    };

    /** @brief Bounded recognized-frame evidence; prevention applies before drag/drop default state advances. */
    struct UiPointerInteractionResult final {
        std::uint32_t dispatched{};
        bool handled{};
        bool defaultPrevented{};
        std::uint32_t activePointers{};
        std::uint32_t defaultActions{}; /**< Actual aggregate control defaults copied into the caller's bounded output. */
    };

    /**
     * @brief Sole transient gesture owner for one exact context/view/presented generation.
     * @details Owns fixed slots and actual capture leases, not a mutable tree, hit-test publisher, focus graph or action owner.
     *          Creation is load-time. Successful pumping has no allocation, blocking, catch-up loop or retained owner borrow.
     *          Input admission and aggregate publication remain the composing host's authority. Handler errors preserve their
     *          identity and cancel local gestures; already applied routed defaults are an explicit ordered prefix, not rolled back.
     */
    class UiPointerInteraction final {
    public:
        /** @brief Copies complete bounded policy and validates exact handle ownership.
         * @param descriptor Owner generation, timing and capacity bounds. @param targets Complete authored target policy.
         * @return Owned recognizer or typed invalid/stale/capacity failure; no owner is activated by a descriptor.
         */
        [[nodiscard]] static Result<UiPointerInteraction> Create(const UiPointerInteractionDescriptor &descriptor,
                                                                 std::span<const UiPointerTargetPolicy> targets);
        ~UiPointerInteraction();
        /** @brief Transfers a quiescent owner; moving/destructing a pumping owner violates the borrow contract and terminates. */
        UiPointerInteraction(UiPointerInteraction &&) noexcept;
        UiPointerInteraction &operator=(UiPointerInteraction &&) noexcept;
        UiPointerInteraction(const UiPointerInteraction &) = delete;
        UiPointerInteraction &operator=(const UiPointerInteraction &) = delete;

        /** @brief Recognizes a bounded admitted batch against the last successfully presented hit-test snapshot.
         * @param environment Actual same-generation owners, borrowed until return.
         * @param canvasSpace Exact resolved screen-canvas mapping used by that presented snapshot.
         * @param samples At most 64 ordered normalized samples, already admitted by the Input context.
         * @param milliseconds Monotonic unscaled owner time; equal times are allowed, backwards time rejects before effects.
         * @param nextSequence Application-owned next routed event sequence, shared with keyboard/control delivery.
         * @return Bounded result or typed geometry, timing, ownership, capture, capacity or handler error.
         * @post Any lost capture is cancelled before another gesture default. No long-press catch-up burst is generated.
         */
        [[nodiscard]] Result<UiPointerInteractionResult> Pump(const UiPointerInteractionEnvironment &environment,
                                                              const UiResolvedScreenCanvas &canvasSpace,
                                                              std::span<const UiPointerSample> samples, std::uint64_t milliseconds,
                                                              std::uint64_t &nextSequence);
        /** @brief Routes a focused keyboard/accessibility alternative through the same gesture/default authority.
         * @param environment Actual same-generation owners. @param focused Actual presented focus target supplied by the focus owner.
         * @param gesture Explicit semantic alternative. @param nextSequence Shared next routed event sequence.
         * @return Routed outcome or typed stale/unsupported/capacity/lifecycle failure; no synthetic mouse contact is created.
         */
        [[nodiscard]] Result<UiPointerInteractionResult> Accessible(const UiPointerInteractionEnvironment &environment,
                                                                    UiElementHandle focused, UiAccessibleGesture gesture,
                                                                    std::uint64_t &nextSequence);
        /** @brief Closes admission and releases owned leases without calling a borrowed owner.
         * @note The aggregate owner cancels its control states at the same lifecycle boundary; no gesture migrates across reload.
         */
        void Shutdown() noexcept;

        /** @brief Borrows immutable source fencing, not publication authority. @return Exact construction binding. */
        [[nodiscard]] const UiPointerCaptureContext &Owner() const noexcept {
            return descriptor_.owner;
        }

        /** @brief Copies the frozen inclusive modal boundary. @return Root or no modal. */
        [[nodiscard]] std::optional<UiElementHandle> ModalRoot() const noexcept {
            return descriptor_.modalRoot;
        }

        /** @brief Reports whether the focused alternative owns an admitted pick-up. @return Whether activation should request Drop. */
        [[nodiscard]] bool HasAccessibleDrag() const noexcept {
            return accessibleDrag_.has_value();
        }

        /** @brief Copies targets needing lifecycle cleanup, including externally cancelled leases. @return Fixed invalid-padded handles. */
        [[nodiscard]] std::array<UiElementHandle, MaximumUiInteractionPointers + 2> TargetsInFlight() const noexcept;
        /** @brief Releases all transient captures and tap history while preserving this generation's admission. */
        void CancelTransient() noexcept;

    private:
        UiPointerInteraction() = default;

        struct Contact final {
            UiPointerCaptureToken capture;
            UiPointerModality modality{UiPointerModality::Count};
            UiLogicalPoint origin;
            UiLogicalPoint position;
            std::uint64_t pressedAt{};
            bool longPressed{};
            bool panning{};
            bool dragging{};
            bool multiTouch{};   /**< Participation disarms tap/long-press until both physical contacts end. */
            bool exceededSlop{}; /**< Physical movement disarms click/hold recognition even when a begin default is prevented. */
        };

        UiPointerInteractionDescriptor descriptor_;
        std::array<UiPointerTargetPolicy, MaximumUiInteractionTargets> targets_{};
        std::size_t targetCount_{};
        std::array<Contact, MaximumUiInteractionPointers> contacts_{};
        std::optional<UiElementHandle> accessibleDrag_;
        UiElementHandle hovered_;
        UiElementHandle lastTap_;
        UiLogicalPoint lastTapPosition_;
        std::uint64_t lastTapTime_{};
        std::uint64_t time_{};
        std::uint64_t transientRevision_{}; /**< Synchronous cancellation fence; dispatch cannot revive cleared contacts. */
        bool pumping_{};
        bool stopped_{};

        /** @brief Admits actual owner lineage and presentation before any dispatch. @return Admission or typed failure. */
        [[nodiscard]] Result<void> Validate(const UiPointerInteractionEnvironment &environment) const;
        /** @brief Resolves one complete copied target policy without allocation. @return Policy or null. */
        [[nodiscard]] const UiPointerTargetPolicy *Policy(UiElementHandle target) const noexcept;
        /** @brief Routes a closed gesture and accumulates bounded consumption evidence. @return Default admitted or typed failure. */
        [[nodiscard]] Result<bool> Emit(const UiPointerInteractionEnvironment &environment, UiElementHandle target,
                                        const UiGestureEvent &gesture, UiLogicalPoint position, std::uint64_t &nextSequence,
                                        UiPointerInteractionResult &result);
        /** @brief Applies one admitted sample to its actual capture slot. @return Ordered prefix or typed failure. */
        [[nodiscard]] Result<void> ProcessSample(const UiPointerInteractionEnvironment &environment, const UiScreenPointerQuery &query,
                                                 const UiPointerSample &sample, std::uint64_t &nextSequence,
                                                 UiPointerInteractionResult &result);
        /** @brief Emits at most one long press per held slot without catch-up. @return Routed admission or typed failure. */
        [[nodiscard]] Result<void> Tick(const UiPointerInteractionEnvironment &environment, std::uint64_t &nextSequence,
                                        UiPointerInteractionResult &result);
        /** @brief Routes mouse-only hover transitions without inventing a touch contact or capture.
         * @param environment Actual presented owners. @param target Current hit, or invalid for surface exit.
         * @param sample Admitted physical mouse evidence. @param position Logical position.
         * @param nextSequence Shared counter. @param result Applied prefix. @return Dispatch or typed route failure.
         */
        [[nodiscard]] Result<void> Hover(const UiPointerInteractionEnvironment &environment, UiElementHandle target,
                                         const UiPointerSample &sample, UiLogicalPoint position, std::uint64_t &nextSequence,
                                         UiPointerInteractionResult &result);
    };
}  // namespace Horo::Runtime::Ui
