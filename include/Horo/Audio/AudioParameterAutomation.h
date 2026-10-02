#pragma once

/** @file AudioParameterAutomation.h
 * @brief Fixed-capacity sample-clock automation with explicit target and continuity ownership.
 */
#include "Horo/Audio/AudioClock.h"

#include <array>
#include <cstddef>

namespace Horo::Audio {
    struct AudioCommand;
    struct ScheduledAudioCommandBatch;
    inline constexpr std::size_t MaximumAudioAutomationParameters = 64;
    inline constexpr std::size_t MaximumAudioAutomationRequests = 128;

    /** @brief Target interpretation is resolved by the host before callback activation. */
    enum class AudioParameterTargetKind : std::uint8_t {
        Voice,
        Bus,
        Send,
        DSP
    };

    /** @brief Exact runtime binding generation; stable IDs never imply a live physical target. */
    struct AudioParameterAddress final {
        AudioParameterTargetKind kind{AudioParameterTargetKind::Voice};
        AudioRuntimeId owner;
        AudioVoiceHandle voice;            /**< Used only for Voice; all other targets require an empty voice. */
        AudioBusId bus;                    /**< Bus/Send/DSP owning bus; empty for Voice. */
        AudioRouteId send;                 /**< Used only for Send. */
        AudioEffectId effect;              /**< Used only for DSP. */
        std::uint64_t bindingGeneration{}; /**< Nonzero graph or voice binding revision, never reused. */
        AudioParameterId parameter;
        auto operator<=>(const AudioParameterAddress &) const noexcept = default;
    };
    /** @brief Interpolation has exact endpoints; curved uses monotone cubic smoothstep. */
    enum class AudioAutomationCurve : std::uint8_t {
        Immediate,
        Linear,
        Smoothstep
    };

    /** @brief Immutable parameter admission and model-unit per-sample continuity limits. */
    struct AudioAutomationParameter final {
        AudioParameterAddress address;
        float minimum{};
        float maximum{1.0F};
        float initialValue{};
        float maximumSampleDelta{0.01F}; /**< Positive model-unit limit; not a perceptual audibility claim. */
        std::uint32_t minimumSmoothingFrames{1};
    };

    /** @brief Owned FIFO intent; request IDs increase within one engine and cannot be reused after cancellation. */
    struct AudioParameterAutomationRequest final {
        AudioParameterAddress address;
        std::uint64_t requestId{};
        std::uint64_t clockGeneration{};
        std::uint64_t discontinuityRevision{};
        std::uint64_t startFrame{};
        std::uint32_t durationFrames{}; /**< Zero for Immediate; linear/curved require positive duration. */
        float targetValue{};
        AudioAutomationCurve curve{AudioAutomationCurve::Immediate};
    };
    /** @brief Fixed-size outcomes; rejected operations leave engine state unchanged. */
    enum class AudioAutomationStatus : std::uint8_t {
        Ok,
        InvalidInput,
        StaleClock,
        Paused,
        Closed,
        Capacity,
        MissingParameter,
        Duplicate,
        Late,
        OutOfRange,
        ContinuityLimit,
        NotFound
    };
    /** @brief Validate target identity shape; liveness and model range remain engine/host authority.
     * @param address Exact typed target with only its applicable identity fields populated.
     * @return True for a well-formed address; no registry or physical binding is consulted.
     */
    [[nodiscard]] bool IsValidAudioParameterAddress(const AudioParameterAddress &address) noexcept;
    /** @brief Validate fixed request shape without resolving a target or acquiring storage.
     * @param request Owned intent carrying nonzero request and clock generations with finite target value.
     * @return True for a structurally valid curve/duration pair; timing, range and capacity admission are separate.
     */
    [[nodiscard]] bool IsValidAudioAutomationRequest(const AudioParameterAutomationRequest &request) noexcept;

