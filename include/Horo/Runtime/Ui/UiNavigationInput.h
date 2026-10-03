#pragma once

/**
 * @file UiNavigationInput.h
 * @brief Owner-thread canonical Input to Runtime UI navigation and glyph composition.
 */

#include "Horo/Runtime/Input.h"
#include "Horo/Runtime/Ui/UiFocusGraph.h"

namespace Horo::Runtime::Ui {
    inline constexpr std::size_t UiNavigationActionCount = static_cast<std::size_t>(UiNavigationDirection::Count);

    /** @brief Builds core keyboard/D-pad/left-stick actions for host composition. @param context Explicit UI input context. @return Eight
     * digital descriptors and ui.navigate Axis2D; allocation is confined to composition. */
    [[nodiscard]] std::vector<Input::ActionDescriptor> DefaultUiNavigationActions(const Input::InputContextId &context);

    /** @brief Closed adapter outcome; blocked input cannot replay on resume. */
    enum class UiNavigationInputStatus : std::uint8_t {
        Active,
        Blocked,
        DuplicateFrame,
        NeedsRebind,
        Stopped
    };

    /** @brief Construction-time navigation bindings and monotonic unscaled repeat policy. */
    struct UiNavigationInputDescriptor final {
        std::array<Input::ActionId, UiNavigationActionCount> actions; /**< Indexed by UiNavigationDirection; all are required. */
        Input::ActionId directionalAxis{std::string{"ui.navigate"}};  /**< Canonical signed Axis2D action for the left stick. */
        std::optional<Input::PlayerId> player; /**< Explicit Input assignment; never derived from a UI player handle. */
        std::uint32_t repeatDelayMilliseconds{350};
        std::uint32_t repeatIntervalMilliseconds{90};
        std::uint32_t modalityHysteresisMilliseconds{100};
    };

    /** @brief Immutable presentation evidence; glyphs reference canonical controls rather than icon/atlas resources. */
    struct UiInputPresentation final {
        Input::InputModality modality{Input::InputModality::Unknown};
        std::optional<Input::GamepadDeviceId> device;
        std::uint64_t revision{1};
        std::array<Input::InputGlyphPresentation, UiNavigationActionCount> glyphs{};
    };

    /** @brief Bounded routing result: at most one focus move and one owner-fenced action; simultaneous cancel wins submit. */
    struct UiNavigationInputFrame final {
        UiNavigationInputStatus status{UiNavigationInputStatus::Blocked};
        std::optional<UiFocusChange> focus;
        std::array<std::optional<UiActionRequestId>, 2> requests{};
        UiInputPresentation presentation;
    };

    /**
     * @brief Production adapter over the existing Input router, focus graph and action queue.
     * @details One adapter belongs to one exact Input token and UI player/layer generation. Creation/rebind copy
     *          bounded effective bindings; Pump allocates no storage on success and performs no callbacks or I/O.
     *          Owners are borrowed only during calls. Stored addresses serve identity checks and are never dereferenced.
     *          Submit/cancel enter the ordinary action queue; its consumer owns control/route behavior and pending actions.
     *          Glyph updates never mutate focus, modal restoration, controls, or admitted action state.
     */
    class UiNavigationInput final {
    public:
        UiNavigationInput(UiNavigationInput &&) noexcept = default;
        UiNavigationInput &operator=(UiNavigationInput &&) noexcept = default;
        UiNavigationInput(const UiNavigationInput &) = delete;
        UiNavigationInput &operator=(const UiNavigationInput &) = delete;
        /**
         * @brief Validates and binds the actual production composition, starting with neutral held state.
         * @param descriptor Required semantic actions, explicit player assignment, and finite policies.
         * @param router Input router owning the committed snapshot and action map.
         * @param context Exact matching live UI context token; its address need not persist.
         * @param focus Existing last-presented focus graph.
         * @param actions Existing same-owner action queue.
         * @return Adapter or typed invalid, stale, capacity, or unsupported navigation failure.
         */
        [[nodiscard]] static Result<UiNavigationInput> Create(const UiNavigationInputDescriptor &descriptor,
                                                              const Input::InputRouter &router, const Input::InputContextToken &context,
                                                              const UiFocusGraph &focus, const UiActionRouter &actions);
        /**
         * @brief Routes one committed snapshot with a monotonic unscaled host clock.
         * @param router Same Input owner as creation.
         * @param context Same exact token identity as creation.
         * @param focus Same focus owner, scope, and last-presented revisions as the current binding.
         * @param actions Same action owner and revisions as the current binding.
         * @param milliseconds Monotonic host time; at most one repeat is emitted per frame, without catch-up loops.
         * @return Immutable result or typed stale/time/capacity failure. A duplicate frame has no effects.
         * @details Call before gameplay capture. Preemption, focus/device/assignment loss, modal changes and rebind
         *          disarm held actions until neutral. The host must call Suspend before an unobserved suspension.
         */
        [[nodiscard]] Result<UiNavigationInputFrame> Pump(Input::InputRouter &router, const Input::InputContextToken &context,
                                                          UiFocusGraph &focus, UiActionRouter &actions, std::uint64_t milliseconds);
        /**
         * @brief Adopts a complete published reload or effective binding replacement without touching queued actions.
         * @param router Original Input owner.
         * @param context Original live token.
         * @param focus Original UI scope with replacement revisions already published.
         * @param actions Matching action queue for that published generation.
         * @return Success or typed rejection; failed rebind preserves previous presentation/binding state.
         */
        [[nodiscard]] Result<void> Rebind(const Input::InputRouter &router, const Input::InputContextToken &context,
                                          const UiFocusGraph &focus, const UiActionRouter &actions);
        /** @brief Disarms input and pending modality evidence before host suspension; focus/actions stay owner-held. */
        void Suspend() noexcept;
        /** @brief Idempotently closes adapter admission without invoking any borrowed owner. */
        void Shutdown() noexcept;
        /** @brief Returns an immutable copied glyph/modality projection. @return Current presentation evidence. */
        [[nodiscard]] UiInputPresentation Presentation() const noexcept;

