#pragma once

#include "Horo/Runtime/Render/FramePacingErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/RenderShaderResources.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "MetalBackendModule.h"
#include "MetalDeviceCapabilities.h"
#include "MetalRenderBackendErrors.h"

#include <memory>

namespace Horo::Render::Detail {
    /** @brief Runtime-owned Metal execution contract used to isolate native code and enable headless contract tests. */
    class IMetalRuntime : public IRenderResourceBackend, public IMaterialBindingBackend {
    public:
        ~IMetalRuntime() override = default;

        /** @brief Unsupported runtimes cannot publish an issued native VFX slot. */
        [[nodiscard]] virtual Result<void> UpdateVfxFrame(const NativeVfxFrameUpdate &) {
            return Result<void>::Failure(MakeError(VfxFrameErrors::Unavailable));
        }

        /** @brief Default injected runtimes explicitly decline native VFX draw admission. */
        [[nodiscard]] virtual Result<void> ValidateVfxWorkload(const RenderGraphVfx &, const RenderGraphExecutionRequest &) const {
            return Result<void>::Failure(MakeError(VfxFrameErrors::Unavailable));
        }

        /** @brief Default injected runtimes never treat an unencoded VFX draw as success. */
        [[nodiscard]] virtual Result<void> ExecuteVfxWorkload(const RenderGraphVfx &, const RenderGraphExecutionRequest &) {
            return Result<void>::Failure(MakeError(VfxFrameErrors::Unavailable));
        }

        /** @brief Unsupported injected runtimes never claim frame-slot upload completion. */
        [[nodiscard]] virtual Result<void> UpdateLightFrame(const NativeLightFrameUpdate &) {
            return Result<void>::Failure(MakeError(LightCullingErrors::Unsupported));
        }

        /** @brief Default injected runtimes decline native cooked light preparation explicitly. */
        [[nodiscard]] virtual Result<std::shared_ptr<IResidentLightCullingKernel>> RealizeLightCullingKernel(
            const CookedLightCullingKernel &) {
            return Result<std::shared_ptr<IResidentLightCullingKernel>>::Failure(MakeError(LightCullingErrors::Unsupported));
        }

