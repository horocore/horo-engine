/** @file OpenXRPlayFrameState.h
 * @brief Private candidate-owned native image and frame debt for the selected application host.
 */
#pragma once

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
        Render::RenderTextureDescriptor allocation;
        std::optional<XR::OpenXRInternal::NativeSwapchainAcquisition> acquisition;
        std::array<Render::RenderTextureViewHandle, 2> eyeViews{};
        std::optional<XR::XRViewRenderPlan> plan;
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
            if (!host.sessions_.Snapshot().admitsFrames)
                return Output::Success(false);  // Real native Ready/Running events, never Scene publication, admit waits.
            rendered = false;
            submitted = false;
            acquisitionRecorded = false;
            guiFinished = false;
            views.reset();
            plan.reset();
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

        /** @brief Zero-time acquires/imports the same native image and creates its bounded eye views before native admission. */
        Result<bool> AcquireEyeImages() {
            using Output = Result<bool>;
            const auto state = host.frames_.Snapshot();
            if (color.Phase() == XR::OpenXRInternal::NativeSwapchainPhase::Available) {
                if (auto acquired = color.Acquire(); acquired.HasError())
                    return Result<bool>::Failure(acquired.ErrorValue());
            }
            if (color.Phase() == XR::OpenXRInternal::NativeSwapchainPhase::Acquired) {
                const auto ready = color.Wait(0);
                if (ready.HasError())
                    return Result<bool>::Failure(ready.ErrorValue());
                if (!ready.Value())
                    return Output::Success(false);  // Zero-time native readiness; retain exact wait/frame/acquisition for next event loop.
            }
            if (color.Phase() == XR::OpenXRInternal::NativeSwapchainPhase::Ready) {
                auto imported = color.Import();
                if (imported.HasError())
                    return Result<bool>::Failure(imported.ErrorValue());
                acquisition.emplace(std::move(imported).Value());
                if (host.frames_.Acquire(state.frame, acquisition->image) != XR::XRFrameStatus::Ok)
                    return Result<bool>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
                acquisitionRecorded = true;
                for (std::uint32_t eye = 0; eye < eyeViews.size(); ++eye) {
                    auto view = host.composition_.frontend.CreateTextureView(
                        {.texture = acquisition->texture, .format = allocation.format, .baseLayer = eye, .layerCount = 1});
                    if (view.HasError())
                        return Result<bool>::Failure(view.ErrorValue());
                    eyeViews[eye] = view.Value().handle;
                    if (auto processed = host.composition_.frontend.ProcessResourceRequests(); processed.HasError())
                        return Result<bool>::Failure(processed.ErrorValue());
                    if (auto ready = host.composition_.frontend.ResourceOperationResult(view.Value().operation); ready.HasError())
                        return Result<bool>::Failure(ready.ErrorValue());
                }
            }
            return Output::Success(true);
        }

        /** @brief Converts the validated same-frame view plan and calibrated reference into the existing scene contract. */
        Result<XR::XRMultiviewScene> ConvertScene(const XR::XRViewRenderPlan &viewPlan, const XR::XRViewConfigurationId &configuration,
                                                  XR::XRViewConfigurationRevision revision, XR::XRWorldOriginRevision origin,
                                                  const Render::MultiviewPublication &publication) const {
            const auto &support = host.composition_.frontend.Capabilities().support;
            const std::array acquiredImages{acquisition->image};
            auto reference = XR::XRPoseSample::Create(views->referenceToWorld, session, origin);
            if (reference.HasError())
                return Result<XR::XRMultiviewScene>::Failure(reference.ErrorValue());
            const std::array references{reference.Value(), reference.Value()};
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
            for (auto &view : eyeViews) {
                if (!view.IsValid())
                    continue;
                if (auto released = host.composition_.frontend.ReleaseTextureView(view); released.HasError())
                    return Result<bool>::Failure(released.ErrorValue());
                view = {};
            }
            if (color.Phase() == XR::OpenXRInternal::NativeSwapchainPhase::Acquired) {
                auto ready = color.Wait(0);
                if (ready.HasError())
                    return Result<bool>::Failure(ready.ErrorValue());
                if (!ready.Value())
                    return Result<bool>::Success(false);
            }
            if (color.Phase() != XR::OpenXRInternal::NativeSwapchainPhase::Available) {
                const auto released = color.PollRelease();
                if (released.HasError())
                    return Result<bool>::Failure(released.ErrorValue());
                if (!released.Value())
                    return Result<bool>::Success(false);
            }
            return Result<bool>::Success(true);
        }

        Result<XrSpace> Borrow(const XR::XRCoordinateSpace &space) const override {
            return host.composition_.session.BorrowPresentationReference(space);
        }

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
