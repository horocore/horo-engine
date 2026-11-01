#include "EditorViewportRendererOpenGL.h"

#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "OpenGLProgramValidation.h"
#include "OpenGLStateSnapshot.h"
#include "OpenGLViewportResourceBridge.h"
#include "OpenGLViewportShaders.h"
#include "RenderFrontendResourceAccess.h"
#include "editor/renderer/EditorRenderMemoryScopes.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "editor/renderer/grid/EditorViewportGridGeometry.h"
#include "editor/screens/workspace/panels/viewport/visualizers/light/LightVisualizerGeometry.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <glad/gl.h>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        constexpr std::uint32_t maxViewportDimension = 8192;

        [[nodiscard]] Error MakeViewportError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] Result<std::uint32_t> CompileShader(const std::uint32_t type, const char *source) {
            const std::uint32_t shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);

            GLint compiled = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled == GL_TRUE) {
                return Result<std::uint32_t>::Success(shader);
            }

            std::array<char, 4096> log{};
            glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
            glDeleteShader(shader);
            return Result<std::uint32_t>::Failure(MakeViewportError(RendererErrors::ViewportShaderCompileFailed,
                                                                    "Viewport shader compilation failed: " + std::string{log.data()}));
        }

    }  // namespace

    /** @brief Owns both unpublished programs and their exact uniform locations on the current GL context. */
    struct EditorViewportRendererOpenGL::ProgramGeneration final : Render::IShaderHotReloadCandidate {
        EditorViewportRendererOpenGL *owner{};
        Render::ShaderHotReloadTargetState expected;
        std::uint32_t gridProgram{};
        std::array<std::uint32_t, 3> buffers{}, bufferSlots{};
        std::uint32_t textureUnit{7};
        std::array<std::size_t, 3> bufferBytes{};
        std::array<Render::MaterialBindingLayout, 3> layouts;
        bool reflected{};
        std::uint32_t program{};
        std::uint32_t shadowProgram{};
        UniformLocations uniforms;

        ~ProgramGeneration() {
            if (gridProgram)
                glDeleteProgram(gridProgram);
            for (const auto buffer : buffers)
                if (buffer)
                    glDeleteBuffers(1, &buffer);
            if (program != 0)
                glDeleteProgram(program);
            if (shadowProgram != 0)
                glDeleteProgram(shadowProgram);
        }
    };

