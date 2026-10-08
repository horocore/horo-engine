#pragma once

/** @file MixerSnapshot.h
 * @brief Bounded persisted mixer presets and atomic sample-boundary transition ownership.
 */
#include "Horo/Audio/ScheduledAudioCommandBatch.h"

#include <array>
#include <span>

namespace Horo::Audio {
    inline constexpr std::size_t MaximumMixerSnapshotParameters = MaximumScheduledAudioCommands / 2;
    inline constexpr std::size_t MaximumMixerSnapshotNameBytes = 63;
    inline constexpr std::size_t MaximumMixerSnapshotSerializedBytes = 128 + MaximumMixerSnapshotParameters * 40;

    /** @brief Persistent mixer parameter reference; no runtime handles, generations or editor solo state. */
    struct MixerSnapshotParameter final {
        AudioParameterTargetKind kind{AudioParameterTargetKind::Bus};
        AudioBusId bus;
        AudioRouteId send;
        AudioEffectId effect;
        AudioParameterId parameter;
        float value{};
        bool operator==(const MixerSnapshotParameter &) const = default;
    };

    /** @brief Version-one detached named preset. Names are display-only; identity references resolve off callback. */
    struct MixerSnapshotAsset final {
        Assets::AssetId mixer;
        std::array<char, MaximumMixerSnapshotNameBytes + 1> name{};
        std::uint32_t parameterCount{};
        std::array<MixerSnapshotParameter, MaximumMixerSnapshotParameters> parameters{};
        bool operator==(const MixerSnapshotAsset &) const = default;
    };
    /** @brief Fixed outcomes; every rejected operation preserves output, automation and transition ownership. */
    enum class MixerSnapshotStatus : std::uint8_t {
        Ok,
        InvalidAsset,
        InvalidBinding,
        InvalidTransition,
        InvalidSerialization,
        UnsupportedVersion,
        Capacity,
        Precedence,
        Duplicate,
        NotFound,
        AutomationRejected
    };
    /** @brief Validate a detached preset without resolution. @param asset Version-one value. @return Shape/duplicate validation. */
    [[nodiscard]] MixerSnapshotStatus ValidateMixerSnapshot(const MixerSnapshotAsset &asset) noexcept;
    /** @brief Encode version-one canonical little-endian data, excluding all editor state.
     * @param asset Valid preset. @param bytes Caller-owned output. @param written Assigned only on success.
     * @return Ok or validation/capacity failure; output is unchanged on rejection.
     */
    [[nodiscard]] MixerSnapshotStatus SerializeMixerSnapshot(const MixerSnapshotAsset &asset, std::span<std::byte> bytes,
                                                             std::size_t &written) noexcept;
    /** @brief Decode bounded version-one data with exact framing and stable references.
     * @param bytes Complete encoded value. @param asset Assigned only on successful validation.
     * @return Typed malformed, version or validation outcome; unknown/trailing fields are rejected.
     */
    [[nodiscard]] MixerSnapshotStatus DeserializeMixerSnapshot(std::span<const std::byte> bytes, MixerSnapshotAsset &asset) noexcept;

    /** @brief Complete retained transition sidecar; the control owner pins it through callback acknowledgement.
     * Apply through MixerSnapshotTransitions after dispatching its single scheduled-batch command.
     * The batch alone cannot carry snapshot precedence or replacement ownership.
     */
    struct PreparedMixerSnapshot final {
        std::uint64_t transitionId{};
        std::int32_t priority{};
        ScheduledAudioCommandBatch batch;
    };

    /** @brief Owned transition intent; clock, scope and identity are retained through callback admission. */
    struct MixerSnapshotTransitionRequest final {
        AudioCommandScope scope;
        AudioCommandTarget target;
        std::uint64_t transitionId{};
        std::uint64_t firstRequestId{};
        std::int32_t priority{};
        std::uint32_t durationFrames{}; /**< Immediate requires zero and uses binding smoothing. */
        AudioAutomationCurve curve{AudioAutomationCurve::Immediate};
    };

