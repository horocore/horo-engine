#include "InteractiveEditorUiTestRenderer.h"

#include "editor/renderer/EditorViewportScene.h"

#include <array>
#include <span>
#include <stdexcept>
#include <utility>

namespace Horo::Tests {
    namespace {
        [[noreturn]] void ThrowRendererError(const Error &error) {
            throw std::runtime_error(error.message);
        }
    }  // namespace

    std::unique_ptr<InteractiveEditorUiTestRenderer> InteractiveEditorUiTestRenderer::Create(
        std::unique_ptr<Render::RenderFrontend> frontend, std::unique_ptr<Editor::IEditorGuiRenderer> guiRenderer,
        std::unique_ptr<Editor::IEditorViewportRenderer> viewportRenderer) {
        if (frontend == nullptr || guiRenderer == nullptr || viewportRenderer == nullptr)
            throw std::invalid_argument("Interactive UI-test renderer composition requires all renderer services.");

        auto composition = std::unique_ptr<InteractiveEditorUiTestRenderer>{
            new InteractiveEditorUiTestRenderer(std::move(frontend), std::move(guiRenderer), std::move(viewportRenderer))};
        composition->Initialize();
        return composition;
    }

    InteractiveEditorUiTestRenderer::~InteractiveEditorUiTestRenderer() {
        Shutdown();
    }

    void InteractiveEditorUiTestRenderer::BeginFrame(const Render::FramebufferExtent outputExtent) {
        if (frame_.has_value())
            throw std::logic_error("Interactive UI-test renderer already owns an active frame.");
        if (!outputExtent.IsValid())
            throw std::invalid_argument("Interactive UI-test renderer requires a valid drawable extent.");

        auto prepared =
            viewportRenderer_->PrepareResources(*frontend_, Render::RenderSceneView{.camera = ToRenderCamera(previousScene_.camera),
                                                                                    .meshResources = previousScene_.meshResources,
                                                                                    .instances = previousScene_.instances,
                                                                                    .lights = previousScene_.lights});
        if (prepared.HasError())
            ThrowRendererError(prepared.ErrorValue());
        if (prepared.Value().has_value())
            viewportTarget_ = *prepared.Value();

        if (outputExtent.width != outputExtent_.width || outputExtent.height != outputExtent_.height) {
            const Result<void> resized = frontend_->Resize(outputExtent);
            if (resized.HasError())
                ThrowRendererError(resized.ErrorValue());
            outputExtent_ = outputExtent;
        }

        auto begun = frontend_->BeginFrame(Render::FrameDescriptor{.frameNumber = frameNumber_++, .outputExtent = outputExtent});
        if (begun.HasError())
            ThrowRendererError(begun.ErrorValue());
        frame_.emplace(std::move(begun).Value());
        frameExecuted_ = false;

        const Result<void> guiBegun = guiRenderer_->BeginFrame();
        if (guiBegun.HasError()) {
            frame_->Cancel();
            frame_.reset();
            ThrowRendererError(guiBegun.ErrorValue());
        }
    }

    void InteractiveEditorUiTestRenderer::RenderViewport(const Editor::EditorViewportSceneView scene) {
        previousScene_ = scene;
        ExecutePasses(scene, viewportRenderer_->RequestedExtent().IsValid() && viewportTarget_.IsValid() && viewportRenderer_->IsReady());
    }

    void InteractiveEditorUiTestRenderer::Present() {
        if (!frame_.has_value())
            throw std::logic_error("Interactive UI-test renderer cannot present without an active frame.");
        if (!frameExecuted_)
            ExecutePasses({}, false);

        const Result<void> rendered = frame_->ComposeGui(*guiRenderer_);
        if (rendered.HasError()) {
            frame_->Cancel();
            frame_.reset();
            ThrowRendererError(rendered.ErrorValue());
        }

        const Result<void> presented = frame_->Present();
        frame_.reset();
        if (presented.HasError())
            ThrowRendererError(presented.ErrorValue());
    }

