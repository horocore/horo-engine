#pragma once

/**
 * @file UiAnimationClock.h
 * @brief Generation-scoped Runtime UI domain snapshots and checked playback time.
 */

#include "Horo/Runtime/Ui/UiIdentity.h"

#include <array>
#include <cstdint>
#include <optional>

namespace Horo::Runtime {
    class UiAnimationRuntimeParticipant;
}

namespace Horo::Runtime::Ui {
    /** @brief Closed UI domains; none grants access to a native or wall clock. */
    enum class UiTimeDomain : std::uint8_t {
        Simulation,
        PresentationUnscaled,
        ScreenTransition,
        EditorPreview,
        DeterministicTest,
        Manual,
        Count,
    };
    inline constexpr std::size_t UiTimeDomainCount = static_cast<std::size_t>(UiTimeDomain::Count);

    struct UiAnimationClockHandleTag;
    /** @brief Owner-allocated domain identity; never serialized or substituted for a host clock. */
    using UiAnimationClockId = UiRuntimeHandle<UiAnimationClockHandleTag>;

    struct UiAnimationHostSourceHandleTag;
    /** @brief Distinct owner-issued application adapter binding; never substituted for a UI clock or element handle. */
    using UiAnimationHostSourceId = UiRuntimeHandle<UiAnimationHostSourceHandleTag>;

    /** @brief Signed nanosecond duration; admission rejects negative authored durations. */
    struct UiDuration final {
        std::int64_t nanoseconds{};
        [[nodiscard]] constexpr auto operator<=>(const UiDuration &) const noexcept = default;
    };

    /** @brief Non-negative local playback ratio; direction is a separate policy. */
    struct UiPlaybackRate final {
        std::uint32_t numerator{1};
        std::uint32_t denominator{1};

        /** @brief Checks the representation; normalization and bounds are validated at admission. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return denominator != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiPlaybackRate &) const noexcept = default;
    };

    /** @brief Exact sub-nanosecond remainder preserved across rational playback-rate changes. */
    struct UiTimeRemainder final {
        std::uint32_t numerator{};
        std::uint32_t denominator{1};
        [[nodiscard]] constexpr auto operator<=>(const UiTimeRemainder &) const noexcept = default;
    };

    /** @brief Continuity of one admitted UI source, independent from timeline traversal direction. */
    enum class UiClockContinuity : std::uint8_t {
        Initial,
        Continuous,
        BaselineReset,
        ExplicitSeek,
        Held,
    };

    /** @brief Copied immutable evidence for one exact UI domain generation. */
    struct UiClockSample final {
        UiAnimationClockId clock;
        UiTimeDomain domain{UiTimeDomain::PresentationUnscaled};
        std::uint64_t sequence{};
        UiDuration elapsed;
        UiDuration delta;
        std::uint64_t sourceRevision{};
        UiClockContinuity continuity{UiClockContinuity::Initial};
        bool available{};
        UiRouteStackId routeStack;               /**< Present only for a real screen-transition reservation. */
        UiRouteInstanceId route;                 /**< Exact pre-issued entering or retained exiting route incarnation. */
        UiRouteOperationSequence routeOperation; /**< Exact stack-issued operation opening this child clock. */
    };

    /** @brief Complete frozen domain samples consumed once by a VariableUpdate candidate. */
    struct UiClockSnapshot final {
        std::uint64_t updateSequence{};
        std::array<UiClockSample, UiTimeDomainCount> domains;
    };

    /**
     * @brief Copied domain observations delivered only through a privately issued actual application read.
     * @details Constructing this inert value grants no authority. Preview/test/manual availability and exact step/seek commands
     *          come from the application participant's issued controller; runtime domains come from its verified dispatch source.
     */
    struct UiAnimationDomainInput final {
        UiDuration delta;
        std::optional<UiDuration> seek;
        UiAnimationClockId controlledClock; /**< Actual issued preview/test/manual incarnation; invalid for host-derived domains. */
        std::uint64_t sourceRevision{};
        UiClockContinuity continuity{UiClockContinuity::Initial};
        bool available{};
    };