    private:
        UiNavigationInput() = default;

        /** @brief One fixed-size post-routing action projection; all sources are copied values. */
        struct Samples {
            std::array<Input::ActionEvidence, UiNavigationActionCount> digital;
            Input::ActionEvidence axis;
            std::optional<Input::ActionSource> meaningful;
        };

        /** @brief Candidate presentation evidence awaiting the finite modality hysteresis deadline. */
        struct PendingPresentation {
            Input::InputModality modality{Input::InputModality::Unknown};
            std::optional<Input::GamepadDeviceId> device;
            std::uint64_t since{};
        };

        UiNavigationInputDescriptor descriptor_;
        UiFocusOwnerContext owner_;
        const Input::InputRouter *routerIdentity_{};
        std::uint64_t contextIdentity_{};
        std::uint64_t configurationRevision_{};
        std::uint64_t assignmentRevision_{};
        std::array<std::array<std::optional<Input::InputGlyphId>, 2>, UiNavigationActionCount> glyphs_{};
        std::array<bool, UiNavigationActionCount> disarmed_{};
        bool axisDisarmed_{true};
        std::array<bool, UiNavigationActionCount> heldOwned_{};
        bool axisOwned_{};
        std::optional<UiNavigationDirection> repeating_;
        std::optional<UiFocusModalId> modal_;
        std::uint64_t repeatAt_{};
        PendingPresentation pending_;
        Input::FrameNumber frame_{};
        std::uint64_t time_{};
        bool hasFrame_{};
        bool stopped_{};
        UiInputPresentation presentation_;

        /** @brief Validates exact owners, lifecycle and finite input scan limits before reading input. */
        [[nodiscard]] Result<void> ValidateCall(const Input::InputRouter &router, const Input::InputContextToken &context,
                                                const UiFocusGraph &focus, const UiActionRouter &actions) const;
        /** @brief Applies ordered frame admission and configuration replacement fencing. */
        [[nodiscard]] Result<UiNavigationInputStatus> AdmitFrame(const Input::InputRouter &router, const Input::InputRoutingState &routing,
                                                                 std::uint64_t milliseconds);
        /** @brief Neutralizes modal/assignment/device changes without changing focus or action state. */
        void ReconcileLifecycle(const Input::InputRouter &router, const Input::InputRoutingState &routing,
                                const UiFocusSnapshot &focus) noexcept;
        /** @brief Copies nine bounded semantic samples and tracks only transitions consumed by this adapter. */
        [[nodiscard]] Result<Samples> ReadSamples(Input::InputRouter &router, const Input::InputContextToken &context);
        /** @brief Resolves one stable direction and one bounded repeat without a catch-up loop. */
        [[nodiscard]] Result<std::optional<UiFocusChange>> Navigate(const Samples &samples, UiFocusGraph &focus,
                                                                    std::uint64_t milliseconds);
        /** @brief Admits at most one focused action, with cancel precedence, through the existing queue. */
        [[nodiscard]] Result<std::array<std::optional<UiActionRequestId>, 2>> QueueActivation(const Samples &samples,
                                                                                              const UiFocusGraph &focus,
                                                                                              UiActionRouter &actions) const;
        /** @brief Rebuilds presentation only; saturation never wraps its diagnostic revision. */
        void SetPresentation(Input::InputModality modality, std::optional<Input::GamepadDeviceId> device) noexcept;
        /** @brief Applies eligible post-filter evidence with finite hysteresis on the host clock. */
        void ObserveModality(const std::optional<Input::ActionSource> &source, std::uint64_t milliseconds) noexcept;
    };
}  // namespace Horo::Runtime::Ui