#include "EditorViewportOpenGLHotReload.inl"

    /** @copydoc EditorViewportRendererOpenGL::AdoptProgram */
    void EditorViewportRendererOpenGL::AdoptProgram(ProgramGeneration &candidate) noexcept {
        std::swap(program_, candidate.program);
        std::swap(shadowProgram_, candidate.shadowProgram);
        std::swap(uniforms_, candidate.uniforms);
        std::swap(gridProgram_, candidate.gridProgram);
        std::swap(reloadBuffers_, candidate.buffers);
        std::swap(reloadBufferSlots_, candidate.bufferSlots);
        std::swap(reloadTextureUnit_, candidate.textureUnit);
        std::swap(reloadBufferBytes_, candidate.bufferBytes);
        std::swap(reloadLayouts_, candidate.layouts);
        std::swap(reflectedProgram_, candidate.reflected);
    }

    /** @copydoc EditorViewportRendererOpenGL::EditorViewportRendererOpenGL */
    EditorViewportRendererOpenGL::EditorViewportRendererOpenGL(Render::RenderFrontend &frontend) noexcept
        : frontend_(&frontend), resources_(frontend, {.depthFormat = Render::RenderTextureFormat::Depth24Stencil8,
                                                      .depthAspect = Render::RenderTextureAspect::DepthStencil,
                                                      .resolveImage = &OpenGLViewportResourceBridge::EditorImageIdentity}) {}

    /** @copydoc EditorViewportRendererOpenGL::~EditorViewportRendererOpenGL */
    EditorViewportRendererOpenGL::~EditorViewportRendererOpenGL() {
        Shutdown();
    }

    /** @copydoc EditorViewportRendererOpenGL::Initialize */
    Result<void> EditorViewportRendererOpenGL::Initialize() {
        if (initialized_ || frontend_ == nullptr) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportAlreadyInitialized, "Editor viewport renderer is already initialized."));
        }
        ProgramGeneration candidate;
        if (const Result<void> program = CreateProgram(candidate); program.HasError()) {
            Shutdown();
            return program;
        }
        if (const Result<void> shadowResources = CreateShadowProgram(candidate); shadowResources.HasError()) {
            Shutdown();
            return shadowResources;
        }
        if (const auto geometry = CreateGridGeometry(); geometry.HasError()) {
            Shutdown();
            return geometry;
        }
        AdoptProgram(candidate);
        ++liveGeneration_;
        initialized_ = true;
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererOpenGL::CreateGridGeometry() {
        GLint previousVertexArray = 0;
        GLint previousArrayBuffer = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
        glGenVertexArrays(1, &gridVertexArray_);
        constexpr std::size_t gridBufferSize = sizeof(Math::Vec3) * (ViewportGridGeometry::MaxRegularVertices + 4);
        const std::vector<std::byte> emptyGrid(gridBufferSize);
        auto gridBuffer = frontend_->CreateBuffer(RenderMemoryScopes::ViewportResources,
                                                  {.byteSize = gridBufferSize,
                                                   .usage = Render::RenderBufferUsage::Vertex | Render::RenderBufferUsage::CopyDestination,
                                                   .access = Render::RenderBufferAccess::HostVisible},
                                                  std::span<const std::byte>{emptyGrid});
        if (gridBuffer.HasError()) {
            return Result<void>::Failure(gridBuffer.ErrorValue());
        }
        gridVertexBufferHandle_ = gridBuffer.Value().handle;
        const auto processed = frontend_->ProcessResourceRequests();
        if (processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        const auto completed = frontend_->ResourceOperationResult(gridBuffer.Value().operation);
        if (completed.HasError())
            return Result<void>::Failure(completed.ErrorValue());
        const auto nativeBuffer = OpenGLViewportResourceBridge::ResolveBuffer(*frontend_, gridVertexBufferHandle_);
        if (nativeBuffer.HasError())
            return Result<void>::Failure(nativeBuffer.ErrorValue());
        gridVertexBuffer_ = nativeBuffer.Value();
        glBindVertexArray(gridVertexArray_);
        glBindBuffer(GL_ARRAY_BUFFER, gridVertexBuffer_);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(Math::Vec3)), nullptr);
        glEnableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glVertexAttrib3f(1, 0.0F, 1.0F, 0.0F);
        glDisableVertexAttribArray(2);
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousArrayBuffer));
        glBindVertexArray(static_cast<GLuint>(previousVertexArray));
        if (gridVertexArray_ == 0 || gridVertexBuffer_ == 0) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportGeometryCreationFailed, "Failed to create viewport grid geometry."));
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererOpenGL::Shutdown */
    void EditorViewportRendererOpenGL::Shutdown() noexcept {
        if (gridProgram_)
            glDeleteProgram(std::exchange(gridProgram_, 0));
        for (const auto buffer : reloadBuffers_)
            if (buffer)
                glDeleteBuffers(1, &buffer);
        reloadBuffers_ = {};
        reflectedProgram_ = false;

        shadowCache_ = {};
        resources_.Shutdown();
        if (gridVertexBufferHandle_.IsValid())
            static_cast<void>(frontend_->ReleaseBuffer(gridVertexBufferHandle_));
        gridVertexBufferHandle_ = {};
        gridVertexBuffer_ = 0;
        if (gridVertexArray_ != 0) {
            glDeleteVertexArrays(1, &gridVertexArray_);
            gridVertexArray_ = 0;
        }
        if (program_ != 0) {
            glDeleteProgram(program_);
            program_ = 0;
        }
        if (shadowProgram_ != 0) {
            glDeleteProgram(shadowProgram_);
            shadowProgram_ = 0;
        }
        uniforms_ = {};
        requestedExtent_ = {};
        gridOptions_ = {};
        lightVisualizerOptions_ = {};
        initialized_ = false;
    }

    Result<void> EditorViewportRendererOpenGL::RequestDirectionalShadowProfile(const Render::DirectionalShadowProfile &profile) {
        return resources_.RequestDirectionalShadowProfile(profile);
    }

    /** @copydoc EditorViewportRendererOpenGL::RequestExtent */
    void EditorViewportRendererOpenGL::RequestExtent(const EditorViewportExtent extent) noexcept {
        requestedExtent_.width = std::min(extent.width, maxViewportDimension);
        requestedExtent_.height = std::min(extent.height, maxViewportDimension);
    }

    /** @copydoc EditorViewportRendererOpenGL::PrepareResources */
    Result<std::optional<Render::RenderTargetHandle>> EditorViewportRendererOpenGL::PrepareResources(Render::RenderFrontend &frontend,
                                                                                                     const Render::RenderSceneView &scene) {
        if (!initialized_ || frontend_ != &frontend) {
            return Result<std::optional<Render::RenderTargetHandle>>::Failure(
                MakeViewportError(RendererErrors::ViewportNotInitialized, "Viewport resource owner is not initialized."));
        }
        // Resource preparation runs before the panel draws. Consume the preceding
        // UI frame's request here so the current frame can publish a fresh request
        // that survives pass execution and advances an asynchronous resize next frame.
        return resources_.Prepare(scene, std::exchange(requestedExtent_, EditorViewportExtent{}));
    }

    /** @copydoc EditorViewportRendererOpenGL::RequestGrid */
    void EditorViewportRendererOpenGL::RequestGrid(const EditorViewportGridOptions &options) noexcept {
        gridOptions_ = options;
        if (!std::isfinite(gridOptions_.targetMinorSpacingPixels) || gridOptions_.targetMinorSpacingPixels <= 0.0F)
            gridOptions_.targetMinorSpacingPixels = 48.0F;
    }

    /** @copydoc EditorViewportRendererOpenGL::RequestLightVisualizer */
    void EditorViewportRendererOpenGL::RequestLightVisualizer(const EditorViewportLightVisualizerOptions &options) noexcept {
        lightVisualizerOptions_ = options;
    }

    /** @copydoc EditorViewportRendererOpenGL::ExecuteStaticMeshPass */
    Result<void> EditorViewportRendererOpenGL::ExecuteStaticMeshPass(const Render::StaticMeshPassDescriptor &descriptor) {
        if (!initialized_) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportNotInitialized, "Viewport renderer is not initialized."));
        }
        // A panel must request an extent every UI frame. PrepareResources consumes
        // the request at the next frame boundary, so hidden/inactive tabs still stop
        // rendering without starving a resize that needs multiple boundary advances.
        const EditorViewportExtent extentRequest = requestedExtent_;
        if (!extentRequest.IsValid())
            return Result<void>::Success();
        const EditorViewportExtent allocatedExtent = resources_.AllocatedExtent();
        const EditorViewportExtent renderExtent = allocatedExtent.IsValid() ? allocatedExtent : extentRequest;
        if (const Result<void> valid = ValidatePassRequest(descriptor, renderExtent); valid.HasError())
            return valid;
        const float aspect = static_cast<float>(allocatedExtent.width) / static_cast<float>(allocatedExtent.height);
        const auto shadow = Render::BuildDirectionalShadowFrame(descriptor.scene, aspect, Math::ClipDepthRange::NegativeOneToOne,
                                                                resources_.ActiveDirectionalShadowProfile());
        if (shadow.HasError())
            return Result<void>::Failure(shadow.ErrorValue());
        const OpenGLStateSnapshot stateSnapshot(reloadTextureUnit_, reloadBufferSlots_);
        return RenderViewportPass(descriptor, shadow.Value(), aspect);
    }

    Result<void> EditorViewportRendererOpenGL::ValidatePassRequest(const Render::StaticMeshPassDescriptor &descriptor,
                                                                   const EditorViewportExtent requestedExtent) const {
        if (!descriptor.IsValid() || descriptor.extent.width != requestedExtent.width ||
            descriptor.extent.height != requestedExtent.height) {
            return Result<void>::Failure(MakeViewportError(RendererErrors::ViewportInvalidScene, "Editor viewport scene data is invalid."));
        }
        if (resources_.Target().IsValid() && resources_.Target() != descriptor.target) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportStaleTarget, "Viewport pass references a stale render target."));
        }
        if (const EditorViewportExtent allocatedExtent = resources_.AllocatedExtent();
            requestedExtent.width != allocatedExtent.width || requestedExtent.height != allocatedExtent.height)
            return Result<void>::Failure(MakeViewportError(RendererErrors::ViewportStaleTarget, "Viewport target extent is not ready."));
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererOpenGL::RenderViewportPass(const Render::StaticMeshPassDescriptor &descriptor,
                                                                  const std::optional<EditorViewportDirectionalShadowView> &shadow,
                                                                  const float aspect) {
        if (shadow.has_value()) {
            if (const Result<void> renderedShadow = DrawDirectionalShadowMap(descriptor.scene, *shadow); renderedShadow.HasError()) {
                return renderedShadow;
            }
        }
        if (const Result<void> bound = OpenGLViewportResourceBridge::BindRenderTarget(*frontend_, resources_.Target()); bound.HasError())
            return bound;
        const EditorViewportExtent allocatedExtent = resources_.AllocatedExtent();
        glViewport(0, 0, static_cast<GLsizei>(allocatedExtent.width), static_cast<GLsizei>(allocatedExtent.height));
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glClearColor(descriptor.clearColor.red, descriptor.clearColor.green, descriptor.clearColor.blue, descriptor.clearColor.alpha);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(program_);
        if (const Result<void> boundShadow =
                OpenGLViewportResourceBridge::BindTexture(*frontend_, resources_.ShadowTextureView(), reloadTextureUnit_);
            boundShadow.HasError())
            return boundShadow;
        reloadParameters_.SetScene(descriptor.scene, shadow, Render::ShaderTargetBackend::OpenGL);
        if (!reflectedProgram_)
            UploadLighting(descriptor.scene, shadow);
        if (const Result<void> grid = DrawGrid(descriptor.scene.camera, aspect, static_cast<float>(allocatedExtent.height));
            grid.HasError()) {
            return grid;
        }
        if (reflectedProgram_)
            glUseProgram(program_);
        if (const Result<void> meshes = DrawSceneMeshes(descriptor.scene, aspect); meshes.HasError())
            return meshes;
        if (const Result<void> visualizer = DrawLightVisualizer(descriptor.scene.camera, aspect); visualizer.HasError())
            return visualizer;
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererOpenGL::DrawSceneMeshes(const Render::RenderSceneView &scene, const float aspect) const {
        for (const Render::RenderStaticMeshInstance &instance : scene.instances) {
            const auto mesh = resources_.FindMesh(instance.mesh.id.value);
            if (!mesh.has_value()) {
                return Result<void>::Failure(
                    MakeViewportError(RendererErrors::ViewportStaleMeshResource, "Viewport instance references a stale mesh resource."));
            }
            if (const Result<void> bound = OpenGLViewportResourceBridge::BindMesh(*frontend_, mesh->mesh); bound.HasError())
                return bound;
            const Result<Math::Mat4> mvp =
                BuildRenderMvp(scene.camera, instance.localToWorld, aspect, Math::ClipDepthRange::NegativeOneToOne);
            if (mvp.HasError())
                return Result<void>::Failure(mvp.ErrorValue());
            if (reflectedProgram_) {
                auto parameters = reloadParameters_;
                parameters.mvp = mvp.Value();
                parameters.model = instance.localToWorld;
                parameters.selection = {instance.presentation.tint.x, instance.presentation.tint.y, instance.presentation.tint.z,
                                        instance.presentation.tintStrength};
                std::array<std::byte, 4096> bytes{};
                const auto packed = parameters.PackLit(reloadLayouts_[0], bytes);
                if (packed.HasError())
                    return Result<void>::Failure(packed.ErrorValue());
                if (const auto uploaded = UploadReload(0, std::span{bytes}.first(packed.Value())); uploaded.HasError())
                    return uploaded;
            } else {
                glUniformMatrix4fv(uniforms_.mvp, 1, GL_FALSE, mvp.Value().values.data());
                glUniformMatrix4fv(uniforms_.model, 1, GL_FALSE, instance.localToWorld.values.data());
                glUniform3f(uniforms_.selectionColor, instance.presentation.tint.x, instance.presentation.tint.y,
                            instance.presentation.tint.z);
                glUniform1f(uniforms_.selectionStrength, instance.presentation.tintStrength);
            }
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh->indexCount), GL_UNSIGNED_INT, nullptr);
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererOpenGL::DrawLightVisualizer */
    Result<void> EditorViewportRendererOpenGL::DrawLightVisualizer(const Render::RenderCameraView &camera, const float aspect) {
        if (!lightVisualizerOptions_.selectedLight.has_value())
            return Result<void>::Success();

        LightVisualizerGeometry geometry;
        if (!BuildLightVisualizerGeometry(LightVisualizerGeometryRequest{.camera = camera, .light = *lightVisualizerOptions_.selectedLight},
                                          geometry)) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportInvalidScene, "Failed to build selected Light visualizer geometry."));
        }
        const Result<Math::Mat4> viewProjection =
            BuildRenderMvp(camera, Math::Mat4::Identity(), aspect, Math::ClipDepthRange::NegativeOneToOne);
        if (viewProjection.HasError())
            return Result<void>::Failure(viewProjection.ErrorValue());

        glDepthMask(GL_FALSE);
        glBindVertexArray(gridVertexArray_);
        glBindBuffer(GL_ARRAY_BUFFER, gridVertexBuffer_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(geometry.Lines().size_bytes()), geometry.Lines().data());
        if (reflectedProgram_) {
            glUseProgram(gridProgram_);
            const auto uploaded = UploadReloadGrid(viewProjection.Value(), geometry.color);
            if (uploaded.HasError())
                return uploaded;
        } else {
            glUniformMatrix4fv(uniforms_.mvp, 1, GL_FALSE, viewProjection.Value().values.data());
            const Math::Mat4 identity = Math::Mat4::Identity();
            glUniformMatrix4fv(uniforms_.model, 1, GL_FALSE, identity.values.data());
            glUniform3f(uniforms_.selectionColor, geometry.color.x, geometry.color.y, geometry.color.z);
            glUniform1f(uniforms_.selectionStrength, 1.0F);
        }
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(geometry.vertexCount));
        glDepthMask(GL_TRUE);
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererOpenGL::DrawGrid */
    Result<void> EditorViewportRendererOpenGL::DrawGrid(const Render::RenderCameraView &camera, const float aspect,
                                                        const float viewportHeightPixels) const {
        if (!gridOptions_.visible)
            return Result<void>::Success();

        ViewportGridGeometry geometry;
        if (!BuildViewportGridGeometry(
                ViewportGridGeometryRequest{
                    .camera = camera,
                    .aspect = aspect,
                    .viewportHeightPixels = viewportHeightPixels,
                    .targetMinorSpacingPixels = gridOptions_.targetMinorSpacingPixels,
                    .targetLineWidthPixels = gridOptions_.targetLineWidthPixels,
                },
                geometry)) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportInvalidScene, "Failed to build finite viewport grid geometry."));
        }

        const Result<Math::Mat4> viewProjection =
            BuildRenderMvp(camera, Math::Mat4::Identity(), aspect, Math::ClipDepthRange::NegativeOneToOne);
        if (viewProjection.HasError())
            return Result<void>::Failure(viewProjection.ErrorValue());

        glDepthMask(GL_FALSE);
        glBindVertexArray(gridVertexArray_);
        if (reflectedProgram_)
            glUseProgram(gridProgram_);
        else {
            glUniformMatrix4fv(uniforms_.mvp, 1, GL_FALSE, viewProjection.Value().values.data());
            const Math::Mat4 identity = Math::Mat4::Identity();
            glUniformMatrix4fv(uniforms_.model, 1, GL_FALSE, identity.values.data());
            glUniform1f(uniforms_.selectionStrength, 1.0F);
        }
        Result<void> upload = Result<void>::Success();
        const auto drawBatch = [&](const ViewportGridLineBatch &batch) {
            if (batch.positions.empty())
                return;
            glBindBuffer(GL_ARRAY_BUFFER, gridVertexBuffer_);
            glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(batch.positions.size_bytes()), batch.positions.data());
            if (reflectedProgram_) {
                upload = UploadReloadGrid(viewProjection.Value(), batch.color);
                if (upload.HasError())
                    return;
            } else
                glUniform3f(uniforms_.selectionColor, batch.color.x, batch.color.y, batch.color.z);
            const GLenum primitive = batch.topology == ViewportGridPrimitiveTopology::Triangles ? GL_TRIANGLES : GL_LINES;
            glDrawArrays(primitive, 0, static_cast<GLsizei>(batch.positions.size()));
        };
        drawBatch(geometry.RegularLines());
        if (upload.HasValue())
            drawBatch(geometry.Axes());
        glDepthMask(GL_TRUE);
        return upload;
    }

    /** @copydoc EditorViewportRendererOpenGL::TextureView */
    EditorViewportTextureView EditorViewportRendererOpenGL::TextureView() const noexcept {
        return EditorViewportTextureView{
            .textureId = resources_.ImageIdentity(),
            .u0 = 0.0F,
            .v0 = 1.0F,
            .u1 = 1.0F,
            .v1 = 0.0F,
            .sampledView = resources_.ColorTextureView(),
        };
    }

    /** @copydoc EditorViewportRendererOpenGL::IsReady */
    bool EditorViewportRendererOpenGL::IsReady() const noexcept {
        return initialized_ && resources_.IsReady();
    }

    Result<void> EditorViewportRendererOpenGL::CreateProgram(ProgramGeneration &candidate) {
        auto vertex = CompileShader(GL_VERTEX_SHADER, Detail::ViewportVertexShader);
        if (vertex.HasError()) {
            return Result<void>::Failure(vertex.ErrorValue());
        }
        auto fragment = CompileShader(GL_FRAGMENT_SHADER, Detail::ViewportFragmentShader);
        if (fragment.HasError()) {
            glDeleteShader(vertex.Value());
            return Result<void>::Failure(fragment.ErrorValue());
        }

        candidate.program = glCreateProgram();
        glAttachShader(candidate.program, vertex.Value());
        glAttachShader(candidate.program, fragment.Value());
        glBindAttribLocation(candidate.program, 0, "aPosition");
        glBindAttribLocation(candidate.program, 1, "aNormal");
        glBindAttribLocation(candidate.program, 2, "aUv");
        glBindFragDataLocation(candidate.program, 0, "outColor");
        glLinkProgram(candidate.program);
        glDeleteShader(vertex.Value());
        glDeleteShader(fragment.Value());

        GLint linked = GL_FALSE;
        glGetProgramiv(candidate.program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            GLint logLength = 0;
            glGetProgramiv(candidate.program, GL_INFO_LOG_LENGTH, &logLength);
            std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
            glGetProgramInfoLog(candidate.program, logLength, nullptr, log.data());
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportShaderLinkFailed, "Viewport shader linking failed: " + log));
        }
        return LoadUniformLocations(candidate);
    }

    Result<void> EditorViewportRendererOpenGL::LoadUniformLocations(ProgramGeneration &candidate) {
        candidate.uniforms.mvp = glGetUniformLocation(candidate.program, "uMvp");
        candidate.uniforms.model = glGetUniformLocation(candidate.program, "uModel");
        candidate.uniforms.cameraPosition = glGetUniformLocation(candidate.program, "uCameraPosition");
        candidate.uniforms.lightCount = glGetUniformLocation(candidate.program, "uLightCount");
        candidate.uniforms.lightPositionKind = glGetUniformLocation(candidate.program, "uLightPositionKind[0]");
        candidate.uniforms.lightDirectionRange = glGetUniformLocation(candidate.program, "uLightDirectionRange[0]");
        candidate.uniforms.lightColorIntensity = glGetUniformLocation(candidate.program, "uLightColorIntensity[0]");
        candidate.uniforms.lightCone = glGetUniformLocation(candidate.program, "uLightCone[0]");
        candidate.uniforms.shadowViewProjection = glGetUniformLocation(candidate.program, "uShadowViewProjection[0]");
        candidate.uniforms.shadowMap = glGetUniformLocation(candidate.program, "uShadowMap");
        candidate.uniforms.shadowEnabled = glGetUniformLocation(candidate.program, "uShadowEnabled");
        candidate.uniforms.shadowLightIndex = glGetUniformLocation(candidate.program, "uShadowLightIndex");
        candidate.uniforms.shadowCascadeCount = glGetUniformLocation(candidate.program, "uShadowCascadeCount");
        candidate.uniforms.cascadeSplits = glGetUniformLocation(candidate.program, "uCascadeSplits[0]");
        candidate.uniforms.atlasRects = glGetUniformLocation(candidate.program, "uAtlasRects[0]");
        candidate.uniforms.cameraForward = glGetUniformLocation(candidate.program, "uCameraForward");
        candidate.uniforms.shadowTexel = glGetUniformLocation(candidate.program, "uShadowTexel");
        candidate.uniforms.shadowBiasFilter = glGetUniformLocation(candidate.program, "uShadowBiasFilter");
        candidate.uniforms.selectionColor = glGetUniformLocation(candidate.program, "uSelectionColor");
        candidate.uniforms.selectionStrength = glGetUniformLocation(candidate.program, "uSelectionStrength");
        if (const std::array locations{
                candidate.uniforms.mvp,
                candidate.uniforms.model,
                candidate.uniforms.cameraPosition,
                candidate.uniforms.lightCount,
                candidate.uniforms.lightPositionKind,
                candidate.uniforms.lightDirectionRange,
                candidate.uniforms.lightColorIntensity,
                candidate.uniforms.lightCone,
                candidate.uniforms.shadowViewProjection,
                candidate.uniforms.shadowMap,
                candidate.uniforms.shadowEnabled,
                candidate.uniforms.shadowLightIndex,
                candidate.uniforms.shadowCascadeCount,
                candidate.uniforms.cascadeSplits,
                candidate.uniforms.atlasRects,
                candidate.uniforms.cameraForward,
                candidate.uniforms.shadowTexel,
                candidate.uniforms.shadowBiasFilter,
                candidate.uniforms.selectionColor,
                candidate.uniforms.selectionStrength,
            };
            !std::ranges::all_of(locations, [](const std::int32_t location) {
            return location >= 0;
        })) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportShaderContractInvalid, "Viewport shader is missing a required frame uniform."));
        }
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererOpenGL::CreateShadowProgram(ProgramGeneration &candidate) {
        auto vertex = CompileShader(GL_VERTEX_SHADER, Detail::ShadowVertexShader);
        if (vertex.HasError())
            return Result<void>::Failure(vertex.ErrorValue());
        auto fragment = CompileShader(GL_FRAGMENT_SHADER, Detail::ShadowFragmentShader);
        if (fragment.HasError()) {
            glDeleteShader(vertex.Value());
            return Result<void>::Failure(fragment.ErrorValue());
        }
        candidate.shadowProgram = glCreateProgram();
        glAttachShader(candidate.shadowProgram, vertex.Value());
        glAttachShader(candidate.shadowProgram, fragment.Value());
        glBindAttribLocation(candidate.shadowProgram, 0, "aPosition");
        glLinkProgram(candidate.shadowProgram);
        glDeleteShader(vertex.Value());
        glDeleteShader(fragment.Value());

        GLint linked = GL_FALSE;
        glGetProgramiv(candidate.shadowProgram, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            GLint logLength = 0;
            glGetProgramiv(candidate.shadowProgram, GL_INFO_LOG_LENGTH, &logLength);
            std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
            glGetProgramInfoLog(candidate.shadowProgram, logLength, nullptr, log.data());
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportShaderLinkFailed, "Viewport shadow shader linking failed: " + log));
        }
        candidate.uniforms.shadowMvp = glGetUniformLocation(candidate.shadowProgram, "uShadowMvp");
        if (candidate.uniforms.shadowMvp < 0) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportShaderContractInvalid, "Viewport shadow shader is missing its matrix uniform."));
        }

        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererOpenGL::DrawDirectionalShadowMap(const Render::RenderSceneView &scene,
                                                                        const EditorViewportDirectionalShadowView &shadow) {
        if (shadowProgramGeneration_ != liveGeneration_)
            shadowCache_ = {};
        if (!Render::DirectionalShadowRenderRequired(shadow, resources_.ShadowGeneration(), shadowCache_))
            return Result<void>::Success();
        // A failed partial redraw invalidates previous contents even if the next scene returns to an old fingerprint.
        shadowCache_.valid = false;
        if (const Result<void> bound = OpenGLViewportResourceBridge::BindRenderTarget(*frontend_, resources_.ShadowTarget());
            bound.HasError())
            return bound;
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        glClear(GL_DEPTH_BUFFER_BIT);
        glUseProgram(shadowProgram_);
        for (std::size_t cascade = 0; cascade < shadow.cascadeCount; ++cascade) {
            const auto drawn = DrawShadowCascade(scene, shadow, cascade);
            if (drawn.HasError())
                return drawn;
        }
        glCullFace(GL_BACK);
        if (glGetError() != GL_NO_ERROR) {
            shadowCache_ = {};
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        }
        const auto cache = Render::CommitDirectionalShadowRender(shadow, resources_.ShadowGeneration(), shadowCache_);
        if (cache.HasError())
            return Result<void>::Failure(cache.ErrorValue());
        shadowCache_ = cache.Value();
        shadowProgramGeneration_ = liveGeneration_;
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererOpenGL::DrawShadowCascade(const Render::RenderSceneView &scene,
                                                                 const Render::DirectionalShadowFrame &shadow,
                                                                 const std::size_t cascade) const {
        const auto &viewport = shadow.cascades[cascade].viewport;
        // Shared rectangles are top-left; GL's native viewport starts at the bottom-left.
        glViewport(static_cast<GLint>(viewport.x), static_cast<GLint>(shadow.atlasHeight - viewport.y - viewport.height),
                   static_cast<GLsizei>(viewport.width), static_cast<GLsizei>(viewport.height));
        for (const Render::RenderStaticMeshInstance &instance : scene.instances) {
            const auto mesh = resources_.FindMesh(instance.mesh.id.value);
            if (!mesh.has_value()) {
                return Result<void>::Failure(
                    MakeViewportError(RendererErrors::ViewportStaleMeshResource, "Shadow pass instance references a stale mesh resource."));
            }
            const Math::Mat4 shadowMvp = EditorViewportShadowMvp(shadow, cascade, instance.localToWorld);
            if (reflectedProgram_) {
                std::array<std::byte, 4096> bytes{};
                const auto packed = PackEditorViewportShadow(reloadLayouts_[2], shadowMvp, bytes);
                if (packed.HasError())
                    return Result<void>::Failure(packed.ErrorValue());
                if (const auto uploaded = UploadReload(2, std::span{bytes}.first(packed.Value())); uploaded.HasError())
                    return uploaded;
            } else
                glUniformMatrix4fv(uniforms_.shadowMvp, 1, GL_FALSE, shadowMvp.values.data());
            if (const Result<void> bound = OpenGLViewportResourceBridge::BindMesh(*frontend_, mesh->mesh); bound.HasError())
                return bound;
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh->indexCount), GL_UNSIGNED_INT, nullptr);
        }
        glCullFace(GL_BACK);
        return Result<void>::Success();
    }

    void EditorViewportRendererOpenGL::UploadLighting(const Render::RenderSceneView &scene,
                                                      const std::optional<EditorViewportDirectionalShadowView> &shadow) const {
        std::array<float, Render::MaximumForwardLights * 4> positionKind{};
        std::array<float, Render::MaximumForwardLights * 4> directionRange{};
        std::array<float, Render::MaximumForwardLights * 4> colorIntensity{};
        std::array<float, Render::MaximumForwardLights * 2> cones{};
        for (std::size_t index = 0; index < scene.lights.size(); ++index) {
            const Render::RenderLight &light = scene.lights[index];
            positionKind[index * 4] = light.position.x;
            positionKind[index * 4 + 1] = light.position.y;
            positionKind[index * 4 + 2] = light.position.z;
            positionKind[index * 4 + 3] = static_cast<float>(light.kind);
            directionRange[index * 4] = light.direction.x;
            directionRange[index * 4 + 1] = light.direction.y;
            directionRange[index * 4 + 2] = light.direction.z;
            directionRange[index * 4 + 3] = light.range;
            colorIntensity[index * 4] = light.color.x;
            colorIntensity[index * 4 + 1] = light.color.y;
            colorIntensity[index * 4 + 2] = light.color.z;
            colorIntensity[index * 4 + 3] = light.intensity;
            cones[index * 2] = light.innerConeCosine;
            cones[index * 2 + 1] = light.outerConeCosine;
        }
        glUniform3f(uniforms_.cameraPosition, scene.camera.position.x, scene.camera.position.y, scene.camera.position.z);
        glUniform1i(uniforms_.lightCount, static_cast<GLint>(scene.lights.size()));
        glUniformMatrix4fv(uniforms_.shadowViewProjection, 4, GL_FALSE, reloadParameters_.shadow.data());
        glUniform4fv(uniforms_.cascadeSplits, 4, reloadParameters_.cascadeSplits.data());
        glUniform4fv(uniforms_.atlasRects, 4, reloadParameters_.atlasRects.data());
        glUniform3fv(uniforms_.cameraForward, 1, reloadParameters_.cameraForward.data());
        glUniform2f(uniforms_.shadowTexel, reloadParameters_.shadowSettings[3], reloadParameters_.cameraForward[3]);
        glUniform4fv(uniforms_.shadowBiasFilter, 1, reloadParameters_.shadowBiasFilter.data());
        glUniform1i(uniforms_.shadowCascadeCount, static_cast<GLint>(reloadParameters_.shadowSettings[2]));
        glUniform1i(uniforms_.shadowMap, 7);
        glUniform1i(uniforms_.shadowEnabled, shadow.has_value() ? 1 : 0);
        glUniform1i(uniforms_.shadowLightIndex, shadow.has_value() ? static_cast<GLint>(shadow->lightIndex) : -1);
        if (!scene.lights.empty()) {
            glUniform4fv(uniforms_.lightPositionKind, static_cast<GLsizei>(scene.lights.size()), positionKind.data());
            glUniform4fv(uniforms_.lightDirectionRange, static_cast<GLsizei>(scene.lights.size()), directionRange.data());
            glUniform4fv(uniforms_.lightColorIntensity, static_cast<GLsizei>(scene.lights.size()), colorIntensity.data());
            glUniform2fv(uniforms_.lightCone, static_cast<GLsizei>(scene.lights.size()), cones.data());
        }
    }

}  // namespace Horo::Editor
