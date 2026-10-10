#pragma once

/** @file UiPointerInput.h
 * @brief Bounded committed Input to actual Runtime UI pointer/control composition.
 */
#include "Horo/Runtime/Input.h"
#include "Horo/Runtime/Ui/UiAnimationOwner.h"

namespace Horo::Runtime::Ui {
    inline constexpr std::size_t UiPointerActionCount = 4;

    /** @brief Constructs canonical activate/context/pick/cancel keyboard actions at host composition.
     * @param context Explicit matching Input context. @return Four remappable descriptors; allocation is load-time only.
     */
    [[nodiscard]] std::vector<Input::ActionDescriptor> DefaultUiPointerActions(const Input::InputContextId &context);

    /** @brief Closed transport status; interrupted held contacts cannot replay on resume. */
    enum class UiPointerInputStatus : std::uint8_t {
        Active,
        Blocked,
        DuplicateFrame,
        NeedsRebind,
        Stopped
    };

    /** @brief Call-duration presented composition and source-to-viewport mapping. */
    struct UiPointerInputSurface final {
        UiPointerInteractionHost &owner;
        const UiHitTestSnapshot &hitTesting;
        const UiResolvedScreenCanvas &canvasSpace;
        UiCanvasPixelRect inputViewport; /**< Explicit viewport in collector coordinate units (SDL window logical units);
                                          * mapped once to canvas pixels, never inferred from a renderer backend. */
        UiEventHandler &routeHandler;
        std::uint64_t &nextSequence; /**< Shared aggregate input counter, including keyboard and control delivery. */
    };

    /** @brief Immutable borrowed output; defaults remain valid until this adapter's next Pump/Rebind/Shutdown. */
    struct UiPointerInputFrame final {
        UiPointerInputStatus status{UiPointerInputStatus::Blocked};
        UiPointerInteractionResult interaction;
        std::span<const UiControlDefaultAction> defaults;
    };

    /**
     * @brief Stable-address production adapter owning bounded contacts, capture and recognition over committed Input snapshots.
     * @details Create reserves all storage. Pump success performs no allocation, I/O or wait. Actual owners are borrowed only
     *          synchronously. Input router capture callbacks only neutralize this adapter; actual control cleanup precedes
     *          the next owner dispatch, or explicit Suspend. Tokens are released before the callback owner is destroyed.
     *          One adapter is composed for one exact live Input token and one presented Runtime UI generation.
     *          Calls are owner-thread synchronous and non-reentrant. Shutdown may occur in a route callback;
     *          destruction during Pump violates its live borrow contract. Rebind is rejected during Pump.
     */
    class UiPointerInput final : private Input::IInputCaptureOwner {
        struct ConstructionKey final {
        private:
            friend class UiPointerInput;
            ConstructionKey() = default;
        };

    public:
        /** @brief Constructs prepared storage using the factory-only key.
         * @param key Private factory admission. @param interaction Validated recognizer.
         */
        explicit UiPointerInput(ConstructionKey key, UiPointerInteraction interaction);

        /** @brief Binds the real router context and copies generation-fenced UI gesture policy at composition.
         * @param router Actual Input owner. @param context Matching live token. @param interaction Complete typed UI owner policy.
         * @param targets Complete copied drag/drop policy. @param actions Four canonical semantic actions in the same Input context.
         * @return Stable-address adapter or typed identity, capacity, binding or allocation failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<UiPointerInput>> Create(
            const Input::InputRouter &router, const Input::InputContextToken &context, const UiPointerInteractionDescriptor &interaction,
            std::span<const UiPointerTargetPolicy> targets, const std::array<Input::ActionId, UiPointerActionCount> &actions);
        ~UiPointerInput();
        UiPointerInput(const UiPointerInput &) = delete;
        UiPointerInput &operator=(const UiPointerInput &) = delete;
        UiPointerInput(UiPointerInput &&) = delete;
        UiPointerInput &operator=(UiPointerInput &&) = delete;

        /** @brief Pumps one committed frame through Input admission, actual presented hit testing and owner-routed defaults.
         * @param router Original Input owner. @param context Original live token. @param surface Exact presented UI and viewport.
         * @param milliseconds Monotonic unscaled clock; backwards clocks/frames reject before consuming any transition.
         * @return Borrowed bounded defaults and consumption status, or typed admission/route failure with observable applied prefix.
         * @note Cancel wins simultaneous activation. Pick toggles pick/drop; activate while picked requests Drop.
         */
        [[nodiscard]] Result<UiPointerInputFrame> Pump(Input::InputRouter &router, const Input::InputContextToken &context,
                                                       const UiPointerInputSurface &surface, std::uint64_t milliseconds);
        /** @brief Cancels actual controls and all held contacts before host suspension/context loss.
         * @param owner Actual aggregate owner. @param nextSequence Shared event counter. @return Cleanup or typed failure.
         */
        [[nodiscard]] Result<void> Suspend(UiPointerInteractionHost &owner, std::uint64_t &nextSequence);
        /** @brief Copies a complete explicitly presented replacement after cancellation; held sources start disarmed.
         * @param owner Aggregate owner for old-control cleanup. @param nextSequence Shared event counter.
         * @param descriptor Replacement exact owner/policy. @param targets Complete replacement drag/drop policy.
         * @return Admission or typed failure, preserving previous binding on failed preparation.
         */
        [[nodiscard]] Result<void> Rebind(UiPointerInteractionHost &owner, std::uint64_t &nextSequence,
                                          const UiPointerInteractionDescriptor &descriptor, std::span<const UiPointerTargetPolicy> targets);
        /** @brief Closes local admission and releases capture tokens without invoking a borrowed UI owner.
         * @note Host calls Suspend first while the aggregate is active; aggregate shutdown already cancels its controls.
         */
        void Shutdown() noexcept;
        /** @brief Observes admitted default prefix even when Pump failed later. @return Borrow valid until next call/shutdown. */
        [[nodiscard]] std::span<const UiControlDefaultAction> Defaults() const noexcept;

