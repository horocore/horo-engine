#pragma once
/** @file OpenXRPlayHost.h
 * @brief Concrete app-owned selected XR Play/Input/event composition over the canonical existing owners.
 */
#include "EditorPlayXrHost.h"
#include "EditorXrSceneGeometry.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "OpenXRApplicationSession.h"
#include "OpenXRInputClock.h"
#include "OpenXRSessionEvents.h"
#include "XRSessionHostControl.h"
#include "XRTrackingPublication.h"

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
    /** @brief Read-only projection of existing Scene/debt and canonical session admission, never another lifecycle. */
    enum class OpenXRPlayPresentation {
        Inactive,
        Pending,
        FrameAdmitted
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

    /** @brief Actual workspace Play adapter and application event/input owner, never a second frame scheduler.
     * @details Create consumes the SAME native preparation after selected device creation and before ScreenStart.
     * Workspace attachment only retains its canonical collection port; native action/source preparation occurs
     * against the actual Scene candidate. All operations and final destruction use the creating control lane.
     * Shutdown closes admission first; acquired-image/scene/GUI/native retirement remains owned independently
     * of outgoing Scene/Input borrows until the canonical completion receipts retire it.
     */
    class OpenXRPlayHost final : public Editor::IEditorPlayXrHost,
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
        /** @brief Severs every router/context borrow before the actual App InputRouter is destroyed.
         * @pre Owner lane, outside a dispatch/native callback; Scene producers are already closed.
         * @post Native/image retirement retains only owned typed values/leases, never outgoing Input storage.
         */
        void ReleaseInputOwner() noexcept;
        /** @brief Returns a canonical Scene participant borrowing this exact retained application owner.
         * @return Owned participant for insertion before the existing UI participant, never another Scene publisher.
         */
        [[nodiscard]] std::unique_ptr<Runtime::SceneActivationParticipant> MakeSceneParticipant() override;

    private:
        [[nodiscard]] Result<void> Prepare(XR::XRSessionPreparation stage, const XR::XRSessionId &candidate,
                                           const XR::XRFeaturePlan &plan) override;
        void Release(XR::XRSessionPreparation stage, const XR::XRSessionId &session) noexcept override;
        void Neutralize(const XR::XRSessionId &session, std::uint64_t revision) noexcept override;
        [[nodiscard]] Result<void> AppendInput(const Runtime::FrameContext &context, const Input::InputCollectionFrame &frame);
        [[nodiscard]] Result<void> Lane() const;
        struct FrameState;
        [[nodiscard]] Result<void> ProgressTail();
        [[nodiscard]] Result<void> AbortRenderFrame();
        [[nodiscard]] Result<void> BindRenderFrame(std::uint64_t frameNumber);

        struct SceneState final {
            Runtime::SceneRuntimeId scene;
            std::unique_ptr<Editor::PreparedEditorXrGeometry> geometry;
            std::optional<OpenXRApplicationProjection> projection;
            XR::XRSessionId session;
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

        struct InputState final {
            Input::InputRouter *router{};
            std::optional<Input::InputActionCollection> port;
            std::optional<Editor::EditorXrSceneSourcePort> geometry;
            std::unique_ptr<XR::XRActionBindingCoordinator> coordinator;
            XR::XRWorldOriginRevision origin;
            Editor::EditorPlayState play{Editor::EditorPlayState::Idle};
            bool admitted{};
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
        const std::thread::id lane_{std::this_thread::get_id()};
        bool eventsBound_{};
        bool eventsPending_{};
        bool busy_{};
        bool pendingRevoke_{};
        bool pendingDetach_{};
        bool closing_{};
    };
}  // namespace Horo::Application::Internal
