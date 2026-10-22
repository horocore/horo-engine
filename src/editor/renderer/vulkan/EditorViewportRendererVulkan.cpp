#include "EditorViewportRendererVulkan.h"

#include "editor/renderer/EditorRendererErrors.h"

#include <algorithm>
#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Stable private GUI name; exact view generation is always carried separately by the existing viewport owner. */
        Result<std::uintptr_t> ViewportImage(const Render::RenderFrontend &frontend, Render::RenderTextureViewHandle view) {
            const auto ready = frontend.BackendInstance(view);
            if (ready.HasError())
                return Result<std::uintptr_t>::Failure(ready.ErrorValue());
            return Result<std::uintptr_t>::Success(std::numeric_limits<std::uintptr_t>::max());
        }
    }  // namespace

    /** @copydoc EditorViewportRendererVulkan::EditorViewportRendererVulkan */
    EditorViewportRendererVulkan::EditorViewportRendererVulkan(Render::RenderFrontend &frontend,
                                                               Render::Detail::VulkanEditorGraphicsBridge &bridge,
                                                               EditorVulkanViewportCook cooked) noexcept
        : frontend_(frontend), bridge_(bridge), shader_{.cooked = std::move(cooked)},
          resources_(frontend, {.resolveImage = &ViewportImage,
                                .meshBufferAccess = Render::RenderBufferAccess::HostVisible,
                                .prepareDirectionalShadow = false}) {}

    EditorViewportRendererVulkan::~EditorViewportRendererVulkan() {
        Shutdown();
    }

    /** @copydoc EditorViewportRendererVulkan::Initialize */
    Result<void> EditorViewportRendererVulkan::Initialize() {
        if (owner_ != std::this_thread::get_id() || initialized_ || !bridge_.Port())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        const auto device = bridge_.Port()->QueryDevice();
        if (device.HasError())
            return Result<void>::Failure(device.ErrorValue());
        device_ = device.Value();
        if (device_.frames == 0 || device_.frames > slots_.size())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportGeometryCreationFailed));
        const auto attached = bridge_.Port()->AttachDeviceChild(*this);
        if (attached.HasError())
            return attached;
        childAttached_ = true;
        if (const auto pipeline = CreatePipeline(); pipeline.HasError()) {
            Shutdown();
            return pipeline;
        }
        if (const auto slots = CreateDrawSlots(); slots.HasError()) {
            Shutdown();
            return slots;
        }
        initialized_ = true;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::ReleaseNative */
    void EditorViewportRendererVulkan::ReleaseNative() noexcept {
        if (owner_ != std::this_thread::get_id())
            std::terminate();
        for (auto &draw : preparedDraws_)
            draw = {};
        for (auto &slot : slots_)
            for (auto &draw : slot.draws)
                draw.material.reset();
        if (shader_.materials)
            static_cast<void>(shader_.materials->Shutdown());
        shader_.materials.reset();
        initialized_ = false;
        childAttached_ = false;
        recordedEver_ = false;
    }

    /** @copydoc EditorViewportRendererVulkan::Shutdown */
    void EditorViewportRendererVulkan::Shutdown() noexcept {
        if (owner_ != std::this_thread::get_id() || (childAttached_ && recordedEver_))
            std::terminate();
        if (childAttached_ && bridge_.Port())
            bridge_.Port()->DetachDeviceChild(*this);
        ReleaseNative();
        emptyGraph_.reset();
        emptyGraphTexture_ = {};
        resources_.Shutdown();
        for (auto &slot : slots_)
            for (auto &draw : slot.draws) {
                if (draw.buffer.IsValid())
                    static_cast<void>(frontend_.ReleaseBuffer(draw.buffer));
                draw = {};
            }
        if (shader_.pipeline.IsValid())
            static_cast<void>(frontend_.ReleaseGraphicsPipeline(shader_.pipeline));
        if (shader_.vertex.IsValid())
            static_cast<void>(frontend_.ReleaseShaderModule(shader_.vertex));
        if (shader_.fragment.IsValid())
            static_cast<void>(frontend_.ReleaseShaderModule(shader_.fragment));
        shader_.pipeline = {};
        shader_.vertex = {};
        shader_.fragment = {};
    }

    /** @copydoc IEditorViewportRenderer::PrepareResources */
    Result<std::optional<Render::RenderTargetHandle>> EditorViewportRendererVulkan::PrepareResources(Render::RenderFrontend &frontend,
                                                                                                     const Render::RenderSceneView &scene) {
        if (&frontend != &frontend_ || owner_ != std::this_thread::get_id() || !initialized_)
            return Result<std::optional<Render::RenderTargetHandle>>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        const auto prepared = resources_.Prepare(scene, requestedExtent_);
        if (prepared.HasError())
            return prepared;
        if (prepared.Value() && resources_.ColorTexture() != emptyGraphTexture_) {
            if (const auto clear = CreateEmptyGraph(); clear.HasError())
                return Result<std::optional<Render::RenderTargetHandle>>::Failure(clear.ErrorValue());
        }
        return prepared;
    }

    void EditorViewportRendererVulkan::RequestExtent(EditorViewportExtent extent) noexcept {
        requestedExtent_ = extent;
    }

    void EditorViewportRendererVulkan::RequestGrid(const EditorViewportGridOptions &options) noexcept {
        requestedGrid_ = options;
    }

    void EditorViewportRendererVulkan::RequestLightVisualizer(const EditorViewportLightVisualizerOptions &options) noexcept {
        requestedLightVisualizer_ = options;
    }

    EditorViewportExtent EditorViewportRendererVulkan::RequestedExtent() const noexcept {
        // Panel requests are next-boundary intent; this frame must keep the complete published target's extent.
        const auto allocated = resources_.AllocatedExtent();
        return requestedExtent_.IsValid() && allocated.IsValid() ? allocated : requestedExtent_;
    }

    Math::ClipDepthRange EditorViewportRendererVulkan::ClipDepthRange() const noexcept {
        return Math::ClipDepthRange::ZeroToOne;
    }

    EditorViewportTextureView EditorViewportRendererVulkan::TextureView() const noexcept {
        return resources_.IsReady()
                   ? EditorViewportTextureView{.textureId = resources_.ImageIdentity(), .sampledView = resources_.ColorTextureView()}
                   : EditorViewportTextureView{};
    }

    bool EditorViewportRendererVulkan::IsReady() const noexcept {
        return initialized_ && resources_.IsReady();
    }

    Result<void> EditorViewportRendererVulkan::ExecuteStaticMeshPass(const Render::StaticMeshPassDescriptor &) {
        return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene,
                                               "This selected viewport requires its exact frontend prepared-frame material submission."));
    }
}  // namespace Horo::Editor
