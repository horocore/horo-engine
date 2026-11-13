#pragma once
/** @file XRSimulatorScenario.h @brief Versioned bounded synthetic simulator controls, independent of live XR identities. */
#include "Horo/XR/XRFakeRuntime.h"
#include "Horo/XR/XRTrackingSnapshot.h"

#include <vector>

namespace Horo::XR {
    inline constexpr std::uint32_t XRSimulatorScenarioVersion = 1;

    namespace XRSimulatorErrors {
        extern const ErrorCodeDescriptor Invalid;
        extern const ErrorCodeDescriptor VersionUnsupported;
        extern const ErrorCodeDescriptor ArtifactInvalid;
        extern const ErrorCodeDescriptor CaptureUnavailable;
    }  // namespace XRSimulatorErrors
    enum class XRSimulatorProvenance : std::uint8_t {
        SyntheticScenario,
        RecordedSimulatorInteraction,
        Count
    };

    /** @brief Authored synthetic pose; each component's validity is independent. */
    struct XRSimulatorPose final {
        Math::Vec3 position{};
        Math::Quaternion orientation{Math::Quaternion::Identity()};
        bool positionValid{true};
        bool orientationValid{true};
    };

    /** @brief One complete normalized role/pose source; explicit loss/disconnect cannot reuse prior poses. */
    struct XRSimulatorSource final {
        XRTrackedDeviceRole role{XRTrackedDeviceRole::Head};
        XRTrackedPoseKind kind{XRTrackedPoseKind::Head};
        XRTrackedDeviceState state{XRTrackedDeviceState::Tracked};
        XRSimulatorPose pose;
    };

    /** @brief Semantic binding index and source role, never a persisted live action/device handle. */
    struct XRSimulatorAction final {
        std::uint32_t bindingIndex{};
        XRTrackedDeviceRole role{XRTrackedDeviceRole::LeftController};
        XRTrackedActionKind kind{XRTrackedActionKind::Boolean};
        bool active{};
        float x{};
        float y{};
    };

    struct XRSimulatorJoint final {
        XRHandedness hand{XRHandedness::Left};
        XRHandJoint joint{XRHandJoint::Palm};
        XRSimulatorPose pose;
        std::optional<float> radiusMeters;
    };

    struct XRSimulatorHardLimits final {
        static constexpr std::size_t MaximumSteps = XRFakeLimits::MaximumSteps;
        static constexpr std::size_t MaximumActions = XRFakeLimits::MaximumActions;
        static constexpr std::size_t MaximumSources = 7;
        static constexpr std::size_t MaximumEncodedBytes = 8U * 1024U * 1024U;
        static constexpr std::size_t MaximumNodes = 1000000;
        static constexpr std::size_t MaximumDepth = 16;
        static constexpr std::size_t MaximumTextBytes = 128;
        static constexpr std::uint64_t MaximumCaptureDurationNanoseconds = 60000000000ULL;
    };

    /** @brief One fixed prepared step. Scenario arrays themselves use bounded heap-owned storage. */
    struct XRSimulatorStep final {
        std::optional<XRSessionEvent> event;
        bool executeFrame{true};
        bool shouldRender{true};
        XRFakeFailure failure{XRFakeFailure::None};
        std::uint64_t deltaNanoseconds{16666667}; /**< Positive frame-relative clock delta; never a live clock value. */
        std::uint32_t viewCount{};
        std::array<XRSimulatorPose, XRFrameHardLimits::MaximumViews> views{};
        std::uint32_t sourceCount{};
        std::array<XRSimulatorSource, XRSimulatorHardLimits::MaximumSources> sources{};
        std::uint32_t actionCount{};
        std::array<XRSimulatorAction, XRSimulatorHardLimits::MaximumActions> actions{};
        std::uint32_t jointCount{};
        std::array<XRSimulatorJoint, XRHandTrackingHardLimits::MaximumJoints> joints{};
    };
    /** @brief Reconstructable initial fake state; replay reaches it only through real canonical events. */
    enum class XRSimulatorInitialState : std::uint8_t {
        Ready,
        Running,
        Visible,
        Focused,
        Count
    };

