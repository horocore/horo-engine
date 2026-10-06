#pragma once

/** @file UiAnimationTimeline.h
 * @brief Inert typed UI timeline policy and immutable playback outcomes; no host clock or callback authority.
 */
#include "Horo/Runtime/Ui/UiAnimationClock.h"

namespace Horo::Runtime::Ui {
    struct UiAnimationIdentityTag;
    struct UiAnimationTimelineHandleTag;
    /** @brief Stable authored track-group identity, independent of transient clock/cursor state. */
    using UiAnimationId = UiStableId<UiAnimationIdentityTag>;
    /** @brief Actual owner-issued timeline incarnation; never serialized or substituted for an element/clock handle. */
    using UiAnimationTimelineId = UiRuntimeHandle<UiAnimationTimelineHandleTag>;

    /** @brief Direction is local to playback and never reverses a host domain. */
    enum class UiPlaybackDirection : std::uint8_t {
        Forward,
        Reverse,
        Alternating,
        AlternatingReverse
    };
    /** @brief Finite iterations complete; infinite iteration is permitted only for non-blocking presentation motion. */
    enum class UiLoopKind : std::uint8_t {
        Finite,
        Infinite
    };
    /** @brief Declares whether values participate before delay and after completion. */
    enum class UiAnimationFill : std::uint8_t {
        None,
        Backwards,
        Forwards,
        Both
    };
    /** @brief Explicit permission for a rational rate of zero. */
    enum class UiZeroRatePolicy : std::uint8_t {
        Reject,
        Hold
    };
    /** @brief Required route motion needs a separate exact UiScreenStack-issued gate; metadata alone grants none. */
    enum class UiAnimationLifecycle : std::uint8_t {
        NonBlocking,
        RequiredEnter,
        RequiredExit
    };

    /** @brief Finite authored iteration count; ignored only by explicitly admitted non-blocking infinite policy. */
    struct UiLoopPolicy final {
        UiLoopKind kind{UiLoopKind::Finite};
        std::uint32_t iterations{1};
        [[nodiscard]] constexpr auto operator<=>(const UiLoopPolicy &) const noexcept = default;
    };

    /**
     * @brief Inert duration/rate/direction policy normalized before instance admission.
     * @details Delay/duration are non-negative. Finite total duration must fit signed nanoseconds. Infinite loops require
     *          positive duration and NonBlocking lifecycle. The owning application resolves accessibility policy before admission;
     *          this value neither selects reduced-motion rules nor registers a clock, route, service or callback.
     */
    struct UiAnimationTimePolicy final {
        UiTimeDomain domain{UiTimeDomain::PresentationUnscaled};
        UiDuration delay;
        UiDuration duration;
        UiPlaybackRate rate;
        UiPlaybackDirection direction{UiPlaybackDirection::Forward};
        UiLoopPolicy loop;
        UiAnimationFill fill{UiAnimationFill::Forwards};
        UiZeroRatePolicy zeroRate{UiZeroRatePolicy::Reject};
        UiAnimationLifecycle lifecycle{UiAnimationLifecycle::NonBlocking};
        [[nodiscard]] constexpr auto operator<=>(const UiAnimationTimePolicy &) const noexcept = default;
    };

    /** @brief Explicit timeline lifecycle; retirement does not change or republish a terminal outcome. */
    enum class UiAnimationState : std::uint8_t {
        Created,
        Waiting,
        Running,
        Held,
        Completed,
        Cancelled,
        Retiring,
        Destroyed
    };
    /** @brief Mutually exclusive exactly-once outcome for one timeline generation. */
    enum class UiAnimationOutcome : std::uint8_t {
        None,
        Completed,
        Cancelled
    };
    /** @brief Typed cancellation correlation; no arbitrary callback or downstream mutation is implied. */
    enum class UiAnimationCancellation : std::uint8_t {
        None,
        Explicit,
        ElementRemoved,
        TargetIncompatible,
        RouteRetired,
        OwnerRetired,
        Reload,
        DependencyFailure,
        Deadline,
        Shutdown,
        AccessibilityReplacement
    };

    /** @brief Copied evaluated playback evidence; publication authority belongs to the actual aggregate owner. */
    struct UiAnimationPlaybackSample final {
        UiAnimationState state{UiAnimationState::Created};
        UiAnimationOutcome outcome{UiAnimationOutcome::None};
        UiAnimationCancellation cancellation{UiAnimationCancellation::None};
        UiDuration elapsed;
        std::uint64_t iteration{};
        std::uint32_t progress{};          /**< Exact endpoints zero/UINT32_MAX; intermediate progress is rounded down deterministically. */
        std::uint32_t crossedIterations{}; /**< Work required by this update, checked before publication. */
        bool contributesValue{};           /**< Fill/delay policy result; this flag grants no target or lifecycle admission. */
        bool newTerminalOutcome{};         /**< Only the successful aggregate candidate may publish this local event once. */
    };
}  // namespace Horo::Runtime::Ui
