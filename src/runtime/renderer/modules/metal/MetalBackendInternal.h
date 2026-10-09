#pragma once

#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "MetalBackendModule.h"
#include "MetalDeviceCapabilities.h"
#include "MetalRenderBackendErrors.h"

#include <memory>

namespace Horo::Render::Detail {
    /** @brief Runtime-owned Metal execution contract used to isolate native code and enable headless contract tests. */
    class IMetalRuntime : public IRenderResourceBackend {
    public:
        ~IMetalRuntime() override = default;

        [[nodiscard]] virtual Result<MetalDeviceCapabilities> Initialize(const MetalPresentationDescriptor &descriptor,
                                                                         const MetalDeviceAdmissionRequest &request) = 0;
        [[nodiscard]] virtual Result<void> BeginFrame(FramebufferExtent extent) = 0;
        [[nodiscard]] virtual Result<void> ExecutePrimaryOutput(const PrimaryOutputAttachment &attachment) = 0;

        /** @brief Checks all native resource identities and ranges before any graph command is encoded. */
        [[nodiscard]] virtual Result<void> ValidateGraphWorkload(const RenderGraphWorkload &workload,
                                                                 std::span<const RenderGraphResourceInstance>) const {
            if (std::holds_alternative<std::monostate>(workload) || std::holds_alternative<PrimaryOutputAttachment>(workload)) {
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
        [[nodiscard]] virtual Result<void> Present() = 0;
        virtual void AbortFrame() noexcept = 0;
        [[nodiscard]] virtual Result<void> Resize(FramebufferExtent extent) = 0;
        virtual void Shutdown() noexcept = 0;
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
