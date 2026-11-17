#pragma once
/** @file XRSimulatorRuntime.h @brief Explicit headset-free control, genuine fake-session replay and bounded interaction recording. */
#include "Horo/XR/XRSimulatorScenario.h"

#include <memory>
#include <string_view>

namespace Horo::XR {
    namespace RuntimeInternal {
        class XRSimulatorHostAccess;
    }
    enum class XRSimulatorPlayback : std::uint8_t {
        Paused,
        Running,
        Replaying,
        Stopped,
        Failed
    };
    enum class XRSimulatorCaptureState : std::uint8_t {
        Idle,
        Armed,
        Capturing,
        Complete,
        Partial,
        Failed,
        Cancelled
    };
    enum class XRSimulatorCaptureChannel : std::uint8_t {
        SimulatorControls,
        NativeTracking,
        Count
    };

    struct XRSimulatorCaptureLimits final {
        std::size_t maximumSteps{XRSimulatorHardLimits::MaximumSteps};
        std::size_t maximumBytes{XRSimulatorHardLimits::MaximumEncodedBytes};
        XRSimulatorCaptureChannel channel{XRSimulatorCaptureChannel::SimulatorControls};
        std::uint64_t maximumDurationNanoseconds{XRSimulatorHardLimits::MaximumCaptureDurationNanoseconds};
    };

    struct XRSimulatorCaptureSnapshot final {
        XRSimulatorCaptureState state{XRSimulatorCaptureState::Idle};
        std::size_t steps{};
        std::size_t bytes{}; /**< Encoded-step bytes plus conservative manifest reservation; never RSS or sizeof payload. */
        std::size_t dropped{};
        std::uint64_t elapsedNanoseconds{};
        XRSimulatorProvenance provenance{XRSimulatorProvenance::RecordedSimulatorInteraction};
    };

    struct XRSimulatorSnapshot final {
        XRSimulatorPlayback playback{XRSimulatorPlayback::Paused};
        XRSessionSnapshot session;
        std::size_t cursor{};
        std::uint64_t relativeNanoseconds{};
        XRSimulatorCaptureSnapshot capture;
        /** @brief Always simulator behavioral evidence, never runtime/headset qualification. */
        static constexpr std::string_view EvidenceLabel = "SimulatorReplay";
    };

    struct XRSimulatorCutoff final {
        std::uint64_t serial{};      /**< Existing host Input cutoff serial, not a new simulation tick. */
        XRSimulationTime simulation; /**< Existing simulation cutoff domain; never inferred from presentation prediction. */
    };

    struct XRSimulatorAdvance final {
        XRFakeOutcome frame;
        bool consumed{};
        std::size_t cursor{};
        std::uint64_t relativeNanoseconds{};
    };

    /** @brief Pimpl owns bounded heap-prepared storage; no multi-megabyte scenario object lives on a caller's stack. */
    class XRSimulatorRuntime final {
    public:
        /** @brief Prepare one explicit synthetic owner with canonical fake session/frame/tracking parents.
         * @return Owner or typed capability/preparation failure; no native runtime or renderer is discovered.
         */
        [[nodiscard]] static Result<std::unique_ptr<XRSimulatorRuntime>> Create();
        ~XRSimulatorRuntime();
        XRSimulatorRuntime(const XRSimulatorRuntime &) = delete;
        XRSimulatorRuntime &operator=(const XRSimulatorRuntime &) = delete;
        /** @brief Replace admitted scenario and prepare a fresh genuine session, preserving prior owner on rejection.
         * @param scenario Immutable bounded artifact. @return Typed replacement result; prior owner survives rejected preparation. */
        [[nodiscard]] Result<void> LoadScenario(const XRSimulatorScenario &scenario);
        /** @brief Stage authored simulator controls; effective values and recording change only on admitted consumption.
         * @param controls Complete bounded synthetic values. @return Typed shape, binding or lifecycle result. */
        [[nodiscard]] Result<void> SetControls(const XRSimulatorStep &controls);
        /** @brief Resume admitted interactive/replay work. @return Typed lifecycle result. */
        [[nodiscard]] Result<void> Run();
        /** @brief Pause consumption while retaining the admitted owner. @return Typed lifecycle result. */
        [[nodiscard]] Result<void> Pause();
        /** @brief Prepare a fresh session and replay the loaded artifact. @return Typed replacement result. */
        [[nodiscard]] Result<void> Replay();
        /** @brief Prepare a fresh session and reset interactive controls. @return Typed replacement result. */
        [[nodiscard]] Result<void> Reset();
        /** @brief Apply canonical stopping and terminate recording as partial. @return Typed lifecycle result. */
        [[nodiscard]] Result<void> Stop();
        /** @brief Consume one opportunity only while running/replaying; supplied cutoff is correlation, never a new clock.
         * @param cutoff Existing host Input cutoff serial; positive and monotonic for consumed work.
         * @return Bounded outcome; successful prepared work allocates nothing and performs no I/O.
         */
        [[nodiscard]] Result<XRSimulatorAdvance> Tick(XRSimulatorCutoff cutoff);
        /** @brief Consume exactly one paused opportunity through the same path as Tick.
         * @param cutoff Existing positive monotonic host correlation and simulation domain. @return Typed consumed outcome. */
        [[nodiscard]] Result<XRSimulatorAdvance> Step(XRSimulatorCutoff cutoff);
        /** @brief Arm finite recording of actually consumed simulator controls/events.
         * @param limits Step/byte bounds and channel. NativeTracking is explicitly unsupported, never consent-enabled by a bool.
         * @return Prepared capture or typed invalid/capacity/unsupported/owner failure.
         */
        [[nodiscard]] Result<void> ArmCapture(XRSimulatorCaptureLimits limits = {});
        /** @brief Finalize a nonempty recording or retain a partial result. @return Typed capture result. */
        [[nodiscard]] Result<void> StopCapture();
        /** @brief Discard capture storage on the owner lane; no native data is retained. */
        void CancelCapture() noexcept;
        /** @brief Export only completed/partial nonempty admitted simulator interaction, not original setup.
         * @return Immutable replayable control artifact or typed unavailable/state/capacity failure.
         */
        [[nodiscard]] Result<XRSimulatorScenario> RecordedScenario() const;
        /** @brief Read bounded owner state. @return State, or default state on a foreign control lane. */
        [[nodiscard]] XRSimulatorSnapshot Snapshot() const noexcept;
        /** @brief Borrow the current canonical publication until the next mutation. @return Current snapshot or null when revoked. */
        [[nodiscard]] const XRTrackingSnapshot *Tracking() const noexcept;
        /** @brief Read the prepared synthetic controls on the owner lane. @return Borrowed controls until the next mutation. */
        [[nodiscard]] const XRSimulatorStep &Controls() const noexcept;
        /** @brief Revoke publication and release exact resources once on the creating lane. */
        void Shutdown() noexcept;

    private:
        friend class RuntimeInternal::XRSimulatorHostAccess;
        struct Impl;
        explicit XRSimulatorRuntime(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::XR