    /** @brief Resolve every persisted target against host-admitted exact immutable bindings off callback.
     * @param asset Preset. @param mixer Exact asset identity being resolved. @param bindings Host-retained live bindings.
     * @param transition Exact retained scope/sample boundary, unused increasing IDs, priority and interpolation policy.
     * @param prepared Assigned only on success.
     * @return Structural preparation outcome; engine capacity/ranges/continuity are rechecked atomically on Apply.
     * Higher priority wins while active; equal priority later transitions replace. Immediate requires zero duration;
     * curved/linear durations obey binding continuity. The host validates graph liveness and retains bindings through
     * detachment; this function does not discover services.
     */
    [[nodiscard]] MixerSnapshotStatus PrepareMixerSnapshot(const MixerSnapshotAsset &asset, const Assets::AssetId &mixer,
                                                           std::span<const AudioAutomationParameter> bindings,
                                                           const MixerSnapshotTransitionRequest &transition,
                                                           PreparedMixerSnapshot &prepared) noexcept;

    /** @brief Callback-exclusive owner of one bounded preset transition, borrowing a sealed automation engine.
     * Same callback owns both objects. Advance automation at each sample before dispatch; Value supplies host-prepared
     * bus/send/DSP bindings for that sample. Replacement cancels old trajectories at their current value before admitting
     * all new ones atomically. Completed groups release precedence. Rejection must be retained/reconciled by the host.
     * No method allocates, locks, logs or resolves resources. Close/detach before destroying either borrowed owner.
     */
    class MixerSnapshotTransitions final {
    public:
        /** @brief Borrow an exclusively owned sealed engine. @param automation Must outlive this owner. */
        explicit MixerSnapshotTransitions(AudioParameterAutomation &automation) noexcept : automation_(&automation) {}

        MixerSnapshotTransitions(const MixerSnapshotTransitions &) = delete;
        MixerSnapshotTransitions &operator=(const MixerSnapshotTransitions &) = delete;
        /** @brief Atomically start a complete retained preset at its exact dispatched boundary.
         * @param prepared Retained normalized intent; caller data is never modified.
         * @return Precedence/replay rejection or atomic automation outcome. IDs advance only on success.
         */
        [[nodiscard]] MixerSnapshotStatus Apply(const PreparedMixerSnapshot &prepared) noexcept;
        /** @brief Cancel this group's remaining trajectories atomically, holding current values.
         * @param transitionId Exact owned group. @param target Current exact sample/clock boundary.
         * @return Ok, NotFound or rejected stale/closed engine outcome; future unrelated work remains intact.
         */
        [[nodiscard]] MixerSnapshotStatus Cancel(std::uint64_t transitionId, const AudioCommandTarget &target) noexcept;

        /** @brief Validate an explicit borrowed engine during host composition. @param automation Candidate owner.
         * @return True only for the exact engine supplied to this controller's constructor.
         */
        [[nodiscard]] bool UsesAutomation(const AudioParameterAutomation &automation) const noexcept {
            return automation_ == &automation;
        }

        /** @brief Last underlying atomic engine result. @return Fixed diagnostic outcome, never authoritative mix state. */
        [[nodiscard]] AudioAutomationStatus AutomationStatus() const noexcept {
            return status_;
        }

    private:
        /** @brief Append cancellation only for requests still owned by this transition. */
        void AppendCancellation(ScheduledAudioCommandBatch &batch) const noexcept;
        /** @brief Test remaining ownership without changing engine state. */
        [[nodiscard]] bool Active() const noexcept;
        AudioParameterAutomation *automation_;
        AudioCommandScope scope_;
        std::array<std::uint64_t, MaximumMixerSnapshotParameters> requests_{};
        std::uint32_t count_{};
        std::uint64_t transitionId_{};
        std::int32_t priority_{};
        AudioAutomationStatus status_{AudioAutomationStatus::Ok};
    };
}  // namespace Horo::Audio
