#pragma once
/** @file EditorPlayXrHost.h
 * @brief Non-installed explicit application XR bridge for the existing workspace input and Play owners.
 */
#include "EditorPlayState.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/InputCollection.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace Horo::Runtime {
    class SceneActivationParticipant;
    class RuntimeSceneView;
}  // namespace Horo::Runtime

namespace Horo::XR {
    enum class XRFeatureProfile : std::uint8_t;
    enum class XRSessionState : std::uint8_t;
    enum class XRTrackingState : std::uint8_t;
    enum class XRCapability : std::uint8_t;
    enum class XRCapabilityState : std::uint8_t;
    enum class XRFeatureDecisionState : std::uint8_t;
    enum class XRTrackedActionKind : std::uint8_t;
    enum class XRTrackedDeviceState : std::uint8_t;
}  // namespace Horo::XR

namespace Horo::Editor {
    class EditorXrSceneSourcePort;

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
        std::uint32_t deviceGeneration{};
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

    /** @brief Owner-lane setup commands, independent of the Play adapter and of GUI or native API types. */
    class IEditorXrSetup {
    public:
        virtual ~IEditorXrSetup() = default;
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
         * @return Owned canonical Scene observer/preparation participant; it never publishes a second Scene.
         * @pre The application host outlives the factory, every returned participant and retained native retirement debt.
         */
        [[nodiscard]] virtual std::unique_ptr<Runtime::SceneActivationParticipant> MakeSceneParticipant() = 0;
        /** @brief Non-allocating logical revocation for failed-enter/destructor recovery.
         * @pre Owner lane. The host outlives the screen; native parents/completion debt stay retained by the host.
         * @post No native callback, wait, clock, Input commit or exception; next canonical cutoff commits neutral state.
         */
        virtual void RevokeInput() noexcept = 0;
    };
}  // namespace Horo::Editor