    private:
        void OnInputCaptureCancelled(Input::CaptureCancellationReason reason) noexcept override;

        struct Contact final {
            Input::TouchContactId source;
            UiPointerId pointer;
            float x{};
            float y{};
        };

        const Input::InputRouter *router_{}; /**< Compared, never dereferenced outside a borrowed call. */
        std::uint64_t context_{};
        std::uint64_t configurationRevision_{};
        std::uint64_t assignmentRevision_{};
        std::array<Input::ActionId, UiPointerActionCount> actions_;
        UiPointerInteraction interaction_;
        std::optional<UiEventDispatcher> dispatcher_;
        Input::PointerCaptureToken mouseCapture_;
        std::optional<Input::PointerButton> mouseButton_;
        std::array<Contact, Input::MaximumTouchContacts> contacts_{};
        std::array<UiPointerSample, MaximumUiInteractionSamples> samples_{};
        std::array<UiControlDefaultAction, MaximumUiInteractionSamples + 1> defaults_{};
        std::uint32_t defaultCount_{};
        std::uint32_t nextPointer_{2};
        Input::FrameNumber frame_{};
        std::uint64_t time_{};
        bool hasFrame_{};
        bool cleanupPending_{};
        bool stopped_{};
        bool pumping_{};
        /** @brief Maps one bounded physical sample to canvas pixels. */
        void AddSample(const UiPointerInputSurface &surface, std::size_t &count, UiPointerId pointer, UiPointerEdge edge,
                       UiPointerModality modality, UiPointerButton button, float x, float y);
        /** @brief Collects consumed mouse edges and maintains the router capture. */
        Result<void> CollectMouse(Input::InputRouter &router, const Input::InputContextToken &context, const UiPointerInputSurface &surface,
                                  std::size_t &count);
        /** @brief Collects one touch source without consuming another adapter's contact. */
        Result<void> CollectTouch(Input::InputRouter &router, const Input::InputContextToken &context, const UiPointerInputSurface &surface,
                                  const Input::TouchContactState &touch, std::size_t &count);
        /** @brief Collects owned touch edges and cancels missing sources. */
        Result<void> CollectTouches(Input::InputRouter &router, const Input::InputContextToken &context,
                                    const UiPointerInputSurface &surface, std::size_t &count);
        /** @brief Validates a committed frame before consuming any input. */
        Result<UiPointerInputStatus> ValidateFrame(Input::InputRouter &router, const Input::InputContextToken &context,
                                                   const UiPointerInputSurface &surface, std::uint64_t milliseconds) const;
        /** @brief Commits frame identity and revokes stale authority before collection. */
        Result<UiPointerInputStatus> AdmitFrame(Input::InputRouter &router, const Input::InputContextToken &context,
                                                const UiPointerInputSurface &surface, std::uint64_t milliseconds,
                                                const Input::InputRoutingState &routing);
        /** @brief Selects the semantic alternative with cancellation taking precedence. */
        Result<std::optional<UiAccessibleGesture>> ReadAlternative(Input::InputRouter &router, const Input::InputContextToken &context,
                                                                   const UiPointerInputSurface &surface);
        /** @brief Collects and delivers admitted input through the live callback fence. */
        Result<UiPointerInputFrame> DeliverFrame(Input::InputRouter &router, const Input::InputContextToken &context,
                                                 const UiPointerInputSurface &surface, std::uint64_t milliseconds,
                                                 std::optional<UiAccessibleGesture> alternative, const Input::InputRoutingState &routing);
        /** @brief Collects only this adapter's consumed physical edges into fixed storage. @return Sample count or typed capacity failure.
         */
        [[nodiscard]] Result<std::size_t> Collect(Input::InputRouter &router, const Input::InputContextToken &context,
                                                  const UiPointerInputSurface &surface);
    };
}  // namespace Horo::Runtime::Ui
