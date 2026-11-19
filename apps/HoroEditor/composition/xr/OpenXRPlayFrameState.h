/** @file OpenXRPlayFrameState.h
 * @brief Private candidate-owned native image and frame debt for the selected application host.
 */
#pragma once

#include "EditorVulkanXrViewportAccess.h"
#include "Horo/Runtime/Render/MultiviewMotionHistory.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/XR/XRMultiviewScene.h"
#include "OpenXRFrameCompositor.h"
#include "OpenXRPlayHost.h"
#include "OpenXRVulkanSwapchain.h"
#include "VulkanEditorGraphicsBridge.h"

namespace Horo::Application::Internal {
    /** @brief Candidate-owned physical image/compositor children of the SAME selected native and render owners.
     * @details Native failures keep this storage retained by the App. Scene Shutdown may revoke its typed
     * publication, but cannot drop these children, outgoing native dependencies or queued image leases.
     * No Scene/component/Input borrow is retained here; all such handoffs are owned typed values.
     */
    struct OpenXRPlayHost::FrameState final : XR::OpenXRInternal::IOpenXRCompositionSpace {
        OpenXRPlayHost &host;
        const XR::XRSessionId session;
        XR::OpenXRInternal::OpenXRVulkanSwapchainImages images;
        XR::OpenXRInternal::OpenXRSwapchain color;
        XR::OpenXRInternal::OpenXRFrameCompositor compositor;

        /** @brief Native writer-only acquisitions retire through the canonical scene/GUI queue lease. */
        struct WriterPair final {
            struct Target final {
                XR::OpenXRInternal::OpenXRVulkanSwapchainImages images;
                XR::OpenXRInternal::OpenXRSwapchain owner;
                std::optional<XR::OpenXRInternal::NativeSwapchainAcquisition> acquisition;
                std::array<Render::RenderTextureViewHandle, 2> views{};

                Target(OpenXRPlayHost &host, XR::OpenXRInternal::OpenXRSelectedVulkanBinding &graphics,
                       const XrGraphicsBindingVulkanKHR &binding, Render::RenderQueueId queue,
                       Render::Detail::VulkanExternalViewDispatch dispatch);
            };

            Target motion;
            Target depth;
            XR::XRFrameId frame;
            std::optional<Editor::EditorVulkanXrViewportAccess::AuxiliaryImages> ready;
            bool available{};

            WriterPair(OpenXRPlayHost &host, XR::OpenXRInternal::OpenXRSelectedVulkanBinding &graphics,
                       const XrGraphicsBindingVulkanKHR &binding, const Render::Detail::VulkanEditorDevice &device,
                       Render::Detail::VulkanExternalViewDispatch views);

            /** @brief Creates the exact session's auxiliary swapchains without registering a compositor provider. */
            Result<void> Prepare(const XR::XRSessionId &session, const OpenXRProjectionWriter &plan, PFN_xrGetInstanceProcAddr getProc);
            /** @brief Acquires both images at zero timeout; an incomplete pair retains exact retirement debt. */
            Result<std::optional<Editor::EditorVulkanXrViewportAccess::AuxiliaryImages>> Acquire(Render::RenderFrontend &frontend,
                                                                                                 const XR::XRFrameId &candidate,
                                                                                                 const OpenXRProjectionWriter &plan);
            /** @brief Retires all touched auxiliary images before the ordinary projection may finish. */
            Result<bool> Retire(Render::RenderFrontend &frontend);
            /** @brief Retires the candidate's native children, including incomplete cold prefixes. */
            Result<bool> Close(Render::RenderFrontend &frontend, const XR::XRSessionId &session, bool lost);
        };

        std::optional<WriterPair> writer;
        Render::RenderTextureDescriptor allocation;
        std::optional<XR::OpenXRInternal::NativeSwapchainAcquisition> acquisition;
        std::array<Render::RenderTextureViewHandle, 2> eyeViews{};
        std::optional<XR::XRViewRenderPlan> plan;

        /** @brief Exact renderer alpha intent and installed World mapping for the bound native prediction. */
        struct PassthroughFrame final {
            XR::XRPassthroughScope scope;
            Render::MultiviewPublication publication;
            XR::XRPoseSample referenceToWorld;
        };

        std::optional<PassthroughFrame> passthrough;
        std::optional<Render::MultiviewMotionFrame> motionFrame;
        std::optional<XR::OpenXRInternal::NativePresentationViews> views;
        std::uint64_t hostFrameNumber{}; /**< Exact successful bind receipt, not another frame issuer. */
        bool guiFinished{};
        bool submitted{};
        bool acquisitionRecorded{};
        bool rendered{};
        bool clockBound{};

