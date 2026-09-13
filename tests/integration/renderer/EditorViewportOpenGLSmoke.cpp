#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "editor/renderer/opengl/EditorViewportRendererOpenGL.h"
#include "editor/renderer/opengl/SdlOpenGLPresentationPort.h"
#include "runtime/renderer/modules/opengl/OpenGLBackendModule.h"

#include <SDL3/SDL.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <glad/gl.h>
#include <memory>
#include <vector>

namespace Horo::Render::OpenGLSmokeTests {
    using Editor::EditorViewportExtent;
    using Editor::EditorViewportInstance;
    using Editor::EditorViewportMeshResourceView;
    using Editor::EditorViewportRendererOpenGL;
    using Editor::EditorViewportSceneView;
    using Editor::EditorViewportTextureView;
    using Editor::SdlOpenGLPresentationPort;
    using Editor::ToRenderCamera;

    void Check(const bool condition) {
        REQUIRE((condition));
    }

    /** @brief Uses a real SDL OpenGL context while omitting display-server presentation pacing in headless CI. */
    class HeadlessOpenGLPresentationPort final : public IOpenGLPresentationPort {
    public:
        explicit HeadlessOpenGLPresentationPort(SDL_Window &window) noexcept : port_(window) {}

        Result<void> CreateContext(const OpenGLContextDescriptor &descriptor) override {
            return port_.CreateContext(descriptor);
        }

        Result<void> MakeCurrent() override {
            return port_.MakeCurrent();
        }

        Result<void> LoadCommandDispatch() override {
            return port_.LoadCommandDispatch();
        }

        Result<OpenGLContextFacts> QueryContextFacts() override {
            return port_.QueryContextFacts();
        }

        Result<void> SetPresentMode(PresentMode) override {
            return Result<void>::Success();
        }

        Result<void> SwapBuffers() override {
            return port_.SwapBuffers();
        }

        void DestroyContext() noexcept override {
            port_.DestroyContext();
        }

    private:
        SdlOpenGLPresentationPort port_;
    };

    RenderTargetHandle PrepareViewportTarget(EditorViewportRendererOpenGL &viewport, RenderFrontend &frontend,
                                             const EditorViewportSceneView &scene, const EditorViewportExtent extent) {
        for (std::size_t attempt = 0; attempt < 4; ++attempt) {
            viewport.RequestExtent(extent);
            auto prepared = viewport.PrepareResources(frontend, RenderSceneView{ToRenderCamera(scene.camera), scene.meshResources,
                                                                                scene.instances, scene.lights});
            Check(prepared.HasValue());
            Check(frontend.ProcessResourceRequests().HasValue());
            if (prepared.Value().has_value())
                return *prepared.Value();
        }
        Check(false);
        return {};
    }

