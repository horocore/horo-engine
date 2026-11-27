#include "CameraCutNativeTestSupport.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "OpenGLBackendModule.h"
#include "editor/renderer/opengl/EditorGuiRendererOpenGL.h"
#include "editor/renderer/opengl/EditorViewportRendererOpenGL.h"
#include "editor/renderer/opengl/SdlOpenGLPresentationPort.h"
#include "renderer/RenderBackendDrainTestSupport.h"

#include <SDL3/SDL.h>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstring>
#include <glad/gl.h>
#include <imgui.h>
#include <iostream>
#include <memory>
#include <thread>
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
    using Tests::CameraCuts::ChangeNativeOutput;
    using Tests::CameraCuts::NativeOutputChange;
    using Tests::CameraCuts::RequireDisplayConfiguration;

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

    /** @brief Borrows the fixture's resource spans while copying its exact current camera and light revision. */
    [[nodiscard]] RenderSceneView ToSceneView(const EditorViewportSceneView &scene) noexcept {
        return {ToRenderCamera(scene.camera), scene.meshResources,        scene.instances, scene.lights,
                scene.lightSceneDomain,       scene.lightSnapshotRevision};
    }

    RenderTargetHandle PrepareViewportTarget(EditorViewportRendererOpenGL &viewport, RenderFrontend &frontend,
                                             const EditorViewportSceneView &scene, const EditorViewportExtent extent) {
        for (std::size_t attempt = 0; attempt < 4; ++attempt) {
            viewport.RequestExtent(extent);
            auto prepared = viewport.PrepareResources(frontend, ToSceneView(scene));
            Check(prepared.HasValue());
            Check(frontend.ProcessResourceRequests().HasValue());
            if (prepared.Value().has_value())
                return *prepared.Value();
        }
        Check(false);
        return {};
    }

    /** @brief Performs the smoke test's explicit synchronous native texture readback. */
    std::vector<std::uint8_t> ReadViewportPixels(const EditorViewportTextureView view, const std::uint32_t width,
                                                 const std::uint32_t height) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(view.textureId));
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        return pixels;
    }

    /** @brief Detects a stale pre-resize camera using the qualification scene's central box silhouette. */
    void CheckResizedCameraPixels(const EditorViewportTextureView view, const EditorViewportExtent extent, const RenderSceneView &scene) {
        const auto &box = scene.instances.front();
        const float halfWidth = box.localBounds.maximum.x * box.localToWorld.At(0, 0);
        const float frontDepth = scene.camera.position.z - box.localBounds.maximum.z * box.localToWorld.At(2, 2);
        const float pixelRadius = halfWidth * static_cast<float>(extent.height) /
                                  (2.0F * frontDepth * std::tan(scene.camera.projection.verticalFovRadians * 0.5F));
        const auto column = extent.width / 2 + static_cast<std::uint32_t>(std::ceil(pixelRadius)) + 2;
        REQUIRE(column < extent.width);
        const auto pixels = ReadViewportPixels(view, extent.width, extent.height);
        const auto offset = (static_cast<std::size_t>(extent.height / 2) * extent.width + column) * 4;
        const auto background = (static_cast<std::size_t>(extent.height - 1) * extent.width + extent.width - 1) * 4;
        const auto center = (static_cast<std::size_t>(extent.height / 2) * extent.width + extent.width / 2) * 4;
        CHECK((pixels[center] != pixels[background] || pixels[center + 1] != pixels[background + 1] ||
               pixels[center + 2] != pixels[background + 2]));
        // This pixel lies outside the destination box and inside its stale, closer-camera silhouette.
        CHECK(pixels[offset] == pixels[background]);
        CHECK(pixels[offset + 1] == pixels[background + 1]);
        CHECK(pixels[offset + 2] == pixels[background + 2]);
    }

    /** @brief Owns and presents exactly one transition frame after target and lighting admission. */
    void PresentTransitionFrame(RenderFrontend &frontend, const StaticMeshPassDescriptor &descriptor, const std::uint64_t number,
                                const FramebufferExtent outputExtent) {
        auto begun = frontend.BeginFrame(FrameDescriptor{.frameNumber = number, .outputExtent = outputExtent});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();
        const std::array passes{RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics, .staticMesh = descriptor}};
        Check(frame.Execute(passes).HasValue());
        Check(frame.Present().HasValue());
    }

    /** @brief Verifies that replacements keep the active viewport usable until publication. */
    void CheckSeamlessResourceTransition(EditorViewportRendererOpenGL &viewport, RenderFrontend &frontend,
                                         const EditorViewportSceneView &scene, const RenderTargetHandle activeTarget,
                                         const EditorViewportTextureView activeTexture, const RenderCameraView &currentCamera,
                                         const FramebufferExtent outputExtent) {
        constexpr EditorViewportExtent resizedExtent{384, 256};
        std::vector<EditorViewportMeshResourceView> replacementMeshes{scene.meshResources.begin(), scene.meshResources.end()};
        replacementMeshes.front().handle.generation = 2;
        std::vector<EditorViewportInstance> replacementInstances{scene.instances.begin(), scene.instances.end()};
        replacementInstances.front().mesh.generation = 2;
        auto replacementCamera = scene.camera;
        replacementCamera.position = currentCamera.position;
        replacementCamera.target = currentCamera.target;
        replacementCamera.up = currentCamera.up;
        const EditorViewportSceneView replacementScene{replacementCamera, replacementMeshes,      replacementInstances,
                                                       scene.lights,      scene.lightSceneDomain, scene.lightSnapshotRevision};
        const auto renderScene = ToSceneView(replacementScene);

        RenderTargetHandle resizedTarget;
        bool deferredGeneration{};
        RenderTargetHandle currentTarget = activeTarget;
        viewport.RequestExtent(resizedExtent);
        for (std::size_t attempt = 0; attempt < 6; ++attempt) {
            if (resizedTarget.IsValid())
                break;
            auto prepared = viewport.PrepareResources(frontend, renderScene);
            Check(prepared.HasValue() && prepared.Value().has_value());
            currentTarget = *prepared.Value();
            Check(frontend.ProcessResourceRequests().HasValue());

            // Match the application frame order: prepare the preceding request,
            // then let the panel publish this frame's request before pass execution.
            viewport.RequestExtent(resizedExtent);
            const EditorViewportExtent passExtent = viewport.RequestedExtent();
            Check(passExtent.IsValid());

            const StaticMeshPassDescriptor descriptor{.target = currentTarget,
                                                      .extent = {passExtent.width, passExtent.height},
                                                      .scene = renderScene};
            const auto lighting = viewport.PrepareLightingFrame(descriptor, {});
            if (lighting.HasError()) {
                Check(lighting.ErrorValue().code.Value() == LightCullingErrors::Pending.code.Value());
                deferredGeneration = true;
                continue;
            }

            PresentTransitionFrame(frontend, descriptor, 2 + attempt, outputExtent);

            if (currentTarget != activeTarget)
                resizedTarget = currentTarget;
        }
        Check(resizedTarget.IsValid());
        Check(deferredGeneration);
        Check(viewport.RequestedExtent() == resizedExtent);
        Check(viewport.TextureView().textureId != activeTexture.textureId);
        CheckResizedCameraPixels(viewport.TextureView(), resizedExtent, renderScene);
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
            return {.camera = {},
                    .meshResources = meshResources,
                    .instances = instances,
                    .lights = lights,
                    .lightSceneDomain = 1,
                    .lightSnapshotRevision = 1};
        }
    };

    [[nodiscard]] ViewportSceneFixture CreateViewportScene() {
        using enum Runtime::PrimitiveMeshType;
        ViewportSceneFixture fixture{
            .lights = {RenderLight{.kind = RenderLightKind::Directional,
                                   .direction = Math::Normalize(Math::Vec3{0.5F, -1.0F, -0.5F}),
                                   .color = {1.0F, 0.95F, 0.88F},
                                   .intensity = 2.0F,
                                   .stableIdentity = 101}},
        };
        constexpr std::array primitiveTypes{Box, Sphere, Capsule, Cylinder, Cone, Plane, Quad};
        constexpr std::array positions{Math::Vec2{0, 0},         Math::Vec2{-1.0F, 0.7F}, Math::Vec2{0, 0.9F},    Math::Vec2{1.0F, 0.7F},
                                       Math::Vec2{-1.0F, -0.7F}, Math::Vec2{0, -0.9F},    Math::Vec2{1.0F, -0.7F}};
        for (std::size_t index = 0; index < primitiveTypes.size(); ++index) {
            auto acquired = fixture.meshCache.Acquire(Runtime::PrimitiveMeshDescriptor::Defaults(primitiveTypes[index]));
            Check(acquired.HasValue());
            Runtime::PrimitiveMeshLease lease = std::move(acquired).Value();
            const MeshData &mesh = lease.Data();
            const RenderMeshSourceHandle handle{lease.Id(), 1};
            fixture.meshResources.emplace_back(handle, mesh.vertices, mesh.indices, mesh.localBounds);
            float scale = index == 0 ? 0.65F : 0.45F;
            if (primitiveTypes[index] == Plane)
                scale = 0.08F;
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
            .staticMesh = StaticMeshPassDescriptor{.target = target, .extent = {extent.width, extent.height}, .scene = ToSceneView(scene)},
        }};
        ConfigureCallerOpenGLState(callerState);
        Check(frame.Execute(passes).HasValue());
        return frame;
    }

    void CheckViewportPixels(const EditorViewportTextureView view, const std::uint32_t width, const std::uint32_t height) {
        const auto pixels = ReadViewportPixels(view, width, height);
        std::size_t coloredPixels = 0;
        for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
            const bool differsFromBackground = pixels[offset] > 20 || pixels[offset + 1] > 24 || pixels[offset + 2] > 32;
            coloredPixels += differsFromBackground ? 1 : 0;
        }
        Check(coloredPixels > 10000);
        const std::size_t center = (static_cast<std::size_t>(height / 2) * width + width / 2) * 4;
        Check(pixels[center] < 150 && pixels[center + 1] > 130 && pixels[center + 2] > 150);
    }

    /** @brief Builds actual GUI CPU draw data while resource admission is still open, before any native scope. */
    void PrepareGuiComposition(Editor::EditorGuiRendererOpenGL &gui, const EditorViewportTextureView view) {
        REQUIRE(gui.BeginFrame().HasValue());
        ImGui::NewFrame();
        ImGui::GetForegroundDrawList()->AddImage(static_cast<ImTextureID>(view.textureId), {0, 0}, {512, 384}, {view.u0, view.v0},
                                                 {view.u1, view.v1});
        ImGui::Render();
    }

    /** @brief Checks actual ImGui sampling into the primary backbuffer despite retained offscreen bindings. */
    void CheckGuiComposition(Editor::EditorGuiRendererOpenGL &gui, RenderFrameScope &frame, const EditorViewportTextureView view,
                             const CallerOpenGLState &callerState) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDrawBuffer(GL_NONE);
        ConfigureCallerOpenGLState(callerState);
        gui.BindViewportTexture(view);
        REQUIRE(frame.ComposeGui(gui).HasValue());
        CheckCallerOpenGLState(callerState);
        RestoreDefaultOpenGLState();
        GLint restoredDrawBuffer = 0;
        glGetIntegerv(GL_DRAW_BUFFER, &restoredDrawBuffer);
        CHECK(restoredDrawBuffer == GL_NONE);
        glDrawBuffer(GL_BACK);
        glReadBuffer(GL_BACK);
        std::array<std::uint8_t, 4> pixel{};
        glReadPixels(256, 480 - 192, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        CHECK(pixel[0] < 150);
        CHECK(pixel[1] > 130);
        CHECK(pixel[2] > 150);
        CHECK(glGetError() == GL_NO_ERROR);
    }

    /** @brief Covers initialization, invalid input, owned context restoration and foreign-thread denial before native drawing. */
    void InitializeGui(Editor::EditorGuiRendererOpenGL &gui, SDL_Window &window, HeadlessOpenGLPresentationPort &presentationPort) {
        CHECK(gui.BeginFrame().HasError());
        CHECK(gui.RenderDrawData({}).HasError());
        REQUIRE(gui.Initialize().HasValue());
        CHECK(gui.Initialize().HasError());
        CHECK(gui.CreateTexture({}).HasError());
        const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
        const auto uploaded = gui.CreateTexture({1, 1, white});
        REQUIRE(uploaded.HasValue());
        gui.DestroyTexture(uploaded.Value());
        gui.DestroyTexture(uploaded.Value());
        const auto context = SDL_GL_GetCurrentContext();
        REQUIRE(SDL_GL_MakeCurrent(&window, nullptr));
        REQUIRE(gui.BeginFrame().HasValue());
        CHECK(SDL_GL_GetCurrentContext() == context);
        CHECK(SDL_GL_GetCurrentWindow() == &window);
        bool foreignRejected = false;
        std::thread foreign([&] {
            foreignRejected = gui.BeginFrame().HasError();
        });
        foreign.join();
        CHECK(foreignRejected);
        CHECK(SDL_GL_GetCurrentContext() == context);
        REQUIRE(presentationPort.MakeCurrent().HasValue());
    }

    /** @brief Owns the SDL window beyond the lifetime of its borrowed renderer and GUI contexts. */
    using NativeViewportWindow = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;

    /** @brief Admits SDL video/display configuration before allocating the qualification window. */
    NativeViewportWindow CreateNativeViewportWindow(const NativeOutputChange outputChange) {
        Check(SDL_Init(SDL_INIT_VIDEO));
        RequireDisplayConfiguration(outputChange);
        Check(SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1));
        Check(SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24));
        NativeViewportWindow window{SDL_CreateWindow("Horo viewport smoke", 640, 480,
                                                     SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE),
                                    SDL_DestroyWindow};
        REQUIRE(window != nullptr);
        return window;
    }

    /** @brief Creates the native frontend while keeping the borrowed presentation port with the test owner. */
    std::unique_ptr<RenderFrontend> CreateViewportFrontend(HeadlessOpenGLPresentationPort &presentationPort) {
        RenderBackendRegistry registry;
        Check(RegisterOpenGLRenderBackend(registry, presentationPort).HasValue());
        Check(registry.Seal().HasValue());
        auto frontendResult = RenderFrontend::Create(registry, RenderBackendId{"opengl"},
                                                     RenderBackendConfig{.requirePresentation = true,
                                                                         .enableValidation = false,
                                                                         .maxFramesInFlight = 2,
                                                                         .presentMode = PresentMode::Immediate});
        Check(frontendResult.HasValue());
        return std::move(frontendResult).Value();
    }

    /** @brief Records the real device behind the current qualification context. */
    void RecordOpenGLDevice() {
        const auto *renderer = glGetString(GL_RENDERER);
        const auto *version = glGetString(GL_VERSION);
        REQUIRE(renderer != nullptr);
        REQUIRE(version != nullptr);
        std::cout << "OpenGL renderer " << reinterpret_cast<const char *>(renderer) << ", version "
                  << reinterpret_cast<const char *>(version) << '\n';
    }

    /** @brief Verifies viewport readback and GUI composition within one fully presented initial frame. */
    EditorViewportTextureView CheckInitialViewportFrame(EditorViewportRendererOpenGL &viewport, RenderFrontend &frontend,
                                                        Editor::EditorGuiRendererOpenGL &gui, const EditorViewportSceneView &scene,
                                                        const RenderTargetHandle target, const EditorViewportExtent extent,
                                                        const CallerOpenGLState &callerState) {
        viewport.RequestExtent(extent);

        const EditorViewportTextureView textureView = viewport.TextureView();
        Check(textureView.IsValid());
        PrepareGuiComposition(gui, textureView);
        RenderFrameScope frame = ExecuteViewportFrame(frontend, scene, target, extent, callerState);
        CheckCallerOpenGLState(callerState);
        RestoreDefaultOpenGLState();
        Check(viewport.IsReady());
        Check(textureView.v0 == 1.0F && textureView.v1 == 0.0F);
        CheckViewportPixels(textureView, extent.width, extent.height);
        CheckGuiComposition(gui, frame, textureView, callerState);
        Check(frame.Present().HasValue());
        return textureView;
    }

    /** @brief Verifies one complete cinematic camera transition across the requested native presentation change. */
    void QualifyCameraPresentation(const Cinematic::CameraBlendCurve curve, const NativeOutputChange outputChange) {
        constexpr EditorViewportExtent extent{512, 384};
        auto window = CreateNativeViewportWindow(outputChange);

        HeadlessOpenGLPresentationPort presentationPort{*window};
        auto frontend = CreateViewportFrontend(presentationPort);
        Check(gladLoadGL(SDL_GL_GetProcAddress) != 0);
        RecordOpenGLDevice();

        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        Editor::EditorGuiRendererOpenGL gui{*window, SDL_GL_GetCurrentContext(), *frontend};
        InitializeGui(gui, *window, presentationPort);
        const CallerOpenGLState callerState = CreateCallerOpenGLState();
        EditorViewportRendererOpenGL viewport{*frontend};
        Horo::Editor::EditorViewportLightingProfile lighting;
        lighting.requested = LightPreparationMode::BaselineForwardCpu;
        Check(viewport.RequestLightingProfile(lighting, {}).HasValue());
        Check(viewport.Initialize().HasValue());
        ViewportSceneFixture fixture = CreateViewportScene();
        auto scene = fixture.View();
        Tests::CameraCuts::NativeCameraPlayback cinematic{curve};
        const auto cinematicCamera = cinematic.Advance(0);
        const float expectedInitialZ = curve == Cinematic::CameraBlendCurve::Linear ? 3.6666667F : 3.5185185F;
        CHECK(cinematicCamera.position.z == Catch::Approx(expectedInitialZ));
        scene.camera.position = cinematicCamera.position;
        scene.camera.target = cinematicCamera.target;
        scene.camera.up = cinematicCamera.up;
        Check(frontend->AttachStaticMeshPassExecutor(viewport).HasValue());
        const RenderTargetHandle target = PrepareViewportTarget(viewport, *frontend, scene, extent);
        const auto textureView = CheckInitialViewportFrame(viewport, *frontend, gui, scene, target, extent, callerState);
        const auto outputExtent = ChangeNativeOutput(*window, *frontend, outputChange);
        CHECK(cinematic.Position() == 6);
        const auto resizedCamera = cinematic.Advance(2);
        CHECK(resizedCamera.position.z == 5.0F);
        CHECK(cinematic.Position() == 8);
        CheckSeamlessResourceTransition(viewport, *frontend, scene, target, textureView, resizedCamera, outputExtent);
        CHECK(cinematicCamera.position.z == Catch::Approx(expectedInitialZ));

        frontend->DetachStaticMeshPassExecutor(viewport);
        // Keep the owner and current context alive until the actual closing fence drains.
        Horo::Render::Test::RequireDrainedOwner(*frontend);
        viewport.Shutdown();
        gui.Shutdown();
        gui.Shutdown();
        ImGui::DestroyContext();
        DestroyCallerOpenGLState(callerState);
        frontend.reset();
        window.reset();
        SDL_Quit();
    }

    TEST_CASE("OpenGL presents text from a replacement atlas after the bootstrap font", "[integration][renderer][gpu][font-atlas]") {
        auto window = CreateNativeViewportWindow(NativeOutputChange::None);
        HeadlessOpenGLPresentationPort port{*window};
        auto frontend = CreateViewportFrontend(port);
        REQUIRE(gladLoadGL(SDL_GL_GetProcAddress) != 0);
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        Editor::EditorGuiRendererOpenGL gui{*window, SDL_GL_GetCurrentContext(), *frontend};
        REQUIRE(gui.Initialize().HasValue());

        constexpr FramebufferExtent output{640, 480};
        for (std::uint64_t frameNumber = 1; frameNumber <= 3; ++frameNumber) {
            REQUIRE(gui.BeginFrame().HasValue());
            using Atlas = std::unique_ptr<ImFontAtlas, decltype(&IM_DELETE<ImFontAtlas>)>;
            Atlas previous{nullptr, &IM_DELETE<ImFontAtlas>};
            if (frameNumber != 1) {
                Atlas candidate{IM_NEW(ImFontAtlas)(), &IM_DELETE<ImFontAtlas>};
                ImFontConfig config;
                config.RasterizerDensity = static_cast<float>(frameNumber);
                auto *font = candidate->AddFontDefault(&config);
                REQUIRE(font);
                REQUIRE(candidate->Build());
                REQUIRE(Editor::ReplaceEditorFontAtlas(gui, *candidate).HasValue());
                CHECK(candidate->TexID != io.Fonts->TexID);
                previous.reset(std::exchange(io.Fonts, candidate.release()));
                io.FontDefault = font;
            }
            ImGui::NewFrame();
            previous.reset();
            ImGui::GetForegroundDrawList()->AddText({12, 12}, IM_COL32_WHITE, "Atlas replacement");
            ImGui::Render();
            auto begun = frontend->BeginFrame({frameNumber, output});
            REQUIRE(begun.HasValue());
            auto frame = std::move(begun).Value();
            const std::array passes{
                RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics, .primaryOutput = PrimaryOutputAttachment{}}};
            const auto drawn = frame.Execute(passes);
            INFO((drawn.HasError() ? drawn.ErrorValue().message : ""));
            REQUIRE(drawn.HasValue());
            const auto composed = frame.ComposeGui(gui);
            INFO((composed.HasError() ? composed.ErrorValue().message : ""));
            REQUIRE(composed.HasValue());
            REQUIRE(frame.Present().HasValue());
        }
        Horo::Render::Test::RequireDrainedOwner(*frontend);
        gui.Shutdown();
        ImGui::DestroyContext();
        frontend.reset();
        window.reset();
        SDL_Quit();
    }

    TEST_CASE("Editor Viewport Open GL Smoke", "[integration][renderer][gpu][cinematic][resize]") {
        const auto curve = GENERATE(Cinematic::CameraBlendCurve::Linear, Cinematic::CameraBlendCurve::SmoothStep);
        QualifyCameraPresentation(curve, NativeOutputChange::Resize);
    }

    /** @brief Uses the product viewport execution seam to submit or observe the native crop. */
    void RenderCaptureFrame(EditorViewportRendererOpenGL &viewport, RenderFrontend &frontend, const StaticMeshPassDescriptor &pass,
                            const std::uint64_t frameNumber) {
        auto begun = frontend.BeginFrame({.frameNumber = frameNumber, .outputExtent = {640, 480}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        const std::array passes{RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics, .staticMesh = pass}};
        REQUIRE(viewport.ExecutePreparedFrame(frame, passes).HasValue());
        REQUIRE(frame.Present().HasValue());
    }

    /** @brief Compares the top-left crop with the bottom-left native texture rows. */
    void CheckOpenGLCrop(const Editor::EditorViewportImageArtifact &image, const EditorViewportTextureView view,
                         const EditorViewportExtent extent) {
        const auto native = ReadViewportPixels(view, extent.width, extent.height);
        const auto &region = image.region;
        constexpr auto channels = Editor::EditorViewportImageRegion::Channels;
        REQUIRE(image.rgba.size() == region.Bytes());
        const auto rowBytes = std::size_t{region.width} * channels;
        bool hasDifferentRows = false;
        for (std::uint32_t row = 0; row < region.height; ++row) {
            const auto sourceRow = extent.height - 1 - region.y - row;
            const auto offset = (std::size_t{sourceRow} * extent.width + region.x) * channels;
            const auto *pixels = image.rgba.data() + row * rowBytes;
            CHECK(std::memcmp(pixels, native.data() + offset, rowBytes) == 0);
            hasDifferentRows |= std::memcmp(pixels, image.rgba.data(), rowBytes) != 0;
        }
        CHECK(hasDifferentRows);
    }

    TEST_CASE("OpenGL publishes an authorized top-left viewport crop and withdraws it on revocation",
              "[integration][renderer][gpu][image-capture]") {
        constexpr EditorViewportExtent extent{128, 128};
        auto window = CreateNativeViewportWindow(NativeOutputChange::None);
        HeadlessOpenGLPresentationPort port{*window};
        auto frontend = CreateViewportFrontend(port);
        REQUIRE(gladLoadGL(SDL_GL_GetProcAddress) != 0);
        bool authorized = true;
        EditorViewportRendererOpenGL viewport{*frontend};
        Editor::EditorViewportLightingProfile lighting{.requested = LightPreparationMode::BaselineForwardCpu};
        REQUIRE(viewport.RequestLightingProfile(lighting, {}).HasValue());
        REQUIRE(viewport.Initialize().HasValue());
        auto fixture = CreateViewportScene();
        const auto scene = fixture.View();
        REQUIRE(frontend->AttachStaticMeshPassExecutor(viewport).HasValue());
        const auto target = PrepareViewportTarget(viewport, *frontend, scene, extent);
        const StaticMeshPassDescriptor pass{.target = target, .extent = {extent.width, extent.height}, .scene = ToSceneView(scene)};
        constexpr Editor::EditorViewportImageRegion region{19, 17, 80, 74};
        viewport.RequestExtent(extent);
        const auto request = viewport.RequestImageCapture(region, [&authorized] {
            return authorized;
        });
        REQUIRE(request.HasValue());
        RenderCaptureFrame(viewport, *frontend, pass, 1);
        glFinish();  // Test-only wait for the async pixel buffer.
        viewport.RequestExtent(extent);
        RenderCaptureFrame(viewport, *frontend, pass, 2);

        const auto image = viewport.ReadImageCapture(request.Value());
        REQUIRE(image);
        REQUIRE(image->phase == Editor::EditorViewportImagePhase::Ready);
        REQUIRE(image->artifact);
        REQUIRE(image->artifact->region == region);
        CheckOpenGLCrop(*image->artifact, viewport.TextureView(), extent);
        REQUIRE_FALSE(image->publication.expired());
        authorized = false;
        const auto withdrawn = viewport.ReadImageCapture(request.Value());
        REQUIRE(withdrawn);
        CHECK(withdrawn->phase == Editor::EditorViewportImagePhase::Cancelled);
        CHECK_FALSE(withdrawn->artifact);
        CHECK(image->publication.expired());

        frontend->DetachStaticMeshPassExecutor(viewport);
        Horo::Render::Test::RequireDrainedOwner(*frontend);
        viewport.Shutdown();
        frontend.reset();
        window.reset();
        SDL_Quit();
    }

    TEST_CASE("OpenGL cinematic camera follows the native fullscreen output", "[integration][renderer][gpu][cinematic][fullscreen]") {
        const auto curve = GENERATE(Cinematic::CameraBlendCurve::Linear, Cinematic::CameraBlendCurve::SmoothStep);
        QualifyCameraPresentation(curve, NativeOutputChange::Fullscreen);
    }

    TEST_CASE("OpenGL cinematic camera follows a second native display", "[integration][renderer][gpu][cinematic][display]") {
        const auto curve = GENERATE(Cinematic::CameraBlendCurve::Linear, Cinematic::CameraBlendCurve::SmoothStep);
        QualifyCameraPresentation(curve, NativeOutputChange::SecondDisplay);
    }

    /** @brief Verifies the output is a weighted composition where two real camera silhouettes differ. */
    void CheckCrossFadePixels(const std::vector<std::uint8_t> &source, const std::vector<std::uint8_t> &destination,
                              const std::vector<std::uint8_t> &mixed) {
        REQUIRE(source.size() == destination.size());
        REQUIRE(source.size() == mixed.size());
        std::size_t informativeChannels{};
        int maximumDifference{};
        for (std::size_t index = 0; index < source.size(); ++index) {
            if (std::abs(int{source[index]} - int{destination[index]}) < 16)
                continue;
            ++informativeChannels;
            maximumDifference = std::max(maximumDifference, std::abs(int{mixed[index]} * 2 - int{source[index]} - int{destination[index]}));
        }
        CHECK(informativeChannels > 100);
        CHECK(maximumDifference <= 2);
    }

    TEST_CASE("OpenGL viewport composites two independent camera rasters in the sampled editor output",
              "[integration][renderer][gpu][cross-fade]") {
        constexpr EditorViewportExtent extent{256, 192};
        auto window = CreateNativeViewportWindow(NativeOutputChange::None);
        HeadlessOpenGLPresentationPort presentationPort{*window};
        auto frontend = CreateViewportFrontend(presentationPort);
        REQUIRE(gladLoadGL(SDL_GL_GetProcAddress) != 0);
        EditorViewportRendererOpenGL viewport{*frontend};
        Horo::Editor::EditorViewportLightingProfile lighting;
        lighting.requested = LightPreparationMode::BaselineForwardCpu;
        REQUIRE(viewport.RequestLightingProfile(lighting, {}).HasValue());
        REQUIRE(viewport.Initialize().HasValue());
        const auto fixture = CreateViewportScene();
        const auto scene = fixture.View();
        const auto destination = ToSceneView(scene);
        auto source = destination;
        source.camera.position.z = 3.0F;
        REQUIRE(viewport.PrepareCameraCrossFadeFrame(Editor::EditorViewportCameraCrossFade{source.camera, 0.5F, 1}).HasError());
        REQUIRE(viewport.EnableCameraCrossFade().HasValue());
        CHECK(viewport.CameraCrossFadeCapacity().availableViews == 0);
        REQUIRE(frontend->AttachStaticMeshPassExecutor(viewport).HasValue());
        const auto target = PrepareViewportTarget(viewport, *frontend, scene, extent);
        const auto capacity = viewport.CameraCrossFadeCapacity();
        REQUIRE(capacity.availableViews == 2);
        viewport.RequestExtent(extent);
        viewport.RequestGrid({.visible = false});
        constexpr FramebufferExtent output{640, 480};
        auto pass = StaticMeshPassDescriptor{.target = target,
                                             .extent = {extent.width, extent.height},
                                             .scene = source,
                                             .clearColor = {0.0F, 0.0F, 0.0F, 1.0F}};
        PresentTransitionFrame(*frontend, pass, 1, output);
        const auto sourcePixels = ReadViewportPixels(viewport.TextureView(), extent.width, extent.height);
        pass.scene = destination;
        PresentTransitionFrame(*frontend, pass, 2, output);
        const auto destinationPixels = ReadViewportPixels(viewport.TextureView(), extent.width, extent.height);
        REQUIRE(viewport.PrepareCameraCrossFadeFrame(Editor::EditorViewportCameraCrossFade{source.camera, 0.5F, capacity.generation})
                    .HasValue());
        PresentTransitionFrame(*frontend, pass, 3, output);
        CheckCrossFadePixels(sourcePixels, destinationPixels, ReadViewportPixels(viewport.TextureView(), extent.width, extent.height));
        REQUIRE(viewport.PrepareCameraCrossFadeFrame(Editor::EditorViewportCameraCrossFade{source.camera, 0.5F, capacity.generation + 1})
                    .HasError());
        PresentTransitionFrame(*frontend, pass, 4, output);
        CHECK(ReadViewportPixels(viewport.TextureView(), extent.width, extent.height) == destinationPixels);
        frontend->DetachStaticMeshPassExecutor(viewport);
        Horo::Render::Test::RequireDrainedOwner(*frontend);
        viewport.Shutdown();
        frontend.reset();
        window.reset();
        SDL_Quit();
    }

}  // namespace Horo::Render::OpenGLSmokeTests