        FrameState(OpenXRPlayHost &owner, const XR::XRSessionId &candidate, XR::OpenXRInternal::OpenXRSelectedVulkanBinding &graphics,
                   const XrGraphicsBindingVulkanKHR &binding, const Render::Detail::VulkanEditorDevice &device,
                   Render::Detail::VulkanExternalViewDispatch viewDispatch, const Render::RenderTextureDescriptor &descriptor)
            : host(owner), session(candidate),
              images(owner.composition_.frontend, graphics, binding, device.graphicsIdentity, viewDispatch), color(owner.native_, images),
              compositor(owner.native_, owner.frames_, *this), allocation(descriptor) {}

        /** @brief Preserves the original frame failure while retaining any failed canonical abort debt. */
        Result<bool> Fail(const Error &error) {
            const auto aborted = host.AbortRenderFrame();
            (void)aborted;
            return Result<bool>::Failure(error);
        }

        /** @brief Starts the sole native prediction and closes non-renderable waits with the same zero-layer protocol. */
        Result<bool> StartPrediction() {
            using Output = Result<bool>;
            auto state = host.frames_.Snapshot();
            if (writer && writer->frame.IsValid())
                return Output::Failure(MakeError(XR::XRErrors::OperationIncompatible));
            if (!host.sessions_.Snapshot().admitsFrames)
                return Output::Success(false);  // Real native Ready/Running events, never Scene publication, admit waits.
            rendered = false;
            submitted = false;
            acquisitionRecorded = false;
            guiFinished = false;
            views.reset();
            plan.reset();
            passthrough.reset();
            motionFrame.reset();
            const auto waited = compositor.Wait();
            if (waited.HasError())
                return Output::Failure(waited.ErrorValue());
            if (!waited.Value().frame.IsValid())
                return Output::Failure(MakeError(XR::XRErrors::OperationUnavailable));
            if (auto begun = compositor.Begin(waited.Value().frame); begun.HasError())
                return Output::Failure(begun.ErrorValue());
            state = host.frames_.Snapshot();
            if (waited.Value().status != XR::XRFrameStatus::Ok || !state.renderAdmitted) {
                guiFinished = true;
                if (state.renderAdmitted) {
                    if (auto aborted = host.AbortRenderFrame(); aborted.HasError())
                        return Output::Failure(aborted.ErrorValue());
                }
                if (auto ended = compositor.EndEmpty(state.frame); ended.HasError())
                    return Output::Failure(ended.ErrorValue());
                CompleteMotion(false);
                return Output::Success(false);
            }
            return Output::Success(true);
        }

        /** @brief Observes the actual converted deadline and retires missed predictions without another camera or wait. */
        Result<bool> CheckDeadline() {
            using Output = Result<bool>;
            const auto state = host.frames_.Snapshot();
            const auto now = host.clock_.PresentationNow();
            if (now.HasError())
                return Fail(now.ErrorValue());
            const auto pacing = compositor.ObservePacing(state.frame, now.Value());
            if (pacing.HasError())
                return Fail(pacing.ErrorValue());
            if (pacing.Value().deadline != XR::OpenXRInternal::NativeFrameDeadline::WithinBudget) {
                if (auto aborted = host.AbortRenderFrame(); aborted.HasError())
                    return Output::Failure(aborted.ErrorValue());
                if (auto tail = host.ProgressTail(); tail.HasError())
                    return Output::Failure(tail.ErrorValue());
                return Output::Success(false);  // Actual missed prediction: finish zero-layer debt, never wait or render another camera.
            }
            return Output::Success(true);
        }

        /** @brief Captures actual runtime views once for the current canonical native prediction. */
        Result<void> LocateViews() {
            const auto state = host.frames_.Snapshot();
            const auto time = host.clock_.PresentationTime(state.predictedDisplayTime);
            if (time.HasError())
                return Result<void>::Failure(time.ErrorValue());
            auto observedViews = host.composition_.session.LocatePresentation(session, time.Value());
            if (observedViews.HasError())
                return Result<void>::Failure(observedViews.ErrorValue());
            if (host.frames_.LocateViews(state.frame, observedViews.Value().count) != XR::XRFrameStatus::Ok)
                return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
            this->views.emplace(std::move(observedViews).Value());
            return Result<void>::Success();
        }

        /** @brief Acquires and retains the next native borrow without claiming render completion. */
        static Result<bool> WaitAndImport(XR::OpenXRInternal::OpenXRSwapchain &owner,
                                          std::optional<XR::OpenXRInternal::NativeSwapchainAcquisition> &image);
        /** @brief Retains every successfully created eye view for retryable retirement. */
        static Result<void> PrepareEyeViews(Render::RenderFrontend &frontend, const XR::OpenXRInternal::NativeSwapchainAcquisition &image,
                                            const Render::RenderTextureDescriptor &descriptor, Render::RenderTextureAspect aspect,
                                            std::span<Render::RenderTextureViewHandle> views);
        /** @brief Releases the caller's view handles while retaining canonical queue pins. */
        static Result<void> ReleaseEyeViews(Render::RenderFrontend &frontend, std::span<Render::RenderTextureViewHandle> views);
        /** @brief Zero-time observes native wait/queue retirement before releasing the exact allocation. */
        static Result<bool> RetireImage(Render::RenderFrontend &frontend, XR::OpenXRInternal::OpenXRSwapchain &owner,
                                        std::span<Render::RenderTextureViewHandle> views);

