#pragma once

#include "MetalBackendModule.h"

#include <memory>

namespace Horo::Render::Detail {
    /** @brief Runtime-owned Metal execution contract used to isolate native code and enable headless contract tests. */
    class IMetalRuntime : public IRenderResourceBackend {
    public:
        ~IMetalRuntime() override = default;

        [[nodiscard]] virtual Result<void> Initialize(const MetalPresentationDescriptor &descriptor) = 0;
        [[nodiscard]] virtual Result<void> BeginFrame(FramebufferExtent extent) = 0;
        [[nodiscard]] virtual Result<void> ExecutePrimaryOutput(const PrimaryOutputAttachment &attachment) = 0;
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
