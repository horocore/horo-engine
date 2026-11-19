#pragma once
#include "Horo/XR/XRSpatialHitTest.h"
/** @file OpenXRPlayHost.h
 * @brief Concrete app-owned selected XR Play/Input/event composition over the canonical existing owners.
 */
#include "EditorPlayXrHost.h"
#include "EditorXrSceneGeometry.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/XR/XRInteraction.h"
#include "OpenXRApplicationSession.h"
#include "OpenXRInputClock.h"
#include "OpenXRSessionEvents.h"
#include "XRSessionHostControl.h"
#include "XRTrackingPublication.h"

#include <chrono>

namespace Horo::Editor {
    class EditorViewportRendererVulkan;
    struct EditorViewportTextureView;
}  // namespace Horo::Editor

namespace Horo::Render {
    class RenderFrontend;
    class RenderFrameScope;
    struct PrimaryOutputAttachment;
}  // namespace Horo::Render

namespace Horo::Application::Internal {

    /** @brief Copies metadata from one validated tracking snapshot into a redacted inspection report.
     * @param out Report for the same canonical session. @param tracking Exact admitted cutoff.
     * @post The report stores no native handles, action values, or pose and joint coordinates. Requires no provider calls.
     */
    void CopyOpenXRInspectionTracking(Editor::EditorXrInspectionSnapshot &out, const XR::XRTrackingSnapshot &tracking) noexcept;
    /** @brief Read-only projection of existing Scene/debt and canonical session admission, never another lifecycle. */
    enum class OpenXRPlayPresentation {
        Inactive,
        Pending,
        FrameAdmitted
    };

    /** @brief Classification of the exact last projected frame's full-eye producer result. */
    enum class OpenXRPlayCoverageStatus : std::uint8_t {
        Unobserved,
        Incomplete,
        Complete
    };

    /** @brief Same startup authorities, retained until native frame/image debt is actually drained.
     * @details This composition is selected only for explicit XR+Vulkan. The ordinary desktop has no host.
     * The same RuntimeHost source/fixedStep and same canonical InputRouter are mandatory; no clock, tick,
     * session candidate, source registration or completion authority is issued by this value.
     */
    struct OpenXRPlayHostComposition final {
        OpenXRApplicationSession &session;
        Input::InputRouter &router;
        Editor::EditorViewportRendererVulkan &viewport;
        Render::RenderFrontend &frontend;
        std::shared_ptr<const OpenXRApplicationBindings> bindings;
        Runtime::RuntimeDispatchSource dispatch;
        Duration fixedStep;
        Runtime::SceneObjectId trackingRoot;
    };

    /** @brief Fully tracked native reference relation for one exact active presentation frame. */
    struct OpenXRTrackedReference final {
        XR::XRFrameId frame;
        XR::XRPoseSample referenceToWorld;
    };

    /** @brief Committed XR interaction scope of the exact playing Scene and assigned local player. */
    struct OpenXRPlayInteractionScope final {
        XR::XRInteractionCutoff cutoff;
        Runtime::SceneRuntimeId scene;
        Input::PlayerId player{};

        bool operator==(const OpenXRPlayInteractionScope &) const noexcept = default;
    };

    /** @brief Current controller/hand source identities for a synchronous query, without raw tracking. */
    class OpenXRPlayInteractionSources final {
    public:
        /** @return Exact Scene/player/tracking fences to pass back for geometry. */
        [[nodiscard]] const OpenXRPlayInteractionScope &Scope() const noexcept {
            return scope_;
        }

        /** @return Tracked controller/hand records in the canonical snapshot's order. */
        [[nodiscard]] std::span<const XR::XRTrackedDeviceRecord> Devices() const noexcept {
            return std::span{devices_}.first(count_);
        }

    private:
        friend class OpenXRPlayHost;
        OpenXRPlayInteractionScope scope_;
        std::array<XR::XRTrackedDeviceRecord, XR::XRTrackingSnapshotHardLimits::MaximumDevices> devices_{};
        std::size_t count_{};
    };