        /** @brief Zero-time acquires/imports the same native image and creates its bounded eye views before native admission. */
        Result<bool> AcquireEyeImages() {
            using Output = Result<bool>;
            const auto state = host.frames_.Snapshot();
            const auto imported = WaitAndImport(color, acquisition);
            if (imported.HasError() || !imported.Value() || acquisitionRecorded)
                return imported;
            if (host.frames_.Acquire(state.frame, acquisition->image) != XR::XRFrameStatus::Ok)
                return Output::Failure(MakeError(XR::XRErrors::OperationIncompatible));
            acquisitionRecorded = true;
            if (auto ready =
                    PrepareEyeViews(host.composition_.frontend, *acquisition, allocation, Render::RenderTextureAspect::Color, eyeViews);
                ready.HasError())
                return Output::Failure(ready.ErrorValue());
            return Output::Success(true);
        }

        /** @brief Checks the located relation against the exact bound native references and frame prediction. */
        Result<XR::XRPoseSample> ReferencePose(const SceneState &scene, const XR::XRFrameSnapshot &frame,
                                               const XR::XRWorldOriginRevision origin) const {
            const auto &reference = views->referenceToWorld;
            const bool selectedBase =
                reference.source == scene.references.local || (scene.references.stage && reference.source == *scene.references.stage);
            if (session != frame.frame.session || session != scene.references.session || reference.source != views->reference ||
                !selectedBase || reference.target != scene.worldReference ||
                reference.time.renderPrediction != frame.predictedDisplayTime ||
                reference.purpose != XR::XRPosePurpose::PresentationPrediction)
                return Result<XR::XRPoseSample>::Failure(MakeError(XR::XRErrors::IdentityStale));
            return XR::XRPoseSample::Create(reference, session, origin);
        }

        /** @brief Converts the validated same-frame view plan and calibrated reference into the existing scene contract. */
        Result<XR::XRMultiviewScene> ConvertScene(const XR::XRViewRenderPlan &viewPlan, const XR::XRViewConfigurationId &configuration,
                                                  XR::XRViewConfigurationRevision revision, XR::XRWorldOriginRevision origin,
                                                  const XR::XRPoseSample &reference,
                                                  const Render::MultiviewPublication &publication) const {
            const auto &support = host.composition_.frontend.Capabilities().support;
            const std::array acquiredImages{acquisition->image};
            const std::array references{reference, reference};
            const XR::XRMultiviewOwnerState
                owner{session, configuration, revision, origin, acquiredImages, publication, Render::ResolveMultiviewCapabilities(support)};
            return XR::XRMultiviewScene::Create({viewPlan,
                                                 references,
                                                 {},
                                                 publication,
                                                 Math::ClipDepthRange::ZeroToOne,
                                                 0.1F,
                                                 1000.0F,
                                                 Render::MultiviewExecutionPolicy::Multipass,
                                                 {}},
                                                owner);
        }

        /** @brief Polls eye/GUI reference retirement before physical release, without issuing logical frame completion.
         * @pre The same canonical tail has accepted GUI completion or explicit scene retirement; no normal-frame wait.
         */
        Result<bool> RetireImageReferences() {
            if (writer) {
                const auto retired = writer->Retire(host.composition_.frontend);
                if (retired.HasError() || !retired.Value())
                    return retired;
            }
            return RetireImage(host.composition_.frontend, color, eyeViews);
        }

        Result<XrSpace> Borrow(const XR::XRCoordinateSpace &space) const override {
            return host.composition_.session.BorrowPresentationReference(space);
        }

        /** @brief Ends a fully released projection, adding the underlay only for the exact live alpha proof. */
        [[nodiscard]] Result<void> EndRenderedProjection(const XR::XRFrameId &frame);

        /** @brief Reports the exact compositor result to the existing selected viewport after successful native end. */
        void CompleteMotion(bool projected) noexcept;

        /** @brief Retires only the exact candidate's frame/image/queue children through the same host.
         * @param host Same retained owner. @param candidate Exact retained Scene candidate.
         * @return False while actual debt remains, original failure, or complete resource retirement.
         */
        [[nodiscard]] static Result<bool> RetireResources(OpenXRPlayHost &host, const XR::XRSessionId &candidate);
        /** @brief Retires graphics after native stopping or the exact captured Lost state permits recovery. */
        [[nodiscard]] static Result<bool> RetireGraphics(OpenXRPlayHost &host, bool lost);
        /** @brief Polls the same submitted queue debt and releases exact eye views during explicit Lost recovery.
         * @return Original poll/view-release error, retaining every unreleased view for retry. No completion is manufactured.
         */
        [[nodiscard]] Result<void> RetireViewsAfterLoss();
    };

}  // namespace Horo::Application::Internal