    void InteractiveEditorUiTestRenderer::Shutdown() noexcept {
        if (frame_.has_value()) {
            frame_->Cancel();
            frame_.reset();
        }
        if (guiInitialized_) {
            guiRenderer_->Shutdown();
            guiInitialized_ = false;
        }
        if (frontend_ != nullptr && executorAttached_) {
            frontend_->DetachStaticMeshPassExecutor(*viewportRenderer_);
            executorAttached_ = false;
        }
        if (frontend_ != nullptr && viewportTarget_.IsValid()) {
            if (!frontend_->Capabilities().supportsRenderTargetResources)
                static_cast<void>(frontend_->ReleaseOffscreenTarget(viewportTarget_));
            viewportTarget_ = {};
        }
        viewportRenderer_.reset();
        guiRenderer_.reset();
        frontend_.reset();
        outputExtent_ = {};
        frameExecuted_ = false;
    }

    Editor::IEditorViewportRenderer &InteractiveEditorUiTestRenderer::ViewportRenderer() noexcept {
        return *viewportRenderer_;
    }

    InteractiveEditorUiTestRenderer::InteractiveEditorUiTestRenderer(
        std::unique_ptr<Render::RenderFrontend> frontend, std::unique_ptr<Editor::IEditorGuiRenderer> guiRenderer,
        std::unique_ptr<Editor::IEditorViewportRenderer> viewportRenderer) noexcept
        : frontend_(std::move(frontend)), guiRenderer_(std::move(guiRenderer)), viewportRenderer_(std::move(viewportRenderer)) {}

    void InteractiveEditorUiTestRenderer::Initialize() {
        const Result<void> guiInitialized = guiRenderer_->Initialize();
        if (guiInitialized.HasError())
            ThrowRendererError(guiInitialized.ErrorValue());
        guiInitialized_ = true;

        const Result<void> attached = frontend_->AttachStaticMeshPassExecutor(*viewportRenderer_);
        if (attached.HasError())
            ThrowRendererError(attached.ErrorValue());
        executorAttached_ = true;

        if (!frontend_->Capabilities().supportsRenderTargetResources) {
            auto target = frontend_->CreateOffscreenTarget({1, 1});
            if (target.HasError())
                ThrowRendererError(target.ErrorValue());
            viewportTarget_ = target.Value();
        }
    }

    void InteractiveEditorUiTestRenderer::ExecutePasses(const Editor::EditorViewportSceneView scene, const bool includeViewport) {
        if (!frame_.has_value() || frameExecuted_)
            throw std::logic_error("Interactive UI-test renderer pass plan has an invalid frame state.");

        std::array<Render::RenderPassDescriptor, 2> passes{};
        std::size_t passCount = 0;
        if (includeViewport) {
            const Editor::EditorViewportExtent extent = viewportRenderer_->RequestedExtent();
            const Render::FramebufferExtent framebufferExtent{extent.width, extent.height};
            const Result<void> resized = frontend_->ResizeOffscreenTarget(viewportTarget_, framebufferExtent);
            if (resized.HasError())
                ThrowRendererError(resized.ErrorValue());
            passes[passCount++] =
                Render::RenderPassDescriptor{.id = Render::RenderPassId{1},
                                             .kind = Render::RenderPassKind::Graphics,
                                             .staticMesh =
                                                 Render::StaticMeshPassDescriptor{.target = viewportTarget_,
                                                                                  .extent = framebufferExtent,
                                                                                  .scene = Render::RenderSceneView{
                                                                                      .camera = Editor::ToRenderCamera(scene.camera),
                                                                                      .meshResources = scene.meshResources,
                                                                                      .instances = scene.instances,
                                                                                      .lights = scene.lights,
                                                                                  }}};
        }
        passes[passCount++] =
            Render::RenderPassDescriptor{.id = Render::RenderPassId{2},
                                         .kind = Render::RenderPassKind::Graphics,
                                         .primaryOutput =
                                             Render::PrimaryOutputAttachment{.loadOperation = Render::AttachmentLoadOperation::Clear,
                                                                             .storeOperation = Render::AttachmentStoreOperation::Store,
                                                                             .clearColor =
                                                                                 Render::ClearColor{0.035F, 0.045F, 0.065F, 1.0F}}};

        const Result<void> executed = frame_->Execute(std::span<const Render::RenderPassDescriptor>{passes.data(), passCount});
        if (executed.HasError()) {
            frame_.reset();
            ThrowRendererError(executed.ErrorValue());
        }
        frameExecuted_ = true;
    }
}  // namespace Horo::Tests