    /**
     * @brief Single-owner bounded automation state; prepare off-callback, then transfer exclusively to callback.
     * No method allocates, locks, invokes clients or accesses a registry. The host owns physical bindings and
     * samples Value after Advance for each rendered frame. Do not copy/move a live callback-owned engine.
     * Bind before Seal; reset/discontinuity requires Close, detach and a newly prepared engine.
     */
    class AudioParameterAutomation final {
    public:
        /** @brief Prepare one exact sample epoch. Invalid clocks leave admission closed.
         * @param scene Exact live scene context retained by the host through callback detachment.
         * @param clock Current running nonzero runtime/epoch/clock/discontinuity with valid sample rate.
         */
        explicit AudioParameterAutomation(const AudioSampleClock &clock, AudioSceneContextHandle scene) noexcept;
        /** @brief Admit one immutable binding before activation. @param parameter Range and tolerance.
         * @return Ok, InvalidInput, Duplicate, Capacity or Closed. */
        [[nodiscard]] AudioAutomationStatus Bind(const AudioAutomationParameter &parameter) noexcept;
        /** @brief Freeze bindings before transferring to callback ownership. @return Ok or Closed. */
        [[nodiscard]] AudioAutomationStatus Seal() noexcept;
        /** @brief Queue FIFO intent after normal command consumption. @param request Exact bound request.
         * @return Typed admission result. Curved/linear durations must bound worst-case admitted range slope;
         * Immediate uses the configured minimum plus range-derived smoothing to respect the same limit.
         * Late requests are rejected; equal-start requests apply in increasing admitted request-ID order.
         */
        [[nodiscard]] AudioAutomationStatus Schedule(const AudioParameterAutomationRequest &request) noexcept;
        /** @brief Advance through all due events, rebasing overlaps at their exact start value.
         * @param clock Exact epoch snapshot at a nondecreasing sample frame; paused does not mutate state.
         * @return Ok or typed closed, clock, pause or late rejection. Bound: 128 events and 64 parameters.
         */
        [[nodiscard]] AudioAutomationStatus Advance(const AudioSampleClock &clock) noexcept;
        /** @brief Consume one normalized ordinary FIFO/batch child at the current sample boundary.
         * @param command Automation, cancellation or matching unload/reset barrier from the normal command path.
         * @return Admission/cancel outcome; wrong scene/runtime/epoch or clock is rejected without mutation.
         * Other payloads are InvalidInput and remain the host's responsibility. This does not drain transport.
         */
        [[nodiscard]] AudioAutomationStatus Apply(const AudioCommand &command) noexcept;
        /** @brief Atomically consume a retained automation-only scheduled batch at its exact host-dispatched boundary.
         * @param batch Normalized owned children; every child must be automation/cancel or a closing barrier.
         * @return Ok only when every child admits; failure retains all state and request IDs unchanged.
         * Mixed host-owned commands are InvalidInput and require host-level aggregate admission.
         * Bounded fixed-state transaction uses one engine-sized stack copy, without allocation.
         */
        [[nodiscard]] AudioAutomationStatus ApplyBatch(const ScheduledAudioCommandBatch &batch) noexcept;
        /** @brief Cancel one admitted request at the last advanced sample, holding its current value.
         * @param requestId ID of pending or currently active trajectory; removing an older replaced ID is NotFound.
         * @return Ok, NotFound or Closed. Future requests on the same target remain queued.
         */
        [[nodiscard]] AudioAutomationStatus Cancel(std::uint64_t requestId) noexcept;
        /** @brief Copy the current exact-binding value without host lookup. @param address Exact target.
         * @param value Assigned only on Ok. @return Ok, MissingParameter or Closed. */
        [[nodiscard]] AudioAutomationStatus Value(const AudioParameterAddress &address, float &value) const noexcept;
        /** @brief Close admission and discard pending/active automation; retain no external references. */
        void Close() noexcept;

    private:
        AudioParameterAutomation(const AudioParameterAutomation &) = default;
        AudioParameterAutomation &operator=(const AudioParameterAutomation &) = default;

        struct ParameterState {
            AudioAutomationParameter descriptor;
            float value{};
            float from{};
            float target{};
            std::uint64_t start{};
            std::uint32_t duration{};
            std::uint64_t activeId{};
            AudioAutomationCurve curve{AudioAutomationCurve::Linear};
        };

        struct Pending {
            AudioParameterAutomationRequest request;
            std::size_t parameterIndex{};
            std::uint32_t duration{};
        };

        /** @brief Locate one exact immutable binding in the bounded table. */
        [[nodiscard]] std::size_t Find(const AudioParameterAddress &address) const noexcept;
        /** @brief Sample every active trajectory at one nondecreasing event/sample boundary. */
        void Sample(std::uint64_t frame) noexcept;
        AudioSampleClock clock_;
        AudioSceneContextHandle scene_;
        std::array<ParameterState, MaximumAudioAutomationParameters> parameters_{};
        std::array<Pending, MaximumAudioAutomationRequests> pending_{};
        std::size_t parameterCount_{};
        std::size_t pendingCount_{};
        std::uint64_t lastRequestId_{};
        bool sealed_{};
        bool closed_{true};
    };
}  // namespace Horo::Audio