        /** @brief Injected runtimes explicitly decline native cooked UI pipelines unless implemented. */
        [[nodiscard]] virtual Result<std::shared_ptr<IResidentUiPaintKernel>> RealizeUiPaintKernel(const CookedUiPaintKernel &) {
            return Result<std::shared_ptr<IResidentUiPaintKernel>>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution));
        }

        /** @brief Default injected runtimes decline reusable native UI updates. */
        [[nodiscard]] virtual Result<void> UpdateUiPaintFrame(const NativeUiPaintFrameUpdate &) {
            return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution));
        }

        [[nodiscard]] virtual Result<MetalDeviceCapabilities> Initialize(const MetalPresentationDescriptor &descriptor,
                                                                         const MetalDeviceAdmissionRequest &request) = 0;

        [[nodiscard]] virtual Result<bool> PollPresentationAvailability(FramebufferExtent) {
            return Result<bool>::Success(true);
        }

        [[nodiscard]] virtual Result<RenderPresentationOutput> ConfigurePresentationOutput(const RenderPresentationOutputRequest &) {
            return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::OutputUnsupported));
        }

        [[nodiscard]] virtual Result<RenderPresentationOutput> PresentationOutput() const {
            return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::OutputUnsupported));
        }

        [[nodiscard]] virtual Result<void> BeginFrame(FramebufferExtent extent) = 0;

        /** @brief Admits exact primary encoding before any operation in the frame is encoded.
         * @param attachment Complete graph-final pixel declaration. @return Exact native admission or typed failure.
         * @details Legacy injected runtimes admit only an absent declaration and cannot accidentally claim HDR output.
         */
        [[nodiscard]] virtual Result<void> ValidatePrimaryOutput(const PrimaryOutputAttachment &attachment) const {
            return attachment.outputColor ? Result<void>::Failure(MakeError(RenderPresentationErrors::OutputUnsupported))
                                          : Result<void>::Success();
        }

        [[nodiscard]] virtual Result<void> ExecutePrimaryOutput(const PrimaryOutputAttachment &attachment) = 0;

        /** @brief Checks all native resource identities and ranges before any graph command is encoded. */
        [[nodiscard]] virtual Result<void> ValidateGraphWorkload(const RenderGraphWorkload &workload,
                                                                 std::span<const RenderGraphResourceInstance>) const {
            if (const auto *primary = std::get_if<PrimaryOutputAttachment>(&workload))
                return ValidatePrimaryOutput(*primary);
            if (std::holds_alternative<std::monostate>(workload)) {
                return Result<void>::Success();
            }
            return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution));
        }

        /** @brief Encodes a prevalidated operation, retaining native references until command completion. */
        [[nodiscard]] virtual Result<void> ExecuteGraphWorkload(const RenderGraphWorkload &workload,
                                                                std::span<const RenderGraphResourceInstance>) {
            if (const auto *primary = std::get_if<PrimaryOutputAttachment>(&workload)) {
                return ExecutePrimaryOutput(*primary);
            }
            if (std::holds_alternative<std::monostate>(workload)) {
                return Result<void>::Success();
            }
            return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution));
        }

        /** @brief Borrows a frontend-owned lease until exact command completion or unsent-frame abandonment. */
        [[nodiscard]] virtual Result<void> RetainGraphResources(IRenderGraphResourceLease &lease) = 0;

        /** @brief Explicit worker-safe native encoding envelope; default fake/unsupported runtimes opt out. */
        [[nodiscard]] virtual RenderParallelRecordingCapabilities ParallelRecordingCapabilities() const noexcept {
            return {};
        }

        /** @brief Owner-only capture; success transfers the exact frontend lease until abandonment/native completion. */
        [[nodiscard]] virtual Result<std::shared_ptr<IRenderParallelGraphRecording>> PrepareParallelGraph(
            const RenderGraphExecutionRequest &) {
            return Result<std::shared_ptr<IRenderParallelGraphRecording>>::Failure(
                MakeError(MetalBackendErrors::UnsupportedGraphExecution));
        }

        /** @brief Owner-only readiness/frame/session admission, never a native completion wait. */
        [[nodiscard]] virtual Result<void> AcceptParallelGraph(const std::shared_ptr<IRenderParallelGraphRecording> &) {
            return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution));
        }

        [[nodiscard]] virtual Result<void> Present() = 0;

        /** @brief Adds native feedback when implemented; otherwise retains ordinary presentation. */
        [[nodiscard]] virtual Result<void> PresentWithTiming(const PresentationTimingRequest &) {
            return Present();
        }

        /** @brief Polls without waiting; absent native providers fail explicitly. */
        [[nodiscard]] virtual Result<std::optional<NativePresentTiming>> PollNativePresentTiming() {
            return Result<std::optional<NativePresentTiming>>::Failure(MakeError(FramePacingErrors::NativeTimingUnsupported));
        }

        virtual void AbortFrame() noexcept = 0;
        [[nodiscard]] virtual Result<void> Resize(FramebufferExtent extent) = 0;
        /** @brief Closes owner-thread admission and cancels unsent work without waiting or releasing pending parents. */
        [[nodiscard]] virtual Result<void> BeginShutdown() = 0;
        /** @brief Polls finite native/CPU inventories; only Drained permits releasing the owning composition. */
        [[nodiscard]] virtual Result<RenderBackendDrainProgress> PollDrain() = 0;
    };

    /** @brief Inert factory seam for constructing a runtime without acquiring native resources. */
    class IMetalRuntimeFactory {
    public:
        virtual ~IMetalRuntimeFactory() = default;

        [[nodiscard]] virtual Result<std::unique_ptr<IMetalRuntime>> Create(IMetalPresentationPort &presentationPort,
                                                                            MetalEditorGraphicsBridge &editorGraphicsBridge) const = 0;
    };

    /** @brief Private accessor that lets the runtime publish borrowed objects without widening the bridge API. */
    struct MetalEditorGraphicsAccess {
        static void PublishPersistent(MetalEditorGraphicsBridge &bridge, void *device, void *commandQueue, void *waitContext,
                                      MetalEditorGraphicsBridge::WaitUntilIdleFunction wait) noexcept;
        static void PublishFrame(MetalEditorGraphicsBridge &bridge, void *commandBuffer, void *renderPassDescriptor,
                                 void *renderEncoder) noexcept;
        static void ClearFrame(MetalEditorGraphicsBridge &bridge) noexcept;
        static void Clear(MetalEditorGraphicsBridge &bridge) noexcept;
    };

    /** @brief Creates the production native Metal runtime without acquiring resources. */
    [[nodiscard]] Result<std::unique_ptr<IMetalRuntime>> CreateMetalRuntime(IMetalPresentationPort &presentationPort,
                                                                            MetalEditorGraphicsBridge &editorGraphicsBridge);

    /** @brief Registers Metal with an injected inert runtime factory for contract tests. */
    [[nodiscard]] Result<void> RegisterMetalRenderBackendWithRuntimeFactory(RenderBackendRegistry &registry,
                                                                            IMetalPresentationPort &presentationPort,
                                                                            MetalEditorGraphicsBridge &editorGraphicsBridge,
                                                                            const IMetalRuntimeFactory &runtimeFactory);
}  // namespace Horo::Render::Detail
