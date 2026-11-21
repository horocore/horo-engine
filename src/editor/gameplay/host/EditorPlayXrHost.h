#pragma once
/** @file EditorPlayXrHost.h
 * @brief Non-installed explicit application XR bridge for the existing workspace input and Play owners.
 */
#include "EditorPlayState.h"
#include "Horo/Foundation/Time.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/InputCollection.h"
#include "Horo/Runtime/Scene/SceneIdentity.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace Horo::Runtime {
    class SceneActivationParticipant;
    class RuntimeSceneView;
}  // namespace Horo::Runtime

namespace Horo::Render {
    struct RenderOriginLocalScene;
}  // namespace Horo::Render

namespace Horo::XR {
    enum class XRFeatureProfile : std::uint8_t;
    enum class XRSessionState : std::uint8_t;
    enum class XRTrackingState : std::uint8_t;
    enum class XRStageBoundaryProximity : std::uint8_t;
    enum class XRCapability : std::uint8_t;
    enum class XRCapabilityState : std::uint8_t;
    enum class XRFeatureDecisionState : std::uint8_t;
    enum class XRTrackedActionKind : std::uint8_t;
    enum class XRTrackedDeviceState : std::uint8_t;
    enum class XRTrackedDeviceRole : std::uint8_t;
}  // namespace Horo::XR

namespace Horo::Editor {
    class EditorXrSceneSourcePort;

    /** @brief Selected geometry and explicit authored Local reference placement in its exact mounted World origin.
     * @details The shared owner pins geometry and the canonical origin lease. Native reference observation remains separately owned.
     */
    struct EditorMountedXrScene final {
        std::shared_ptr<const Render::RenderOriginLocalScene> geometry;
        Runtime::SceneObjectId trackingRoot;
        Math::Transform originFromLocal;
    };

    /** @brief Captures the exact private Play Scene and explicitly selected authored tracking root. */
    using EditorMountedXrGeometryCapture =
        std::function<Result<EditorMountedXrScene>(const Runtime::RuntimeSceneView &, Runtime::SceneObjectId trackingRoot)>;

    /** @brief Explicit local player selected by application startup for the selected XR Play source. */
    struct EditorXrInputPlayerSelection final {
        Input::PlayerId player{};
    };

    /** @brief One owner-issued test request, valid only while that request is queued. */
    struct EditorXrSetupTestId final {
        std::uint64_t value{};
        constexpr bool operator==(const EditorXrSetupTestId &) const noexcept = default;
    };

    /** @brief Cold probe state exposed identically to graphical and non-graphical callers. */
    enum class EditorXrSetupTestState : std::uint8_t {
        NotRun,
        Queued,
        Passed,
        Failed,
        Cancelled
    };

    /** @brief Actionable redacted reason; raw provider strings never enter the setup surface. */
    enum class EditorXrSetupIssue : std::uint8_t {
        None,
        Busy,
        Unavailable,
        Renderer,
        Unsupported,
        Permission,
        Stale,
        Capacity,
        Invalid,
        Cleanup
    };

    /** @brief Fixed-size setup evidence from the selected application owner. */
    struct EditorXrSetupReport final {
        std::uint16_t loaderMajor{}; /**< Nonzero only after real loader/runtime/system discovery. */
        std::uint16_t loaderMinor{};
        std::uint16_t loaderPatch{};
        std::optional<XR::XRFeatureProfile> requestedProfile;
        bool rendererReady{};  /**< The startup-selected renderer supplied the exact native session binding. */
        bool mayRequestTest{}; /**< Ownership and idle state permit a test or cleanup retry. */
        EditorXrSetupTestState test{EditorXrSetupTestState::NotRun};
        EditorXrSetupIssue issue{EditorXrSetupIssue::Unavailable};
    };

    /** @brief Completeness of the exact selected session's inspection evidence. */
    enum class EditorXrInspectionStatus : std::uint8_t {
        Unavailable,
        Idle,
        WaitingForSample,
        Unfocused,
        EventsPending,
        Current,
        Stale
    };