    /** @brief Verifies that replacements keep the active viewport usable until publication. */
    void CheckSeamlessResourceTransition(EditorViewportRendererOpenGL &viewport, RenderFrontend &frontend,
                                         const EditorViewportSceneView &scene, const RenderTargetHandle activeTarget,
                                         const EditorViewportTextureView activeTexture) {
        constexpr EditorViewportExtent activeExtent{512, 384};
        constexpr EditorViewportExtent resizedExtent{384, 256};
        std::vector<EditorViewportMeshResourceView> replacementMeshes{scene.meshResources.begin(), scene.meshResources.end()};
        replacementMeshes.front().handle.generation = 2;
        std::vector<EditorViewportInstance> replacementInstances{scene.instances.begin(), scene.instances.end()};
        replacementInstances.front().mesh.generation = 2;
        const EditorViewportSceneView replacementScene{scene.camera, replacementMeshes, replacementInstances, scene.lights};
        const RenderSceneView renderScene{ToRenderCamera(replacementScene.camera), replacementScene.meshResources,
                                          replacementScene.instances, replacementScene.lights};

        RenderTargetHandle resizedTarget;
        RenderTargetHandle currentTarget = activeTarget;
        viewport.RequestExtent(resizedExtent);
        for (std::size_t attempt = 0; attempt < 6 && !resizedTarget.IsValid(); ++attempt) {
            auto prepared = viewport.PrepareResources(frontend, renderScene);
            Check(prepared.HasValue() && prepared.Value().has_value());
            currentTarget = *prepared.Value();
            Check(frontend.ProcessResourceRequests().HasValue());

            // Match the application frame order: prepare the preceding request,
            // then let the panel publish this frame's request before pass execution.
            viewport.RequestExtent(resizedExtent);
            const EditorViewportExtent passExtent = viewport.RequestedExtent();
            Check(passExtent.IsValid());

            auto begun = frontend.BeginFrame(FrameDescriptor{.frameNumber = 2 + attempt, .outputExtent = {640, 480}});
            Check(begun.HasValue());
            RenderFrameScope frame = std::move(begun).Value();
            const std::array passes{RenderPassDescriptor{
                .id = RenderPassId{1},
                .kind = RenderPassKind::Graphics,
                .staticMesh = StaticMeshPassDescriptor{.target = currentTarget,
                                                       .extent = {passExtent.width, passExtent.height},
                                                       .scene = renderScene},
            }};
            Check(frame.Execute(passes).HasValue());
            Check(frame.Present().HasValue());

            if (currentTarget != activeTarget)
                resizedTarget = currentTarget;
        }
        Check(resizedTarget.IsValid());
        Check(viewport.RequestedExtent() == resizedExtent);
        Check(viewport.TextureView().textureId != activeTexture.textureId);
    }

    struct CallerOpenGLState {
        GLuint vertexArray{0};
        GLuint arrayBuffer{0};
        GLuint drawFramebuffer{0};
        GLuint readFramebuffer{0};
    };

    [[nodiscard]] CallerOpenGLState CreateCallerOpenGLState() {
        CallerOpenGLState state;
        glGenVertexArrays(1, &state.vertexArray);
        glGenBuffers(1, &state.arrayBuffer);
        glGenFramebuffers(1, &state.drawFramebuffer);
        glGenFramebuffers(1, &state.readFramebuffer);
        glBindVertexArray(state.vertexArray);
        glBindBuffer(GL_ARRAY_BUFFER, state.arrayBuffer);
        return state;
    }