    /** @brief Fixed synthetic-only purpose/retention/export policy; constants grant no live-data access. */
    struct XRSimulatorCaptureManifest final {
        static constexpr std::uint32_t SimulatorDevelopmentPurpose = 0;
        static constexpr std::uint32_t MemoryUntilExplicitExport = 0;
        static constexpr std::uint32_t SyntheticControlsOnlyExport = 0;
        std::uint32_t maximumSteps{};
        std::uint64_t maximumEncodedBytes{};
        std::uint64_t maximumDurationNanoseconds{};
        std::uint64_t elapsedNanoseconds{};
    };

    using XRSimulatorBindingFingerprint = std::array<std::byte, 32>;

    struct XRSimulatorScenarioDescriptor final {
        std::uint32_t version{XRSimulatorScenarioVersion};
        XRSimulatorProvenance provenance{XRSimulatorProvenance::SyntheticScenario};
        XRSimulatorInitialState initialState{XRSimulatorInitialState::Focused};
        std::span<const XRSimulatorStep> steps;
        bool complete{true}; /**< Partial captures remain visibly partial after import/export. */
        std::uint32_t droppedSteps{};
        XRSimulatorBindingFingerprint bindings{}; /**< Compatibility identity of semantic binding order, never an authorization grant. */
        std::optional<XRSimulatorCaptureManifest> capture;
    };

    /** @brief Immutable copied synthetic/recorded control input. No native snapshot or consent authority is accepted. */
    class XRSimulatorScenario final {
    public:
        /** @brief Validate/copy all bounded input before publication.
         * @param descriptor Version, provenance, initial state and finite steps.
         * @return Immutable scenario or typed invalid/unsupported/capacity error; allocation is lifecycle-only.
         */
        [[nodiscard]] static Result<XRSimulatorScenario> Create(const XRSimulatorScenarioDescriptor &descriptor);
        /** @return Borrowed immutable bounded timeline. */
        [[nodiscard]] std::span<const XRSimulatorStep> Steps() const noexcept;
        /** @return Synthetic or recorded simulator interaction provenance. */
        [[nodiscard]] XRSimulatorProvenance Provenance() const noexcept;
        /** @return Canonical event-derived replay starting state. */
        [[nodiscard]] XRSimulatorInitialState InitialState() const noexcept;
        /** @return Whether recording finalized without a partial cutoff. */
        [[nodiscard]] bool Complete() const noexcept;
        /** @return Number of admitted controls omitted at capture exhaustion. */
        [[nodiscard]] std::uint32_t DroppedSteps() const noexcept;
        /** @return Canonical semantic-order compatibility identity, never authority. */
        [[nodiscard]] const XRSimulatorBindingFingerprint &BindingFingerprint() const noexcept;
        /** @return Explicit synthetic capture bounds and retention policy, if recorded. */
        [[nodiscard]] const std::optional<XRSimulatorCaptureManifest> &CaptureManifest() const noexcept;

    private:
        explicit XRSimulatorScenario(const XRSimulatorScenarioDescriptor &descriptor);
        XRSimulatorProvenance provenance_;
        XRSimulatorInitialState initialState_;
        std::vector<XRSimulatorStep> steps_;
        bool complete_{};
        std::uint32_t droppedSteps_{};
        XRSimulatorBindingFingerprint bindings_{};
        std::optional<XRSimulatorCaptureManifest> capture_;
    };

    /** @brief Validate one prepared step without issuing identity or touching a session.
     * @param step Complete normalized synthetic values. @return Typed schema/value/order/capacity result.
     */
    [[nodiscard]] Result<void> ValidateXRSimulatorStep(const XRSimulatorStep &step);
    /** @brief Parse strict version-one JSON with finite byte/node/depth/text budgets and duplicate/unknown field rejection.
     * @param bytes Owned-by-caller encoded artifact; borrowed only during this call.
     * @return Complete immutable scenario or typed rejection; no session or installed scenario changes.
     */
    [[nodiscard]] Result<XRSimulatorScenario> ParseXRSimulatorScenario(std::span<const std::byte> bytes);
    /** @brief Canonically export bounded synthetic input, never native snapshots or handles.
     * @param scenario Admitted immutable scenario. @return Bounded JSON bytes or typed capacity failure.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeXRSimulatorScenario(const XRSimulatorScenario &scenario);
}  // namespace Horo::XR