    /** @brief Only diagnostic summaries cross the editor port. Sensitive raw channels require a separately admitted capture. */
    enum class EditorXrInspectionPrivacy : std::uint8_t {
        Redacted
    };

    /** @brief Aggregate pose metadata with no coordinates, quaternion or native-space handles. */
    struct EditorXrInspectionValidity final {
        std::array<std::uint16_t, 3> position{};    /**< Invalid, inferred, tracked. */
        std::array<std::uint16_t, 3> orientation{}; /**< Invalid, inferred, tracked. */
        std::array<std::uint16_t, 3> confidence{};  /**< Absent, low, high. */
    };

    /** @brief One anatomical source's current lifecycle and redacted joint metadata. */
    struct EditorXrInspectionHand final {
        std::optional<XR::XRTrackedDeviceState> state;
        std::uint32_t generation{};
        EditorXrInspectionValidity validity;
    };

    /** @brief Current observed capability and the separately accepted decision from the same revision. */
    struct EditorXrInspectionCapability final {
        XR::XRCapability kind;
        XR::XRCapabilityState observed;
        XR::XRFeatureDecisionState decision;
    };

    /** @brief One binding-plan sample's typed presence; native paths and payload values are excluded. */
    struct EditorXrInspectionAction final {
        std::uint32_t bindingIndex{};
        std::uint32_t deviceSlot{}; /**< Session-local source slot. */
        std::uint32_t deviceGeneration{};
        std::optional<XR::XRTrackedDeviceRole> role;
        XR::XRTrackedActionKind kind;
        bool active{};
    };

    /** @brief Owned redacted snapshot copied on the application owner lane without any native API access. */
    struct EditorXrInspectionSnapshot final {
        static constexpr std::size_t MaximumDisplayedActions = 12;
        static constexpr std::size_t CapabilityCount = 7;
        EditorXrInspectionStatus status{EditorXrInspectionStatus::Unavailable};
        EditorXrInspectionPrivacy privacy{EditorXrInspectionPrivacy::Redacted};
        std::optional<XR::XRSessionState> session;
        std::optional<XR::XRTrackingState> tracking;
        std::optional<XR::XRStageBoundaryProximity> stageBoundary;
        std::uint64_t runtimeGeneration{};
        std::uint32_t systemGeneration{};
        std::uint32_t sessionGeneration{};
        std::uint64_t capabilityRevision{};
        std::uint64_t trackingRevision{};
        std::uint64_t originRevision{};
        std::uint64_t bindingRevision{};
        std::int64_t sampleNanoseconds{}; /**< Exact admitted runtime cutoff, zero when there is no current sample. */
        bool focused{};
        std::uint16_t deviceCount{};
        std::uint16_t actionCount{};
        std::uint16_t activeActionCount{};
        std::uint16_t omittedActionCount{};
        EditorXrInspectionValidity poses;
        std::array<std::uint16_t, 5> spaces{};         /**< Pose references by canonical view, local, stage, world and action kind. */
        std::array<EditorXrInspectionHand, 2> hands{}; /**< Redacted joint summaries, left then right. */
        std::array<EditorXrInspectionCapability, CapabilityCount> capabilities{};
        std::array<EditorXrInspectionAction, MaximumDisplayedActions> actions{};
    };

    /** @brief Anatomical controller side for a scoped preview request. */
    enum class EditorXrHapticHand : std::uint8_t {
        Left,
        Right
    };

    /** @brief Bounded, backend-neutral vibration intent for the currently assigned Play controller. */
    struct EditorXrHapticIntent final {
        static constexpr auto MaximumDuration = Duration::FromMilliseconds(500);

        EditorXrHapticHand hand;
        float amplitude;   /**< Finite normalized amplitude in [0, 1]. */
        Duration duration; /**< Positive and at most MaximumDuration. */
    };

    /** @brief Generation-safe receipt for a single operator-requested finite vibration preview. */
    struct EditorXrHapticPreviewId final {
        std::uint64_t value{};
        constexpr bool operator==(const EditorXrHapticPreviewId &) const noexcept = default;
    };