    /**
     * @brief Stack-borrowed authoritative host evidence constructible only by the real Runtime adapter.
     * @details The adapter derives committed simulation duration from matching staged fixed attempts and the scheduler's
     *          completed tick. Presentation baseline/continuity comes from FrameContext. No caller-supplied commitment flag,
     *          raw clock, retained context reference, or native identity is accepted. The read must not outlive its callback.
     */
    class UiAnimationHostRead final {
    public:
        UiAnimationHostRead(const UiAnimationHostRead &) = delete;
        UiAnimationHostRead &operator=(const UiAnimationHostRead &) = delete;
        UiAnimationHostRead(UiAnimationHostRead &&) = delete;
        UiAnimationHostRead &operator=(UiAnimationHostRead &&) = delete;

        /** @brief Returns the admitted UI owner source binding. @return Exact owner-issued binding. */
        [[nodiscard]] UiAnimationHostSourceId Source() const noexcept {
            return source_;
        }

        /** @brief Returns the actual scheduler frame. @return Monotonic frame ordinal. */
        [[nodiscard]] std::uint64_t Frame() const noexcept {
            return frame_;
        }

        /** @brief Returns the actual committed fixed tick. @return Scheduler commitment revision. */
        [[nodiscard]] std::uint64_t SimulationTick() const noexcept {
            return simulationTick_;
        }

        /** @brief Returns the sum of newly committed matching fixed durations. @return Copied duration. */
        [[nodiscard]] UiDuration SimulationDelta() const noexcept {
            return domains_[static_cast<std::size_t>(UiTimeDomain::Simulation)].delta;
        }

        /** @brief Returns the host-normalized presentation duration. @return Copied duration. */
        [[nodiscard]] UiDuration PresentationDelta() const noexcept {
            return domains_[static_cast<std::size_t>(UiTimeDomain::PresentationUnscaled)].delta;
        }

        /** @brief Returns the host presentation baseline generation. @return Exact producer revision. */
        [[nodiscard]] std::uint64_t PresentationGeneration() const noexcept {
            return presentationGeneration_;
        }

        /** @brief Reports a producer-admitted reset, without inferring it from zero delta. @return Reset evidence. */
        [[nodiscard]] bool PresentationReset() const noexcept {
            return presentationReset_;
        }

        /** @brief Reports the actual host clamp diagnostic. @return Whether the host clamped this sample. */
        [[nodiscard]] bool PresentationClamped() const noexcept {
            return presentationClamped_;
        }

        /** @brief Borrows copied actual source/controller observations; this return value grants no mutation authority. @return Six closed
         * inputs. */
        [[nodiscard]] const std::array<UiAnimationDomainInput, UiTimeDomainCount> &Domains() const noexcept {
            return domains_;
        }

    private:
        friend class Horo::Runtime::UiAnimationRuntimeParticipant;

        /** @brief Copied producer facts grouped to keep construction explicit and bounded. */
        struct Facts final {
            std::uint64_t frame{};
            std::uint64_t simulationTick{};
            std::array<UiAnimationDomainInput, UiTimeDomainCount> domains;
            std::uint64_t presentationGeneration{};
            bool presentationReset{};
            bool presentationClamped{};
        };

        UiAnimationHostRead(UiAnimationHostSourceId source, const Facts &facts) noexcept
            : source_(source), frame_(facts.frame), simulationTick_(facts.simulationTick), domains_(facts.domains),
              presentationGeneration_(facts.presentationGeneration), presentationReset_(facts.presentationReset),
              presentationClamped_(facts.presentationClamped) {}

        UiAnimationHostSourceId source_;
        std::uint64_t frame_{};
        std::uint64_t simulationTick_{};
        std::array<UiAnimationDomainInput, UiTimeDomainCount> domains_;
        std::uint64_t presentationGeneration_{};
        bool presentationReset_{};
        bool presentationClamped_{};
    };
}  // namespace Horo::Runtime::Ui