    /** @brief Actual workspace Play adapter and application event/input owner, never a second frame scheduler.
     * @details Create consumes the SAME native preparation after selected device creation and before ScreenStart.
     * Workspace attachment only retains its canonical collection port; native action/source preparation occurs
     * against the actual Scene candidate. All operations and final destruction use the creating control lane.
     * Shutdown closes admission first; acquired-image/scene/GUI/native retirement remains owned independently
     * of outgoing Scene/Input borrows until the canonical completion receipts retire it.
     */
    class OpenXRPlayHost final : public Editor::IEditorPlayXrHost,
                                 public Editor::IEditorXrSetup,
                                 public Editor::IEditorXrHapticPreview,
                                 public Editor::IEditorXrPlaneCapturePreview,
                                 public Editor::IEditorXrEyeGazePreview,
                                 public Editor::IEditorXrPassthroughPreview,
                                 private XR::RuntimeInternal::IXRSessionRetirementResources,
                                 private XR::IXRBindingNeutralizer {
    public:
        /** @brief Factory-only authority for adopting an already prepared native session. */
        class ConstructionKey final {
            ConstructionKey() = default;
            friend class OpenXRPlayHost;
        };

        /** @brief Adopts the existing composition after its native preparation succeeded. */
        OpenXRPlayHost(const ConstructionKey &key, OpenXRPlayHostComposition composition, OpenXRApplicationPreparation preparation);

        /** @brief Composes actual existing native and scheduler parents without publishing Ready.
         * @param composition Exact selected application owners; all borrowed parents outlive the host.
         * @return Concrete host or preserved native/declaration/currentness failure; no fallback renderer is selected.
         */
        [[nodiscard]] static Result<std::unique_ptr<OpenXRPlayHost>> Create(OpenXRPlayHostComposition composition);
        ~OpenXRPlayHost() override;
        /** @brief Closes producers and makes one finite physical retirement poll in the App's existing recovery owner.
         * @return True only after actual frame/image/GUI/native retirement; false/error retains every parent for retry.
         * @pre Owner lane outside callbacks. No normal-frame wait or guessed completion; explicit App teardown
         * retains this owner and the selected device/loader until success. Repeated calls preserve original debt.
         * A retained terminal compositor Begin/End fault revokes the SAME session to Lost before bounded cleanup;
         * closing still rejects new work. Only actual native destruction permits the quiescent frame reset.
         */
        [[nodiscard]] Result<bool> Close();
        [[nodiscard]] Editor::EditorXrSetupReport ReadSetup() const noexcept override;
        [[nodiscard]] Editor::EditorXrInspectionSnapshot ReadInspection() const noexcept override;
        [[nodiscard]] Result<Editor::EditorXrSetupTestId> RequestSetupTest() override;
        void CancelSetupTest(Editor::EditorXrSetupTestId request) noexcept override;
        /** @brief Performs one queued cold setup test outside RuntimeHost frames. */
        void PumpSetupTest();

        [[nodiscard]] Editor::IEditorXrHapticPreview *HapticPreview() noexcept override {
            return this;
        }

        [[nodiscard]] bool CanPreviewHaptic(Editor::EditorXrHapticHand hand) const noexcept override;
        [[nodiscard]] Result<Editor::EditorXrHapticPreviewId> QueueHapticPreview(Editor::EditorXrHapticIntent intent) override;
        [[nodiscard]] Editor::EditorXrHapticPreviewState ReadHapticPreview(Editor::EditorXrHapticPreviewId receipt) const noexcept override;
        void CancelHapticPreview(Editor::EditorXrHapticPreviewId receipt) noexcept override;
        /** @brief Runs one admitted preview between RuntimeHost frames and progresses its scoped cancellation. */
        void PumpHapticPreview();

        [[nodiscard]] Editor::IEditorXrPlaneCapturePreview *PlaneCapturePreview() noexcept override {
            return this;
        }

        [[nodiscard]] Editor::EditorXrPlaneCaptureState ReadPlaneCaptureState() const noexcept override;
        [[nodiscard]] Result<void> ArmPlaneCapture() override;
        void RevokePlaneCapture() noexcept override;

        [[nodiscard]] Editor::IEditorXrEyeGazePreview *EyeGazePreview() noexcept override {
            return this;
        }

        [[nodiscard]] Editor::EditorXrEyeGazeState ReadEyeGazeState() const noexcept override;
        [[nodiscard]] Result<void> ArmEyeGaze() override;
        void RevokeEyeGaze() noexcept override;
        /** @return Only the exact current Play pointing sample, gated by the focused cutoff and consent. */
        [[nodiscard]] std::optional<XR::XRPoseSample> ReadEyeGaze() const noexcept;
        /** @return Live authorized geometry for the current calibrated Play cutoff or typed unavailability. */
        [[nodiscard]] Result<XR::XRPlaneSnapshot> ReadPlaneObservations() const;
        /** @brief Intersects a ray with the current consented World planes.
         * @param request Exact capture scope, age limit and
         * finite ray.
         * @return Ordered placement candidates without mutating authored or runtime Scene objects. */
        [[nodiscard]] Result<XR::XRSpatialHits> QueryPlaneHits(const XR::XRSpatialHitRequest &request) const;

        [[nodiscard]] Editor::IEditorXrPassthroughPreview *PassthroughPreview() noexcept override {
            return this;
        }

        [[nodiscard]] Editor::EditorXrPassthroughState ReadPassthroughPreviewState() const noexcept override;
        [[nodiscard]] Result<void> ArmPassthrough() override;
        void RevokePassthrough() noexcept override;
        /** @brief Reads redacted current owner-lane availability; exposes no native handle or camera data. */
        [[nodiscard]] XR::XRCapabilityState ReadPassthroughState() const noexcept;
        /** @brief Supplies bounded, current runtime prerequisites to the separate device qualification runner. */
        [[nodiscard]] Result<OpenXRPackagedPreflight> ReadDeviceQualificationPreflight() const;
        [[nodiscard]] Result<void> AttachInput(const Input::InputActionCollection &input,
                                               const Editor::EditorXrSceneSourcePort &geometry) override;
        [[nodiscard]] Result<void> UpdateScene(const Runtime::RuntimeSceneView &scene) override;
        [[nodiscard]] Result<void> SetPlayState(Editor::EditorPlayState state) override;
        [[nodiscard]] Result<void> DetachInput() override;
        void RevokeInput() noexcept override;
        /** @brief Observes only the exact published selected-XR Scene; no readiness or camera fallback is implied. */
        [[nodiscard]] bool UsesXrScene() const noexcept;
        /** @brief Projects selected Scene/retirement ownership and canonical frame admission on the owner lane.
         * @return Inactive only after all owned Scene/render state is absent; Pending while preparing/retiring
         * or native admission is closed; FrameAdmitted only from the same canonical session snapshot
         * and idle frame gate. Pending GPU tail debt uses the existing desktop event-loop cap, never a GPU wait.
         * @details Pending forbids desktop-camera fallback. This query never issues a frame or native readiness.
         */
        [[nodiscard]] OpenXRPlayPresentation PresentationState() const noexcept;
        /** @brief Reads the exact just-ended frame's owned coverage status, if available before the next prediction.
         * @param frame Canonical native frame that was projected.
         * @return Unobserved for stale identities, unsupported paths or absent completion. Does not admit a compositor provider.
         */
        [[nodiscard]] OpenXRPlayCoverageStatus CoverageState(const XR::XRFrameId &frame) const noexcept;
        /** @brief Prepares one actual native prediction/acquisition and binds cold graph storage before the SAME App scope.
         * @param frameNumber SAME RuntimeHost frame number, never a second counter.
         * @return True for a complete bound acquisition, false while actual retirement/wait is pending; original errors persist.
         */
        [[nodiscard]] Result<bool> PrepareRenderFrame(std::uint64_t frameNumber);
        /** @brief Executes the prepared actual acquired-eye graph through the SAME App frame. */
        [[nodiscard]] Result<void> ExecuteRenderFrame(Render::RenderFrameScope &frame, const Render::PrimaryOutputAttachment &output);
        /** @brief Records actual GUI encoding acceptance or cancels this exact prepared frame before scene submission.
         * @param accepted Exact ComposeGui success, or false on GUI/native-Begin/scene-encoding failure. True requires
         * actual scene submission; false also closes a prepared acquisition which could not enter the App scope.
         * @return Canonical frame transition result; release remains deferred. Idle/duplicate notification rejects.
         * @post Cancellation never becomes a successful projection layer or physical completion receipt. The caller
         * preserves its original error and continues the SAME owned tail even if this logical abort is fenced by loss.
         */
        [[nodiscard]] Result<void> FinishGui(bool accepted);
        /** @brief Borrows the exact submitted acquisition's GUI mirror for this canonical App frame.
         * @param frameNumber Same RuntimeHost frame supplied to PrepareRenderFrame; never a new counter.
         * @return Full typed GUI binding or empty for wrong lane, stale frame, loss, cancellation or retirement.
         * @pre Owner lane outside callbacks, after ExecuteRenderFrame and before FinishGui. The App must
         * reject an empty selected-XR binding; it must not substitute the desktop camera. This value is
         * synchronous encoding input only: native retention is owned by the same scene/GUI queue leases.
         */
        [[nodiscard]] Editor::EditorViewportTextureView MirrorView(std::uint64_t frameNumber) const noexcept;
        /** @brief Polls scene+GUI completion and exact restored native state before release and actual xrEndFrame.
         * @return Original retirement/compositor failure, retaining the same frame/image debt. Never waits on the GPU.
         */
        [[nodiscard]] Result<void> ProgressRenderTail();
        /** @brief Polls the actual selected native instance from the App's existing platform-event phase.
         * @return Preserved native error, after closing Input/render admission on lost/inconsistent evidence.
         * @post Native end is deferred while the SAME canonical frame/image obligations remain outstanding.
         */
        [[nodiscard]] Result<void> PollEvents();
        /** @brief Performs the ONE actual App input cutoff, then lets the existing workspace route that snapshot.
         * @param context Opaque SAME scheduler callback evidence. @param raw SAME SDL backend committed raw snapshot.
         * @return Success or original XR/Input error. Missing/failed XR still commits neutral semantic contribution
         * with physical input intact; a failed transaction commit never publishes a partial cutoff.
         */
        [[nodiscard]] Result<void> BuildInputSnapshot(const Runtime::FrameContext &context, const Input::RawInputSnapshot &raw);
        /** @brief Reads gesture candidates from the current focused cutoff. @return Bounded candidates or empty after loss. */
        [[nodiscard]] XR::XRHandGestureCandidates ReadHandGestures() const noexcept;
        /** @brief Reads the selected Play host's current interaction sources on its owner lane.
         * @param player Exact local player using the existing Play Input assignment.
         * @return Copied sources or a typed unavailable, wrong-owner, or stale failure.
         */
        [[nodiscard]] Result<OpenXRPlayInteractionSources> ReadInteractionSources(Input::PlayerId player) const;
        /** @brief Converts the same committed XR input cutoff into geometry for a synchronous consumer.
         * @param scope Exact scope from ReadInteractionSources.
         * @param probes Consumer-supplied shapes and live controller/hand devices.
         * @return Bounded geometry or typed failure, with no interaction target or semantic action authority.
         */
        [[nodiscard]] Result<XR::XRInteractionCandidates> ReadInteractionCandidates(const OpenXRPlayInteractionScope &scope,
                                                                                    std::span<const XR::XRInteractionProbe> probes) const;
        /** @brief Copies current reference identity and bounds from the selected Play session.
         * @param session Exact published session.
         * @return Immutable-by-copy evidence or typed stale/unavailable error.
         */
        [[nodiscard]] Result<XR::XRReferenceSpaceSnapshot> ReadReferences(XR::XRSessionId session) const;
        /** @brief Reads the native reference-to-created-World relation already located for a bound frame.
         * @param session Exact published session and runtime incarnation.
         * @param hostFrameNumber Current owner frame issued by the renderer caller.
         * @return Fully tracked relation or typed pose, stale frame or unavailable failure.
         * @details No native call or new tracking sample. The value is evidence for this frame only.
         * It describes the runtime's already-created World space. Canonical mounted origin correspondence requires separate admission.
         */
        [[nodiscard]] Result<OpenXRTrackedReference> ReadTrackedReference(XR::XRSessionId session, std::uint64_t hostFrameNumber) const;
        /** @brief Copies the last reference change which triggered retirement of the specified session.
         * @param session Exact old owner of the notification.
         * @return Historical value until a newer Play publication replaces it; never grants space access.
         */
        [[nodiscard]] std::optional<XR::XRReferenceSpaceChange> ReadReferenceChange(XR::XRSessionId session) const noexcept;
        /** @brief Severs every router/context borrow before the actual App InputRouter is destroyed.
         * @pre Owner lane, outside a dispatch/native callback; Scene producers are already closed.
         * @post Native/image retirement retains only owned typed values/leases, never outgoing Input storage.
         */
        void ReleaseInputOwner() noexcept;
        /** @brief Returns a canonical Scene participant borrowing this exact retained application owner.
         * @return Owned participant for insertion before the existing UI participant, never another Scene publisher.
         */
        [[nodiscard]] std::unique_ptr<Runtime::SceneActivationParticipant> MakeSceneParticipant(
            Editor::EditorMountedXrGeometryCapture mounted = {}) override;

    private:
        [[nodiscard]] Result<void> Prepare(XR::XRSessionPreparation stage, const XR::XRSessionId &candidate,
                                           const XR::XRFeaturePlan &plan) override;
        void Release(XR::XRSessionPreparation stage, const XR::XRSessionId &session) noexcept override;
        void Neutralize(const XR::XRSessionId &session, std::uint64_t revision) noexcept override;
        [[nodiscard]] Result<void> AppendInput(const Runtime::FrameContext &context, const Input::InputCollectionFrame &frame);
        [[nodiscard]] Result<void> Lane() const;
        /** @brief Borrows a current committed interaction cutoff for this synchronous owner-lane read only. */
        [[nodiscard]] Result<const XR::XRTrackingSnapshot *> CurrentInteractionTracking(Input::PlayerId player) const;
        /** @brief Checks current owner lane and absence of Play work before a private session test. */
        [[nodiscard]] bool SetupIdle() const noexcept;
        /** @brief Captures a bounded event result and revokes the exact Scene/Input authority when it cannot continue.
         * @param observed Exact batch of normalized events from this native owner.
         * @return True when the ordinary frame tail can continue, false on orderly retirement or pending events,
         * or the original profile/admission failure when a changed input plan must retire.
         */
        [[nodiscard]] Result<bool> ApplyEventObservation(const XR::OpenXRInternal::NativeSessionEventObservation &observed);
        /** @brief Fences replacement against the old session's latest effective reference-change time.
         * @return Success when the exact retained runtime time passed the barrier, or typed pending/currentness failure.
         */
        [[nodiscard]] Result<void> AdmitReferenceReplacement() const;
        struct FrameState;
        /** @brief Drives one cold activation/retirement step on the actual native event cutoff. */
        void ProgressPassthrough();
        /** @brief Checks the exact local grant, Play and World ownership without calling platform authority.
         * @param scope Frozen session, World and operator grant. @return Whether all three are still current.
         */
        [[nodiscard]] bool MatchesPassthroughGrant(const XR::XRPassthroughScope &scope) const noexcept;
        /** @brief Checks current consent, platform authority, focus, calibration and native activation.
         * @param scope Owned scope copy that remains valid across the external permission check.
         * @return Whether the exact native and Play scope is still authorized.
         */
        [[nodiscard]] bool HasPassthrough(XR::XRPassthroughScope scope) const noexcept;
        [[nodiscard]] Result<void> ProgressTail();
        [[nodiscard]] Result<void> AbortRenderFrame();
        [[nodiscard]] Result<void> BindRenderFrame(std::uint64_t frameNumber);

        /** @brief Holds one selected geometry authority and never switches basis after preparation. */
        struct SceneGeometry final {
            std::unique_ptr<Editor::PreparedEditorXrGeometry> authored;
            std::shared_ptr<const Render::RenderOriginLocalScene> mounted;
            Runtime::RuntimeSceneView mountedScene; /**< Captured structural occurrence, checked while borrowing mounted geometry. */
            bool revoked{};

            /** @brief Checks the selected source and its retained structural and origin lifetimes. */
            [[nodiscard]] Result<void> ValidateSource() const;
            /** @brief Updates authored draws or checks the exact immutable mounted Scene occurrence. */
            [[nodiscard]] Result<void> Update(const Runtime::RuntimeSceneView &scene);
            /** @brief Borrows leased mesh resources during cold renderer preparation. */
            [[nodiscard]] std::span<const Render::RenderMeshResourceView> Resources() const noexcept;
            /** @brief Borrows geometry in the selected source's admitted coordinate basis. */
            [[nodiscard]] std::span<const Render::RenderStaticMeshInstance> Instances() const noexcept;
            /** @brief Borrows only issued canonical per-draw motion identities. */
            [[nodiscard]] std::span<const Render::RenderMotionObjectId> MotionObjects() const noexcept;
            /** @brief Closes admission without dropping resources retained for native retirement. */
            void Revoke() noexcept;
        };

        struct SceneState final {
            Runtime::SceneRuntimeId scene;
            std::optional<SceneGeometry> geometry;
            std::optional<OpenXRApplicationProjection> projection;
            XR::XRReferenceSpaceSnapshot references; /**< Same cold native preparation as this Scene candidate. */
            XR::XRCoordinateSpace worldReference;    /**< Exact calibrated native World child from the same candidate. */
            std::optional<XR::OpenXRInternal::NativeReferenceCalibrationObservation> mountedCalibration;
            XR::XRSessionId session;
            std::optional<XR::XRPassthroughGrant> passthroughGrant;
            CancellationSource preparationCancellation; /**< Exact unpublished attempt; revocation cannot affect a replacement. */
            bool activated{};
            bool published{};
            bool retiring{};
            bool nativeRetired{}; /**< Exact candidate's successful physical destruction, retained across bookkeeping retry. */
            /** @brief Prepares native/Input/render children for this exact state under the canonical Scene rollback scope.
             * @param host Same retained application owner. @param tracking Actual calibrated tracking-root request.
             * @return Original native/resource/activation error; no Scene publication occurs here.
             */
            [[nodiscard]] Result<void> PrepareResources(OpenXRPlayHost &host,
                                                        const XR::OpenXRInternal::NativeTrackingSpaceRequest &tracking);
            /** @brief Seals mounted placement only after the exact native relation and origin lease agree. */
            [[nodiscard]] Result<void> CalibrateMounted(const OpenXRPlayHost &host, const Math::Transform &placement);
            /** @brief Revalidates this exact current state without issuing a replacement identity.
             * @param host Same retained owner. @return Source/native/currentness failure or success.
             */
            [[nodiscard]] Result<void> Validate(const OpenXRPlayHost &host) const;
            /** @brief Revokes this state only; native/GUI debt remains independently retained for retry.
             * @param host Same creating owner; owner lane required. Late retirement cannot revoke a replacement.
             */
            void Retire(OpenXRPlayHost &host) noexcept;
            /** @brief Polls the same current state's physical debt; absence is already retired.
             * @param host Same creating owner. @return False while debt remains, original error, or complete retirement.
             */
            [[nodiscard]] static Result<bool> ProgressRetirement(OpenXRPlayHost &host);
        };
        class SceneCandidate;
        class SceneParticipant;

        /** @brief Drops logical frame/event debt only after this candidate's native session was actually destroyed.
         * @param candidate Exact retained Scene candidate whose nativeRetired receipt was committed on success.
         * @return Original bookkeeping error; failure retains the receipt and remaining owners for retry.
         * @pre Every submitted scene/GUI reference and native swapchain/view has retired. Lost recovery does not
         * claim a successful Abort/Release/End: physical session destruction is its quiescence proof instead.
         */
        [[nodiscard]] Result<void> FinishFrameRetirement(const XR::XRSessionId &candidate);

        /** @brief Checks whether the exact optional provider policy needs the current projection's history. */
        [[nodiscard]] bool WantsMotionHistory(const XR::XRFeaturePlan &plan) const noexcept;

        /** @brief Creates the SAME native color owner/views after the canonical candidate activates that session.
         * @param candidate Actual issued candidate. @param color Exact previously observed sampled UNORM tuple.
         * @return Original native/device error; any partial cleanup debt remains owned in render_.
         */
        [[nodiscard]] Result<void> PrepareFrameResources(const XR::XRSessionId &candidate, const Render::RenderTextureDescriptor &color);
        /** @brief Binds actual event/compositor/clock adapters after canonical Ready but before Scene publication.
         * @param candidate Exact newly activated owner. @param capabilities Same complete immutable preparation facts.
         * @return Original bind failure; admission remains private until every stage and Scene validation succeeds.
         */
        [[nodiscard]] Result<void> FinishFrameResources(const XR::XRSessionId &candidate, const XR::XRCapabilitySnapshot &capabilities);
        /** @brief Observes only actual completed native/renderer cleanup of the exact retained candidate. */
        [[nodiscard]] Result<bool> RetirementComplete(const XR::XRSessionId &session) const override;
        /** @brief Revokes both canonical frame admission and Input after unusable native event evidence.
         * @pre Serialized owner operation; no native destruction or completion is inferred.
         * @post Native/frame/image debt remains owned for the explicit retirement path.
         */
        void RevokeEventAdmission() noexcept;
        /** @brief Retires the selected session only for a confirmed input profile transition.
         * @param error Exact failure from native sampling or explicit profile observation.
         * @return Whether the existing Play retirement path accepted the profile change.
         */
        [[nodiscard]] bool RetireIfInputProfileChanged(const Error &error);
        /** @brief Neutralizes the XR source while keeping the SAME lifetime-fenced workspace collection for next Play.
         * @post No native frame/source can publish until a new actual preparation/binding succeeds; replacement still
         * requires the router's real neutral commit. Explicit workspace RevokeInput/DetachInput also drop that borrow.
         */
        void RevokeSource() noexcept;
        /** @brief Ends one serialized foreign-call opportunity and performs deferred logical source revocation. */
        void FinishOperation() noexcept;

        struct Operation final {
            Operation(const Operation &) = delete;
            Operation &operator=(const Operation &) = delete;

            OpenXRPlayHost &host;

            explicit Operation(OpenXRPlayHost &owner) noexcept : host(owner) {
                host.busy_ = true;
            }

            ~Operation() {
                host.FinishOperation();
            }
        };

        /** @brief Complete app-scoped target. No native handles or raw tracking samples are retained. */
        struct HapticTarget final {
            XR::XRSessionId session;
            XR::XRDeviceId device;
            std::uint64_t bindings;
            Input::PlayerId player;
            XR::XRTrackedDeviceRole role;
            bool operator==(const HapticTarget &) const = default;
        };

        /** @brief One pending or submitted operator preview with finite local admission duration. */
        struct HapticPreviewRecord final {
            Editor::EditorXrHapticPreviewId receipt;
            HapticTarget target;
            Editor::EditorXrHapticIntent intent;
            std::chrono::steady_clock::time_point until;
            Editor::EditorXrHapticPreviewState state{Editor::EditorXrHapticPreviewState::Queued};
            bool cancelRequested{};
            bool mayNeedStop{};
        };

        /** @brief Copies current player, controller and binding identity when the preview scope is available. */
        [[nodiscard]] std::optional<HapticTarget> CaptureHapticTarget(Editor::EditorXrHapticHand hand) const noexcept;
        /** @brief Reports whether the signed binding plan declares an output for this exact controller role. */
        [[nodiscard]] bool HasHapticRoute(XR::XRTrackedDeviceRole role) const noexcept;
        /** @brief Verifies the borrowed assigned Play Input context without refreshing or mutating it. */
        [[nodiscard]] bool HasCurrentAssignedInput() const;
        std::optional<HapticPreviewRecord> hapticPreview_;
        std::uint64_t nextHapticPreview_{};
        bool planesArmed_{}; /**< One-use Setup consent, consumed by an actual Play preparation. */
        bool gazeArmed_{};   /**< In-memory grant consumed by one actual Play preparation. */
        std::uint64_t nextPassthroughGrant_{};
        std::optional<XR::XRPassthroughGrant> passthroughArmed_;
        std::optional<XR::XRPassthroughScope> passthroughScope_; /**< Retained until the native owner confirms retirement. */
        XR::XRCapabilityState passthroughState_{XR::XRCapabilityState::Unavailable};

        struct InputState final {
            Input::InputRouter *router{};
            std::optional<Input::InputActionCollection> port;
            std::optional<Editor::EditorXrSceneSourcePort> geometry;
            std::unique_ptr<XR::XRActionBindingCoordinator> coordinator;
            XR::XRWorldOriginRevision origin;
            Editor::EditorPlayState play{Editor::EditorPlayState::Idle};
            bool admitted{};
            bool neutralPending{}; /**< An observed focus edge requires an empty committed cutoff before resampling. */
        };

        struct Parents final {
            OpenXRApplicationSession &session;
            Editor::EditorViewportRendererVulkan &viewport;
            Render::RenderFrontend &frontend;
            std::shared_ptr<const OpenXRApplicationBindings> bindings;
            Runtime::RuntimeDispatchSource dispatch;
            Duration fixedStep;
            Runtime::SceneObjectId trackingRoot;
        };

        Parents composition_;

        XR::OpenXRInternal::OpenXRNativeSession &native_;
        const XR::XRLoaderPreflightAttempt discoveryAttempt_;
        PFN_xrGetInstanceProcAddr getProc_;
        XR::XRSessionLifecycle sessions_{*this};
        XR::XRFrameLifecycle frames_{sessions_};
        XR::RuntimeInternal::XRTrackingPublication tracking_{sessions_};
        OpenXRInputClock clock_;
        std::optional<XR::OpenXRInternal::OpenXRSessionEvents> events_;
        std::shared_ptr<FrameState> render_;
        XR::XRSessionId retired_;              /**< Receipt assigned only after successful physical candidate retirement. */
        Runtime::SceneRuntimeId retiredScene_; /**< Exact already-drained Scene; late canonical retirement is an idempotent no-op. */
        InputState input_;
        std::shared_ptr<SceneState> scene_;
        Editor::EditorXrSetupReport setup_;
        std::optional<OpenXRPackagedPreflight> setupPreflight_;
        Editor::EditorXrSetupTestId queuedSetupTest_;
        std::uint64_t lastSetupTest_{};
        const std::thread::id lane_{std::this_thread::get_id()};
        std::optional<XR::XRReferenceSpaceChange> lastReferenceChange_;
        bool eventsBound_{};
        bool eventsPending_{};
        bool busy_{};
        bool pendingRevoke_{};
        bool pendingDetach_{};
        bool closing_{};
        bool setupCleanupPending_{};
        bool instanceLost_{}; /**< The startup-selected graphics device cannot be rebound to a different instance in place. */
    };
}  // namespace Horo::Application::Internal