    /** @brief Submission evidence from the app owner. Submitted confirms runtime acceptance only. */
    enum class EditorXrHapticPreviewState : std::uint8_t {
        Unknown,
        Queued,
        Submitted,
        Unsupported,
        Rejected,
        Cancelled
    };

    /** @brief Borrowed control-lane preview port. UI submits or cancels intent and never invokes native output. */
    class IEditorXrHapticPreview {
    public:
        virtual ~IEditorXrHapticPreview() = default;
        /** @return Whether the current player and hand have a target for the cold preview request. */
        [[nodiscard]] virtual bool CanPreviewHaptic(EditorXrHapticHand hand) const noexcept = 0;
        /**
         * @brief Queues an exact target and effect for the cold application owner.
         * @param intent Controller side and finite effect validated at admission.
         * @return Exact receipt or a typed invalid, unsupported, unavailable, stale or capacity failure.
         */
        [[nodiscard]] virtual Result<EditorXrHapticPreviewId> QueueHapticPreview(EditorXrHapticIntent intent) = 0;
        /** @return Latest copied evidence for that exact receipt. */
        [[nodiscard]] virtual EditorXrHapticPreviewState ReadHapticPreview(EditorXrHapticPreviewId receipt) const noexcept = 0;
        /** @brief Revokes only the requested receipt. Native stop remains deferred to the app owner. */
        virtual void CancelHapticPreview(EditorXrHapticPreviewId receipt) noexcept = 0;
    };

    /** @brief Redacted operator evidence for one consented gaze preview. */
    enum class EditorXrEyeGazeState : std::uint8_t {
        Unavailable,
        Ready,
        Armed,
        WaitingForSample,
        Tracking
    };

    /** @brief Owner-lane control limited to the next Play session's eye pointing preview. */
    class IEditorXrEyeGazePreview {
    public:
        virtual ~IEditorXrEyeGazePreview() = default;
        /** @return Current redacted state, with no coordinates or history. */
        [[nodiscard]] virtual EditorXrEyeGazeState ReadEyeGazeState() const noexcept = 0;
        /** @return A one-use in-memory grant, or a typed policy, hardware or lifecycle failure. */
        [[nodiscard]] virtual Result<void> ArmEyeGaze() = 0;

        /** @return Whether Setup can offer separate consent for gaze-based rendering. */
        [[nodiscard]] virtual bool CanArmEyeGazeRendering() const noexcept {
            return false;
        }

        /** @return Whether the one-use rendering grant was armed. */
        [[nodiscard]] virtual bool ArmEyeGazeRendering() {
            return false;
        }

        /** @brief Synchronously removes any armed/active grant and discards its latest sample. */
        virtual void RevokeEyeGaze() noexcept = 0;
    };

    /** @brief Redacted state of an explicitly consented spatial plane preview. */
    enum class EditorXrPlaneCaptureState : std::uint8_t {
        Unavailable,
        Ready,
        Armed,
        WaitingForSample,
        Tracking
    };

    /** @brief Grants spatial plane capture to one Play session. Exposes no room geometry to Setup. */
    class IEditorXrPlaneCapturePreview {
    public:
        virtual ~IEditorXrPlaneCapturePreview() = default;
        /** @return Redacted state for the current operator consent. */
        [[nodiscard]] virtual EditorXrPlaneCaptureState ReadPlaneCaptureState() const noexcept = 0;
        /** @return A one-use in-memory grant or a typed policy/lifecycle rejection. */
        [[nodiscard]] virtual Result<void> ArmPlaneCapture() = 0;
        /** @brief Revokes armed or live consent and withdraws geometry immediately. */
        virtual void RevokePlaneCapture() noexcept = 0;
    };

    /** @brief Redacted status of a session-local anchor at the calibrated World origin. */
    enum class EditorXrSpatialAnchorState : std::uint8_t {
        Unavailable,
        Ready,
        Armed,
        Locating,
        Localized
    };

