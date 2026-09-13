#pragma once

/**
 * @file MetalBackendModule.h
 * @brief Metal backend registration and editor-private graphics bridge contracts.
 */

#include "Horo/Runtime/Render/RenderBackendRegistry.h"

#include <memory>

namespace Horo::Render {
    namespace Detail {
        struct MetalEditorGraphicsAccess;
    }

    /** @brief Metal presentation policy captured when the backend acquires its native resources. */
    struct MetalPresentationDescriptor {
        bool enableValidation{false};
        std::uint32_t maxFramesInFlight{2};
        PresentMode presentMode{PresentMode::Fifo};
    };

    /** @brief Returns immutable native-free metadata used before creating the host window. */
    [[nodiscard]] const RenderBackendModuleInfo &GetMetalRenderBackendModuleInfo() noexcept;

    /**
     * @brief Creates a bounded synchronous discovery owner for native Metal adapters.
     * @return Discovery service or a typed platform initialization failure.
     *
     * Discovery performs no surface, command queue, or render-device activation. The
     * returned owner must be stopped before the host unloads the Metal module.
     */
    [[nodiscard]] Result<std::unique_ptr<IRenderAdapterDiscovery>> CreateMetalAdapterDiscovery();

    /**
     * @brief Platform surface attachment borrowed by the Metal runtime.
     *
     * The concrete port owns only the host window's Metal view and layer attachment.
     * Metal devices, queues, drawables, command buffers, and encoders remain owned by
     * the runtime Metal module. Calls are serialized on the host render-capable thread.
     */
    class IMetalPresentationPort {
    public:
        virtual ~IMetalPresentationPort() = default;

        /**
         * @brief Creates the host Metal view and layer attachment.
         * @return Success or a typed platform failure. A returned failure retains no surface.
         */
        [[nodiscard]] virtual Result<void> CreateSurface() = 0;

        /**
         * @brief Returns the borrowed platform layer as an editor-private opaque pointer.
         * @return Current CAMetalLayer pointer, or nullptr when no surface exists.
         */
        [[nodiscard]] virtual void *Layer() const noexcept = 0;  // NOSONAR(cpp:S5008) Required opaque platform seam.

        /** @brief Releases the host Metal view and layer attachment; repeated calls are safe. */
        virtual void DestroySurface() noexcept = 0;
    };

    /**
     * @brief Read-only editor integration view over runtime-owned Metal objects.
     *
     * Every returned pointer is borrowed and valid only for the lifetime/state described
     * by its accessor. This bridge is private composition infrastructure and is not part
     * of Horo's public renderer API.
     */
    class MetalEditorGraphicsBridge {
    public:
        /** @brief Returns the runtime-owned Metal device while the backend is initialized. */
        [[nodiscard]] void *Device() const noexcept;

        /** @brief Returns the runtime-owned Metal command queue while the backend is initialized. */
        [[nodiscard]] void *CommandQueue() const noexcept;

        /** @brief Returns the active frame command buffer, or nullptr outside a frame. */
        [[nodiscard]] void *CurrentCommandBuffer() const noexcept;

        /** @brief Returns the active primary render-pass descriptor, or nullptr outside a frame. */
        [[nodiscard]] void *CurrentRenderPassDescriptor() const noexcept;

        /** @brief Returns the active primary render encoder, or nullptr before primary pass encoding. */
        [[nodiscard]] void *CurrentRenderEncoder() const noexcept;

        /** @brief Waits for submitted Metal work; restricted to teardown and deterministic tests. */
        void WaitUntilIdle() noexcept;

    private:
        friend struct Detail::MetalEditorGraphicsAccess;

        using WaitUntilIdleFunction = void (*)(void *) noexcept;

        void *device_{nullptr};
        void *commandQueue_{nullptr};
        void *commandBuffer_{nullptr};
        void *renderPassDescriptor_{nullptr};
        void *renderEncoder_{nullptr};
        void *waitContext_{nullptr};
        WaitUntilIdleFunction waitUntilIdle_{nullptr};
    };

    /**
     * @brief Registers the inert Metal backend provider under the canonical `metal` identity.
     * @param registry Mutable renderer backend registry owned by the host.
     * @param presentationPort Borrowed platform surface port that outlives all created backends.
     * @param editorGraphicsBridge Stable borrowed bridge updated from runtime lifecycle transitions.
     * @return Registration result; no Metal device, surface, drawable, or command resource is created.
     */
    [[nodiscard]] Result<void> RegisterMetalRenderBackend(RenderBackendRegistry &registry, IMetalPresentationPort &presentationPort,
                                                          MetalEditorGraphicsBridge &editorGraphicsBridge);
}  // namespace Horo::Render
