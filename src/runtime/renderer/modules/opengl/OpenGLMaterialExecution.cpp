#include "OpenGLMaterialExecution.h"

#include "Horo/Runtime/Render/VfxDrawData.h"
#include "OpenGLRenderBackendErrors.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <glad/gl.h>
#include <string>

namespace Horo::Render::Detail {
    namespace {
        constexpr std::size_t MaximumBindings = 128;
        constexpr std::size_t MaximumClipDistances = 64;
        constexpr GLenum FixedIndexRestart = 0x8D69;  // GL_PRIMITIVE_RESTART_FIXED_INDEX, not declared by every GL4.1 loader.

        /** @brief Inspects optional fixed-index restart without issuing an unsupported enum query on baseline contexts. */
        Result<bool> SupportsFixedIndexRestart() {
            GLint major = 0;
            GLint minor = 0;
            glGetIntegerv(GL_MAJOR_VERSION, &major);
            glGetIntegerv(GL_MINOR_VERSION, &minor);
            if (major > 4 || (major == 4 && minor >= 3))
                return Result<bool>::Success(true);
            GLint extensions = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
            if (extensions < 0 || extensions > 4096)
                return Result<bool>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
            for (GLint index = 0; index < extensions; ++index) {
                const auto *name = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(index));
                if (!name)
                    return Result<bool>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
                if (std::strcmp(reinterpret_cast<const char *>(name), "GL_ARB_ES3_compatibility") == 0)
                    return Result<bool>::Success(true);
            }
            return Result<bool>::Success(false);
        }