    class IEditorXrSpatialAnchorPreview {
    public:
        virtual ~IEditorXrSpatialAnchorPreview() = default;
        [[nodiscard]] virtual EditorXrSpatialAnchorState ReadSpatialAnchorState() const = 0;
        [[nodiscard]] virtual Result<void> ArmSpatialAnchor() = 0;
        virtual void RevokeSpatialAnchor() noexcept = 0;
    };

    /** @brief Status of the operator's separate local anchor storage grant. */
    enum class EditorXrAnchorStorageState : std::uint8_t {
        Unavailable,
        Ready,
        Armed,
        Granted
    };

    class IEditorXrAnchorStorageConsent {
    public:
        virtual ~IEditorXrAnchorStorageConsent() = default;
        [[nodiscard]] virtual EditorXrAnchorStorageState ReadAnchorStorageState() const = 0;
        [[nodiscard]] virtual Result<void> ArmAnchorStorage() = 0;
        virtual void RevokeAnchorStorage() noexcept = 0;
    };

    /** @brief Redacted operator state for runtime-composited passthrough. */
    enum class EditorXrPassthroughState : std::uint8_t {
        Unavailable,
        Ready,
        Armed,
        Pending,
        Active,
        Denied
    };

    /** @brief Controls only the one-use operator consent in the selected Play owner. */
    class IEditorXrPassthroughPreview {
    public:
        virtual ~IEditorXrPassthroughPreview() = default;
        /** @return Current redacted state without camera inputs or native handles. */
        [[nodiscard]] virtual EditorXrPassthroughState ReadPassthroughPreviewState() const noexcept = 0;
        /** @return Success if one future Play grant was armed, or the original typed refusal. */
        [[nodiscard]] virtual Result<void> ArmPassthrough() = 0;
        /** @brief Stops queued or live consent immediately while the Play owner retires native work. */
        virtual void RevokePassthrough() noexcept = 0;
    };

    /** @brief Borrowed editor setup and optional preview composition. */
    class IEditorXrSetup {
    public:
        virtual ~IEditorXrSetup() = default;

        /** @return Optional scoped vibration preview available from the selected host composition. */
        [[nodiscard]] virtual IEditorXrHapticPreview *HapticPreview() noexcept {
            return nullptr;
        }

        /** @return Optional eye pointing preview protected by an explicit operator grant. */
        [[nodiscard]] virtual IEditorXrEyeGazePreview *EyeGazePreview() noexcept {
            return nullptr;
        }

        /** @return Optional consent control for the selected native plane capture provider. */
        [[nodiscard]] virtual IEditorXrPlaneCapturePreview *PlaneCapturePreview() noexcept {
            return nullptr;
        }

        /** @return Optional consent control for one session-local spatial anchor. */
        [[nodiscard]] virtual IEditorXrSpatialAnchorPreview *SpatialAnchorPreview() noexcept {
            return nullptr;
        }

        /** @return Optional local anchor storage consent on the selected host. */
        [[nodiscard]] virtual IEditorXrAnchorStorageConsent *AnchorStorageConsent() noexcept {
            return nullptr;
        }

        /** @return Optional compositor passthrough consent for the selected platform authority. */
        [[nodiscard]] virtual IEditorXrPassthroughPreview *PassthroughPreview() noexcept {
            return nullptr;
        }

        /** @return Existing fixed-size evidence without provider calls, file access or allocation. */
        [[nodiscard]] virtual EditorXrSetupReport ReadSetup() const noexcept = 0;
        /** @return Owned, bounded, redacted evidence for the current owner, or an explicit unavailable/stale status. */
        [[nodiscard]] virtual EditorXrInspectionSnapshot ReadInspection() const noexcept = 0;
        /** @return Exact queued request or typed rejection. No native work happens on submission. */
        [[nodiscard]] virtual Result<EditorXrSetupTestId> RequestSetupTest() = 0;
        /** @brief Cancel only the specified queued request. Committed observations cannot be revoked by a stale ID. */
        virtual void CancelSetupTest(EditorXrSetupTestId request) noexcept = 0;
    };