    void ConfigureCallerOpenGLState(const CallerOpenGLState &state) {
        glViewport(7, 9, 111, 113);
        glClearColor(0.2F, 0.3F, 0.4F, 0.5F);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glEnable(GL_SCISSOR_TEST);
        glScissor(3, 5, 7, 11);
        glEnable(GL_BLEND);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT_AND_BACK);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_FALSE);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state.drawFramebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, state.readFramebuffer);
    }

    void CheckCallerOpenGLState(const CallerOpenGLState &state) {
        GLint drawFramebuffer = 0;
        GLint readFramebuffer = 0;
        std::array<GLint, 4> viewport{};
        GLint depthFunction = 0;
        std::array<GLint, 4> scissorBox{};
        std::array<GLboolean, 4> colorMask{};
        GLboolean depthMask = GL_TRUE;
        std::array<GLfloat, 4> clearColor{};
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        glGetIntegerv(GL_VIEWPORT, viewport.data());
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunction);
        glGetIntegerv(GL_SCISSOR_BOX, scissorBox.data());
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask.data());
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor.data());
        Check(drawFramebuffer == static_cast<GLint>(state.drawFramebuffer));
        Check(readFramebuffer == static_cast<GLint>(state.readFramebuffer));
        Check(viewport[0] == 7 && viewport[1] == 9 && viewport[2] == 111 && viewport[3] == 113);
        Check(glIsEnabled(GL_DEPTH_TEST) == GL_TRUE && depthFunction == GL_ALWAYS);
        Check(glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE && glIsEnabled(GL_BLEND) == GL_TRUE && glIsEnabled(GL_CULL_FACE) == GL_TRUE);
        Check(scissorBox[0] == 3 && scissorBox[1] == 5 && scissorBox[2] == 7 && scissorBox[3] == 11);
        Check(colorMask[0] == GL_FALSE && colorMask[1] == GL_FALSE && colorMask[2] == GL_FALSE && colorMask[3] == GL_FALSE);
        Check(depthMask == GL_FALSE);
        Check(std::fabs(clearColor[0] - 0.2F) < 0.001F && std::fabs(clearColor[3] - 0.5F) < 0.001F);
    }

    void RestoreDefaultOpenGLState() {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }

    void DestroyCallerOpenGLState(const CallerOpenGLState &state) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
        glDeleteBuffers(1, &state.arrayBuffer);
        glDeleteVertexArrays(1, &state.vertexArray);
        glDeleteFramebuffers(1, &state.drawFramebuffer);
        glDeleteFramebuffers(1, &state.readFramebuffer);
    }

    struct ViewportSceneFixture {
        Runtime::PrimitiveMeshCache meshCache{Runtime::PrimitiveMeshCacheLimits{}};
        std::vector<Runtime::PrimitiveMeshLease> meshLeases;
        std::vector<EditorViewportMeshResourceView> meshResources;
        std::vector<EditorViewportInstance> instances;
        std::array<RenderLight, 1> lights;

        [[nodiscard]] EditorViewportSceneView View() const noexcept {
            return {.camera = {}, .meshResources = meshResources, .instances = instances, .lights = lights};
        }
    };

    [[nodiscard]] ViewportSceneFixture CreateViewportScene() {
        ViewportSceneFixture fixture{
            .lights = {RenderLight{.kind = RenderLightKind::Directional,
                                   .direction = Math::Normalize(Math::Vec3{0.5F, -1.0F, -0.5F}),
                                   .color = {1.0F, 0.95F, 0.88F},
                                   .intensity = 2.0F}},
        };
        constexpr std::array primitiveTypes{Runtime::PrimitiveMeshType::Box,     Runtime::PrimitiveMeshType::Sphere,
                                            Runtime::PrimitiveMeshType::Capsule, Runtime::PrimitiveMeshType::Cylinder,
                                            Runtime::PrimitiveMeshType::Cone,    Runtime::PrimitiveMeshType::Plane,
                                            Runtime::PrimitiveMeshType::Quad};
        constexpr std::array positions{Math::Vec2{0, 0},         Math::Vec2{-1.0F, 0.7F}, Math::Vec2{0, 0.9F},    Math::Vec2{1.0F, 0.7F},
                                       Math::Vec2{-1.0F, -0.7F}, Math::Vec2{0, -0.9F},    Math::Vec2{1.0F, -0.7F}};
        for (std::size_t index = 0; index < primitiveTypes.size(); ++index) {
            auto acquired = fixture.meshCache.Acquire(Runtime::PrimitiveMeshDescriptor::Defaults(primitiveTypes[index]));
            Check(acquired.HasValue());
            Runtime::PrimitiveMeshLease lease = std::move(acquired).Value();
            const MeshData &mesh = lease.Data();
            const RenderMeshSourceHandle handle{lease.Id(), 1};
            fixture.meshResources.push_back({handle, mesh.vertices, mesh.indices, mesh.localBounds});
            const float scale = primitiveTypes[index] == Runtime::PrimitiveMeshType::Plane ? 0.08F : index == 0 ? 0.65F : 0.45F;
            fixture.instances.push_back(
                {handle,
                 Math::Transform{.translation = {positions[index].x, positions[index].y, 0}, .scale = {scale, scale, scale}}.ToMatrix(),
                 mesh.localBounds,
                 CoreDefaultMaterial,
                 {.tint = {0.12F, 0.72F, 1.0F}, .tintStrength = index == 0 ? 0.65F : 0.0F}});
            fixture.meshLeases.push_back(std::move(lease));
        }
        return fixture;
    }

    [[nodiscard]] RenderFrameScope ExecuteViewportFrame(RenderFrontend &frontend, const EditorViewportSceneView &scene,
                                                        const RenderTargetHandle target, const EditorViewportExtent extent,
                                                        const CallerOpenGLState &callerState) {
        auto begun = frontend.BeginFrame(FrameDescriptor{.frameNumber = 1, .outputExtent = {640, 480}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();
        const std::array passes{RenderPassDescriptor{
            .id = RenderPassId{1},
            .kind = RenderPassKind::Graphics,
            .staticMesh = StaticMeshPassDescriptor{.target = target,
                                                   .extent = {extent.width, extent.height},
                                                   .scene = RenderSceneView{ToRenderCamera(scene.camera), scene.meshResources,
                                                                            scene.instances, scene.lights}},
        }};
        ConfigureCallerOpenGLState(callerState);
        Check(frame.Execute(passes).HasValue());
        return frame;
    }

    void CheckViewportPixels(const EditorViewportTextureView view, const std::uint32_t width, const std::uint32_t height) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(view.textureId));
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        std::size_t coloredPixels = 0;
        for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
            const bool differsFromBackground = pixels[offset] > 20 || pixels[offset + 1] > 24 || pixels[offset + 2] > 32;
            coloredPixels += differsFromBackground ? 1 : 0;
        }
        Check(coloredPixels > 10000);
        const std::size_t center = (static_cast<std::size_t>(height / 2) * width + width / 2) * 4;
        Check(pixels[center] < 150 && pixels[center + 1] > 130 && pixels[center + 2] > 150);
    }

    TEST_CASE("Editor Viewport Open GL Smoke", "[integration][renderer][gpu]") {
        constexpr EditorViewportExtent extent{512, 384};
        Check(SDL_Init(SDL_INIT_VIDEO));
        Check(SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1));
        Check(SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24));
        SDL_Window *window = SDL_CreateWindow("Horo viewport smoke", 640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        Check(window != nullptr);

        HeadlessOpenGLPresentationPort presentationPort{*window};
        RenderBackendRegistry registry;
        Check(RegisterOpenGLRenderBackend(registry, presentationPort).HasValue());
        Check(registry.Seal().HasValue());
        auto frontendResult = RenderFrontend::Create(registry, RenderBackendId{"opengl"},
                                                     RenderBackendConfig{.requirePresentation = true,
                                                                         .enableValidation = false,
                                                                         .maxFramesInFlight = 2,
                                                                         .presentMode = PresentMode::Immediate});
        Check(frontendResult.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(frontendResult).Value();
        Check(gladLoadGL(SDL_GL_GetProcAddress) != 0);

        const CallerOpenGLState callerState = CreateCallerOpenGLState();
        EditorViewportRendererOpenGL viewport{*frontend};
        Check(viewport.Initialize().HasValue());
        ViewportSceneFixture fixture = CreateViewportScene();
        const EditorViewportSceneView scene = fixture.View();
        Check(frontend->AttachStaticMeshPassExecutor(viewport).HasValue());
        const RenderTargetHandle target = PrepareViewportTarget(viewport, *frontend, scene, extent);
        viewport.RequestExtent(extent);

        RenderFrameScope frame = ExecuteViewportFrame(*frontend, scene, target, extent, callerState);
        CheckCallerOpenGLState(callerState);
        RestoreDefaultOpenGLState();
        Check(viewport.IsReady());
        const EditorViewportTextureView textureView = viewport.TextureView();
        Check(textureView.IsValid());
        Check(textureView.v0 == 1.0F && textureView.v1 == 0.0F);
        CheckViewportPixels(textureView, extent.width, extent.height);
        Check(frame.Present().HasValue());
        CheckSeamlessResourceTransition(viewport, *frontend, scene, target, textureView);

        frontend->DetachStaticMeshPassExecutor(viewport);
        viewport.Shutdown();
        DestroyCallerOpenGLState(callerState);
        frontend.reset();
        SDL_DestroyWindow(window);
        SDL_Quit();
    }

}  // namespace Horo::Render::OpenGLSmokeTests