        /** @brief Restores every native binding and raster state touched by this bounded material consumer. */
        class MaterialState final {
        public:
            MaterialState(const GLuint target, const GLint drawBuffers, const GLint clips, const bool fixedRestartSupported) noexcept
                : target_(target), clipCount_(clips), fixedRestartSupported_(fixedRestartSupported), drawBufferCount_(drawBuffers) {
                glGetIntegerv(GL_CURRENT_PROGRAM, &bindings_.program);
                glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &bindings_.vertexArray);
                glGetIntegerv(GL_ACTIVE_TEXTURE, &bindings_.activeTexture);
                glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &bindings_.uniformBuffer);
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bindings_.framebuffer);
                glGetFloati_v(GL_VIEWPORT, 0, clear_.viewport.data());
                glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_.color.data());
                glGetDoublev(GL_DEPTH_CLEAR_VALUE, &clear_.depth);
                glGetBooleani_v(GL_COLOR_WRITEMASK, 0, raster_.colorMask.data());
                glGetBooleanv(GL_DEPTH_WRITEMASK, &raster_.depthMask);
                glGetIntegerv(GL_DEPTH_FUNC, &raster_.depthFunction);
                glGetIntegerv(GL_CULL_FACE_MODE, &raster_.cull);
                glGetIntegerv(GL_FRONT_FACE, &raster_.frontFace);
                glGetIntegerv(GL_POLYGON_MODE, raster_.polygonMode.data());
                glGetIntegeri_v(GL_BLEND_SRC_RGB, 0, &blendState_.sourceRgb);
                glGetIntegeri_v(GL_BLEND_DST_RGB, 0, &blendState_.destinationRgb);
                glGetIntegeri_v(GL_BLEND_SRC_ALPHA, 0, &blendState_.sourceAlpha);
                glGetIntegeri_v(GL_BLEND_DST_ALPHA, 0, &blendState_.destinationAlpha);
                glGetIntegeri_v(GL_BLEND_EQUATION_RGB, 0, &blendState_.equationRgb);
                glGetIntegeri_v(GL_BLEND_EQUATION_ALPHA, 0, &blendState_.equationAlpha);
                raster_.blend = glIsEnabledi(GL_BLEND, 0);
                raster_.scissor = glIsEnabledi(GL_SCISSOR_TEST, 0);
                for (std::size_t index = 0; index < capabilities_.size(); ++index)
                    raster_.enabled[index] = glIsEnabled(capabilities_[index]);
                if (fixedRestartSupported_)
                    fixedRestartEnabled_ = glIsEnabled(FixedIndexRestart);
                for (std::size_t index = 0; index < static_cast<std::size_t>(clipCount_); ++index)
                    raster_.clipEnabled[index] = glIsEnabled(GL_CLIP_DISTANCE0 + static_cast<GLenum>(index));
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_);
                for (GLint index = 0; index < drawBufferCount_; ++index) {
                    GLint value = 0;
                    glGetIntegerv(GL_DRAW_BUFFER0 + static_cast<GLenum>(index), &value);
                    targetDrawBuffers_[static_cast<std::size_t>(index)] = static_cast<GLenum>(value);
                }
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(bindings_.framebuffer));
            }

            ~MaterialState() noexcept {
                for (std::size_t index = 0; index < MaximumBindings; ++index) {
                    if (textures_[index].used) {
                        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(index));
                        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(textures_[index].texture));
                        glBindSampler(static_cast<GLuint>(index), static_cast<GLuint>(textures_[index].sampler));
                    }
                    if (buffers_[index].used) {
                        const auto &buffer = buffers_[index];
                        if (buffer.object == 0)
                            glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(index), 0);
                        else
                            glBindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(index), static_cast<GLuint>(buffer.object),
                                              static_cast<GLintptr>(buffer.offset), static_cast<GLsizeiptr>(buffer.size));
                    }
                }
                glBindBuffer(GL_UNIFORM_BUFFER, static_cast<GLuint>(bindings_.uniformBuffer));
                glActiveTexture(static_cast<GLenum>(bindings_.activeTexture));
                glUseProgram(static_cast<GLuint>(bindings_.program));
                glBindVertexArray(static_cast<GLuint>(bindings_.vertexArray));
                glViewportIndexedf(0, clear_.viewport[0], clear_.viewport[1], clear_.viewport[2], clear_.viewport[3]);
                glClearColor(clear_.color[0], clear_.color[1], clear_.color[2], clear_.color[3]);
                glClearDepth(clear_.depth);
                glColorMaski(0, raster_.colorMask[0], raster_.colorMask[1], raster_.colorMask[2], raster_.colorMask[3]);
                glDepthMask(raster_.depthMask);
                glDepthFunc(static_cast<GLenum>(raster_.depthFunction));
                glCullFace(static_cast<GLenum>(raster_.cull));
                glFrontFace(static_cast<GLenum>(raster_.frontFace));
                glPolygonMode(GL_FRONT_AND_BACK, static_cast<GLenum>(raster_.polygonMode[0]));
                glBlendFuncSeparatei(0, static_cast<GLenum>(blendState_.sourceRgb), static_cast<GLenum>(blendState_.destinationRgb),
                                     static_cast<GLenum>(blendState_.sourceAlpha), static_cast<GLenum>(blendState_.destinationAlpha));
                glBlendEquationSeparatei(0, static_cast<GLenum>(blendState_.equationRgb), static_cast<GLenum>(blendState_.equationAlpha));
                SetIndexed(GL_BLEND, raster_.blend);
                SetIndexed(GL_SCISSOR_TEST, raster_.scissor);
                for (std::size_t index = 0; index < capabilities_.size(); ++index)
                    Set(capabilities_[index], raster_.enabled[index]);
                if (fixedRestartSupported_)
                    Set(FixedIndexRestart, fixedRestartEnabled_);
                for (std::size_t index = 0; index < static_cast<std::size_t>(clipCount_); ++index)
                    Set(GL_CLIP_DISTANCE0 + static_cast<GLenum>(index), raster_.clipEnabled[index]);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_);
                glDrawBuffers(drawBufferCount_, targetDrawBuffers_.data());
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(bindings_.framebuffer));
            }

            MaterialState(const MaterialState &) = delete;
            MaterialState &operator=(const MaterialState &) = delete;

            /** @brief Captures only distinct native resource slots before any caller bindings change. */
            void Capture(const std::span<const OpenGLMaterialDrawCommand> commands) noexcept {
                for (const auto &command : commands)
                    for (const auto &slot : command.slots)
                        CaptureSlot(slot);
                glActiveTexture(static_cast<GLenum>(bindings_.activeTexture));
            }

            /** @brief Selects deterministic state at the admitted attachment without changing higher indexed draw state. */
            void Apply(const RenderMaterialExecutionRequest &request) const noexcept {
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_);
                glDrawBuffer(GL_COLOR_ATTACHMENT0);
                glViewportIndexedf(0, 0, 0, static_cast<float>(request.extent.width), static_cast<float>(request.extent.height));
                glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                SetIndexed(GL_SCISSOR_TEST, false);
                for (const auto capability : capabilities_)
                    Set(capability, false);
                if (fixedRestartSupported_)
                    Set(FixedIndexRestart, false);
                for (std::size_t index = 0; index < static_cast<std::size_t>(clipCount_); ++index)
                    Set(GL_CLIP_DISTANCE0 + static_cast<GLenum>(index), false);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                GLbitfield clear = 0;
                if (request.color.loadOperation == AttachmentLoadOperation::Clear) {
                    const auto &color = request.color.clearColor;
                    glClearColor(color.red, color.green, color.blue, color.alpha);
                    clear |= GL_COLOR_BUFFER_BIT;
                }
                if (request.depthClear.has_value()) {
                    glDepthMask(GL_TRUE);
                    glClearDepth(*request.depthClear);
                    clear |= GL_DEPTH_BUFFER_BIT;
                }
                if (clear != 0)
                    glClear(clear);
            }

        private:
            /** @brief Captures each distinct buffer/texture unit exactly once before encoding changes it. */
            void CaptureSlot(const OpenGLMaterialSlot &slot) noexcept {
                if (slot.kind == MaterialResidentResourceKind::Buffer) {
                    auto &buffer = buffers_[slot.binding];
                    if (buffer.used)
                        return;
                    glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, slot.binding, &buffer.object);
                    glGetInteger64i_v(GL_UNIFORM_BUFFER_START, slot.binding, &buffer.offset);
                    glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, slot.binding, &buffer.size);
                    buffer.used = true;
                    return;
                }
                auto &texture = textures_[slot.binding];
                if (texture.used)
                    return;
                glActiveTexture(GL_TEXTURE0 + slot.binding);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture.texture);
                // OpenGL 4.1 exposes this binding through the already-selected active texture unit.
                glGetIntegerv(GL_SAMPLER_BINDING, &texture.sampler);
                texture.used = true;
            }

            static void Set(const GLenum capability, const bool enabled) noexcept {
                if (enabled)
                    glEnable(capability);
                else
                    glDisable(capability);
            }

            static void SetIndexed(const GLenum capability, const bool enabled) noexcept {
                if (enabled)
                    glEnablei(capability, 0);
                else
                    glDisablei(capability, 0);
            }

            struct TextureState {
                GLint texture{};
                GLint sampler{};
                bool used{false};
            };

            struct BufferState {
                GLint object{};
                GLint64 offset{};
                GLint64 size{};
                bool used{false};
            };

            static constexpr std::array capabilities_{GL_DEPTH_TEST,
                                                      GL_CULL_FACE,
                                                      GL_STENCIL_TEST,
                                                      GL_DITHER,
                                                      GL_FRAMEBUFFER_SRGB,
                                                      GL_RASTERIZER_DISCARD,
                                                      GL_POLYGON_OFFSET_FILL,
                                                      GL_COLOR_LOGIC_OP,
                                                      GL_SAMPLE_ALPHA_TO_COVERAGE,
                                                      GL_SAMPLE_ALPHA_TO_ONE,
                                                      GL_SAMPLE_COVERAGE,
                                                      GL_SAMPLE_MASK,
                                                      GL_PRIMITIVE_RESTART};

            /** @brief Caller object bindings restored after slot-specific state. */
            struct ObjectBindings {
                GLint program{};
                GLint vertexArray{};
                GLint activeTexture{};
                GLint uniformBuffer{};
                GLint framebuffer{};
            };

            /** @brief Indexed viewport and attachment-clear values borrowed from the caller. */
            struct ViewportClearState {
                std::array<float, 4> viewport{};
                std::array<float, 4> color{};
                GLdouble depth{};
            };

            /** @brief Depth, culling, write masks and capability state for the caller's raster configuration. */
            struct RasterState {
                GLint depthFunction{};
                GLint cull{};
                GLint frontFace{};
                std::array<GLint, 2> polygonMode{};
                std::array<GLboolean, 4> colorMask{};
                GLboolean depthMask{};
                GLboolean blend{};
                GLboolean scissor{};
                std::array<GLboolean, capabilities_.size()> enabled{};
                std::array<GLboolean, MaximumClipDistances> clipEnabled{};
            };

            /** @brief Indexed color blending factors and equations restored independently of enablement. */
            struct BlendState {
                GLint sourceRgb{};
                GLint destinationRgb{};
                GLint sourceAlpha{};
                GLint destinationAlpha{};
                GLint equationRgb{};
                GLint equationAlpha{};
            };

            GLuint target_;
            GLint clipCount_;
            bool fixedRestartSupported_;
            bool fixedRestartEnabled_{false};
            GLint drawBufferCount_;
            ObjectBindings bindings_;
            ViewportClearState clear_;
            RasterState raster_;
            BlendState blendState_;
            std::array<GLenum, 64> targetDrawBuffers_{};
            std::array<TextureState, MaximumBindings> textures_{};
            std::array<BufferState, MaximumBindings> buffers_{};
        };

        /** @brief Applies the complete immutable pipeline recipe without interpreting authored material settings. */
        void ApplyPipeline(const RenderGraphicsPipelineState &state) noexcept {
            constexpr std::array compare{GL_NEVER, GL_LESS, GL_LEQUAL, GL_EQUAL, GL_GEQUAL, GL_GREATER, GL_NOTEQUAL, GL_ALWAYS};
            if (state.depthTest)
                glEnable(GL_DEPTH_TEST);
            else
                glDisable(GL_DEPTH_TEST);
            glDepthMask(state.depthWrite ? GL_TRUE : GL_FALSE);
            glDepthFunc(compare[static_cast<std::size_t>(state.depthCompare)]);
            if (state.cull == RenderCullMode::None)
                glDisable(GL_CULL_FACE);
            else
                glEnable(GL_CULL_FACE);
            glCullFace(state.cull == RenderCullMode::Front ? GL_FRONT : GL_BACK);
            glFrontFace(state.frontFace == RenderFrontFace::Clockwise ? GL_CW : GL_CCW);
            if (state.blend == RenderBlendMode::Disabled)
                glDisablei(GL_BLEND, 0);
            else
                glEnablei(GL_BLEND, 0);
            glBlendEquationSeparatei(0, GL_FUNC_ADD, GL_FUNC_ADD);
            switch (state.blend) {
                using enum RenderBlendMode;
                case Disabled:
                    glBlendFuncSeparatei(0, GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
                    break;
                case Alpha:
                    glBlendFuncSeparatei(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    break;
                case PremultipliedAlpha:
                    glBlendFuncSeparatei(0, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    break;
                case Additive:
                    glBlendFuncSeparatei(0, GL_ONE, GL_ONE, GL_ONE, GL_ONE);
                    break;
            }
        }

        /** @brief Captures bounded optional context capabilities at pipeline preparation rather than scanning extensions in-frame. */
        Result<OpenGLMaterialContextFeatures> QueryFeatures() {
            if (glGetError() != GL_NO_ERROR)
                return Result<OpenGLMaterialContextFeatures>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
            GLint drawBuffers = 0;
            GLint clips = 0;
            glGetIntegerv(GL_MAX_DRAW_BUFFERS, &drawBuffers);
            glGetIntegerv(GL_MAX_CLIP_DISTANCES, &clips);
            if (drawBuffers < 1 || drawBuffers > 64 || clips < 0 || clips > static_cast<GLint>(MaximumClipDistances))
                return Result<OpenGLMaterialContextFeatures>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
            const auto fixedRestart = SupportsFixedIndexRestart();
            if (fixedRestart.HasError())
                return Result<OpenGLMaterialContextFeatures>::Failure(fixedRestart.ErrorValue());
            if (glGetError() != GL_NO_ERROR)
                return Result<OpenGLMaterialContextFeatures>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
            return Result<OpenGLMaterialContextFeatures>::Success({drawBuffers, clips, fixedRestart.Value()});
        }

        /** @brief Binds one already validated immutable command's exact native material resources. */
        void BindMaterialDraw(const OpenGLMaterialDrawCommand &command) {
            ApplyPipeline(command.state);
            glUseProgram(command.program);
            glBindVertexArray(command.vertexArray);
            for (const auto &slot : command.slots) {
                if (slot.kind == MaterialResidentResourceKind::Buffer)
                    glBindBufferRange(GL_UNIFORM_BUFFER, slot.binding, slot.object, static_cast<GLintptr>(slot.offset),
                                      static_cast<GLsizeiptr>(slot.byteCount));
                else {
                    glActiveTexture(GL_TEXTURE0 + slot.binding);
                    glBindTexture(GL_TEXTURE_2D, slot.object);
                    glBindSampler(slot.binding, slot.sampler);
                }
            }
        }

        /** @brief Submits the existing zero-base indexed draw without touching capture/restoration ownership. */
        void SubmitMaterialDraw(const OpenGLMaterialDrawCommand &command) {
            const GLenum indexType = command.indexFormat == RenderIndexFormat::UInt16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
            if (command.instanceCount == 1)
                glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(command.indexCount), indexType, nullptr);
            else
                glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(command.indexCount), indexType, nullptr,
                                        static_cast<GLsizei>(command.instanceCount));
        }

        /** @brief Retains the first native failure while every later capture still consumes its own GL error exactly once. */
        struct MaterialExecutionFailure final {
            GLenum nativeError{GL_NO_ERROR};
            const char *stage{"none"};

            bool Capture(const char *currentStage) noexcept {
                const auto error = glGetError();
                if (error != GL_NO_ERROR && nativeError == GL_NO_ERROR) {
                    nativeError = error;
                    stage = currentStage;
                }
                return error != GL_NO_ERROR;
            }
        };

        /** @brief Encodes bounded indexed draws under allocation-free restoration; no compilation, waits or resource allocation occur. */
        Result<void> Execute(const RenderMaterialExecutionRequest &request, const std::span<const OpenGLMaterialDrawCommand> commands) {
            if (glGetError() != GL_NO_ERROR || commands.empty() || std::ranges::any_of(commands, [](const auto &draw) {
                return draw.instanceCount == 0 || draw.instanceCount > MaximumVfxDrawInstances;
            }))
                return Result<void>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
            const auto features = commands.front().features;
            if (!features.IsValid())
                return Result<void>::Failure(MakeError(OpenGLBackendErrors::InvalidCapabilities));
            GLboolean feedback = GL_FALSE;
            GLboolean paused = GL_FALSE;
            glGetBooleanv(GL_TRANSFORM_FEEDBACK_BUFFER_ACTIVE, &feedback);
            glGetBooleanv(GL_TRANSFORM_FEEDBACK_BUFFER_PAUSED, &paused);
            if (feedback && !paused)
                return Result<void>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
            MaterialExecutionFailure failure;
            bool failed = failure.Capture("transform feedback inspection");
            {
                MaterialState state{static_cast<GLuint>(request.target), features.drawBuffers, features.clipDistances,
                                    features.fixedIndexRestart};
                state.Capture(commands);
                failed = failure.Capture("caller state capture") || failed;
                if (!failed) {
                    state.Apply(request);
                    failed = failure.Capture("attachment and raster setup");
                }
                for (const auto &command : commands) {
                    if (failed)
                        break;
                    BindMaterialDraw(command);
                    failed = failure.Capture("pipeline and resource binding");
                    if (!failed) {
                        SubmitMaterialDraw(command);
                        failed = failure.Capture("indexed draw");
                    }
                }
            }
            const auto restorationError = glGetError();
            if (failed || restorationError != GL_NO_ERROR)
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::CommandFailed, std::string{"OpenGL material execution failed at "} + failure.stage +
                                                                      " (native error " + std::to_string(failure.nativeError) +
                                                                      ", restoration error " + std::to_string(restorationError) +
                                                                      "); submitted generations remain completion-owned."));
            return Result<void>::Success();
        }

        bool IsAvailable() noexcept {
            return glGetError && glGetStringi && glGetIntegerv && glGetIntegeri_v && glGetInteger64i_v && glGetBooleanv &&
                   glGetBooleani_v && glGetFloatv && glGetFloati_v && glGetDoublev && glIsEnabled && glIsEnabledi && glEnable &&
                   glDisable && glEnablei && glDisablei && glUseProgram && glBindVertexArray && glActiveTexture && glBindTexture &&
                   glBindSampler && glBindBuffer && glBindBufferBase && glBindBufferRange && glBindFramebuffer && glDrawBuffer &&
                   glDrawBuffers && glViewportIndexedf && glClearColor && glClearDepth && glClear && glColorMaski && glDepthMask &&
                   glDepthFunc && glCullFace && glFrontFace && glPolygonMode && glBlendFuncSeparatei && glBlendEquationSeparatei &&
                   glDrawElements && glDrawElementsInstanced;
        }
    }  // namespace

    /** @copydoc ProductionOpenGLMaterialExecutionFunctions */
    OpenGLMaterialExecutionFunctions ProductionOpenGLMaterialExecutionFunctions() noexcept {
        return {.isAvailable = &IsAvailable, .queryFeatures = &QueryFeatures, .execute = &Execute};
    }
}  // namespace Horo::Render::Detail