    /** @brief Copied actual consumed simulator view in canonical World coordinates; never authored-camera fallback. */
    struct EditorPlayXrPreview final {
        Math::Vec3 position;
        Math::Quaternion orientation{Math::Quaternion::Identity()};
        float verticalFovRadians{1.04719755F};
        float nearPlane{0.01F};
        float farPlane{1000.0F};
    };

    /** @brief Borrowed selected host; it cannot create contexts/actions/players or commit Input.
     * @details All calls are serialized on the workspace/router owner lane. The application keeps this
     * adapter and its verified native parents alive until after screen destruction and native drain.
     * Ordinary desktop omits the adapter. Explicitly selected XR preserves failures without fallback.
     */
    class IEditorPlayXrHost {
    public:
        virtual ~IEditorPlayXrHost() = default;

        /** @return Explicit selected presentation intent; native implementations preserve SelectedXr by default. */
        [[nodiscard]] virtual EditorPlayViewSource ViewSource() const noexcept {
            return EditorPlayViewSource::SelectedXr;
        }

        /** @return Owned actual consumed simulator view, or empty for native mirror/unavailable evidence. */
        [[nodiscard]] virtual std::optional<EditorPlayXrPreview> Preview() const noexcept {
            return {};
        }

        /** @brief Attaches the actual workspace's current context/player-bound collection port.
         * @param input Exact existing InputRouter-issued port, after project profile installation.
         * @param geometry Weak source from the SAME workspace document and mesh owners; no parent lifetime is extended.
         * @return Retained context-bound port or typed owner/duplicate failure; attachment grants no Ready or sampling authority.
         * @post The actual Scene preparation reserves native actions/source against its real origin and candidate.
         * Failed later preparation retains native cleanup debt in the application, never in the workspace token.
         */
        [[nodiscard]] virtual Result<void> AttachInput(const Input::InputActionCollection &input,
                                                       const EditorXrSceneSourcePort &geometry) = 0;
        /** @brief Updates owned presentation values from the actual canonical Play Scene, without retaining its borrow.
         * @param scene Exact active occurrence whose entities were captured by the Scene participant; borrowed only for this call.
         * @details Implementations and overrides take a const reference and copy presentation values into owned storage
         * before returning. They must not retain the view or invoke a callback that can invalidate it during the update.
         * @return Original source/mapping/transform failure; no camera, native call or Input publication occurs.
         */
        [[nodiscard]] virtual Result<void> UpdateScene(const Runtime::RuntimeSceneView &scene) = 0;
        /** @brief Observes the actual existing Play state without inventing another Play lifecycle.
         * @param state Exact canonical controller projection, including Paused/Stopping/Failed.
         * @return Original native/admission outcome; failure closes this adapter's Input producer before returning.
         */
        [[nodiscard]] virtual Result<void> SetPlayState(EditorPlayState state) = 0;
        /** @brief Closes Input admission before the workspace releases/reconfigures its context.
         * @return Actual registration revocation outcome; native frame/completion debt remains application-owned.
         * @post No append uses the detached context, including on failure. No blocking GPU/native retirement occurs.
         */
        [[nodiscard]] virtual Result<void> DetachInput() = 0;
        /** @brief Creates the narrow participant used by the actual Play Scene factory, before the UI participant.
         * @param mounted Explicit private mounted geometry source, absent on the ordinary Scene path.
         * @return Owned canonical Scene observer or null for unsupported composition. It never publishes another Scene.
         * @pre The application host outlives the factory, every returned participant and retained native retirement debt.
         */
        [[nodiscard]] virtual std::unique_ptr<Runtime::SceneActivationParticipant> MakeSceneParticipant(
            EditorMountedXrGeometryCapture mounted = {}) = 0;
        /** @brief Non-allocating logical revocation for failed-enter/destructor recovery.
         * @pre Owner lane. The host outlives the screen; native parents/completion debt stay retained by the host.
         * @post No native callback, wait, clock, Input commit or exception; next canonical cutoff commits neutral state.
         */
        virtual void RevokeInput() noexcept = 0;
    };
}  // namespace Horo::Editor
