#pragma once
#include "Horo/XR/XRSimulatorRuntime.h"
#include "XRFakeRuntimeHostComposition.h"
#include "XRSimulatorCodecInternal.h"
#include "host/XRSimulatorHostComposition.h"

#include <functional>
#include <thread>

namespace Horo::XR {
    struct XRSimulatorRuntime::Impl final : IXRSessionResources {
        struct Binding final {
            XRTrackedDeviceRole role{};
            XRTrackedActionKind kind{};
        };

        struct Bindings final {
            XRSessionId session;
            const XRActionBindingCoordinator *owner{};
            std::uint64_t revision{};
            std::array<Binding, XRFakeLimits::MaximumActions> values{};
            std::size_t count{};
            XRSimulatorBindingFingerprint fingerprint{};
        };

        const XRRuntimeGeneration incarnation;
        IXRSessionResources *children{};
        std::function<const XRActionBindingCoordinator *()> query;
        RuntimeInternal::XRFakeRuntimeHostComposition host{this};
        Bindings candidateBindings;
        Bindings activeBindings;
        XRSimulatorBindingFingerprint expectedFingerprint{};
        XRSimulatorBindingFingerprint captureFingerprint{};
        std::optional<XRSimulatorScenario> scenario;
        XRSimulatorStep controls;
        std::optional<XRSessionEvent> pendingEvent;
        XRSimulationTime simulationTime;
        XRSimulatorPlayback playback{XRSimulatorPlayback::Paused};
        bool replay{};
        std::size_t cursor{};
        std::uint64_t time{};
        std::uint64_t cutoff{};
        std::uint64_t lastFrameSequence{};
        XRSimulatorCaptureLimits captureLimits;
        XRSimulatorCaptureSnapshot capture;
        XRSimulatorInitialState captureInitial{XRSimulatorInitialState::Focused};
        std::vector<XRSimulatorStep> recorded;
        std::size_t recordedNodes{};
        bool busy{};
        bool shutdown{};
        bool shutdownRequested{};
        const std::thread::id lane{std::this_thread::get_id()};
        explicit Impl(XRRuntimeGeneration incarnation, IXRSessionResources *resources = nullptr,
                      std::function<const XRActionBindingCoordinator *()> bindings = {});
        ~Impl() override;
        [[nodiscard]] Result<void> Lane() const;
        void Close() noexcept;
        void Finish() noexcept;
        [[nodiscard]] Result<void> Activate(XRSimulatorInitialState initial);
        [[nodiscard]] Result<void> Bootstrap(XRSimulatorInitialState initial);
        [[nodiscard]] Result<void> Prepare(XRSessionPreparation stage, const XRSessionId &candidate, const XRFeaturePlan &plan) override;
        void Release(XRSessionPreparation stage, const XRSessionId &session) noexcept override;
        [[nodiscard]] Result<void> CopyBindings(const XRResolvedActionBindings &plan);
        [[nodiscard]] Result<void> PrepareBindings(const XRSessionId &candidate);
        [[nodiscard]] Result<void> CheckBindings() const;
        [[nodiscard]] Result<void> ValidateControlBindings(const XRSimulatorStep &step) const;
        [[nodiscard]] Result<XRSimulatorAdvance> Advance(XRSimulatorCutoff hostCutoff);
        [[nodiscard]] Result<void> Publish(const XRSimulatorStep &step, const XRFakeOutcome &outcome);
        [[nodiscard]] Result<void> PublishRecords(
            const XRSimulatorStep &step, const XRFakeOutcome &outcome,
            const std::array<RuntimeInternal::TrackingSourceEvidence, XRSimulatorHardLimits::MaximumSources> &sources);
        [[nodiscard]] std::size_t ActionRecords(const XRSimulatorStep &step, const XRFakeOutcome &outcome,
                                                std::array<XRTrackedActionRecord, XRFakeLimits::MaximumActions> &records) const;
        [[nodiscard]] Result<void> JointRecords(const XRSimulatorStep &step,
                                                std::array<XRHandJointRecord, XRHandTrackingHardLimits::MaximumJoints> &records) const;
        [[nodiscard]] Result<XRSimulatorAdvance> Consume(const XRSimulatorStep &step, XRSimulatorCutoff cutoff);
        [[nodiscard]] Result<XRSimulatorAdvance> CommitConsumption(const XRSimulatorStep &step, XRSimulatorCutoff cutoff,
                                                                   const XRFakeOutcome &outcome, std::uint64_t nextTime);
        [[nodiscard]] Result<void> Disconnect(const XRSimulatorStep &step);
        [[nodiscard]] Result<XRFakeStep> Compile(const XRSimulatorStep &step, std::uint64_t nextTime) const;
        void Record(const XRSimulatorStep &step) noexcept;
        [[nodiscard]] bool FitsCapture(const XRSimulatorStep &step, const SimulatorInternal::EncodedMeasure &measured) const noexcept;
        void EndCapture(XRSimulatorCaptureState state) noexcept;
        [[nodiscard]] Result<void> Arm(XRSimulatorCaptureLimits limits);

        struct Operation final {
            Impl &owner;

            explicit Operation(Impl &value) noexcept : owner(value) {
                owner.busy = true;
            }

            ~Operation() {
                owner.Finish();
            }
        };
    };
}  // namespace Horo::XR
