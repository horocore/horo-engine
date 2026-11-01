#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif

#include "OpenGLViewportShaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <utility>

namespace Horo::Editor::Detail {
    namespace {
        struct CglPixelFormat final {
            CGLPixelFormatObj value{};

            ~CglPixelFormat() {
                if (value)
                    CGLReleasePixelFormat(value);
            }
        };

        /** @brief Owns a drawable-free real core context and restores the exact prior context on every exit path. */
        class OffscreenCoreContext final {
        public:
            OffscreenCoreContext() noexcept : previous_(CGLGetCurrentContext()) {
                if (previous_)
                    CGLRetainContext(previous_);
            }

            ~OffscreenCoreContext() {
                static_cast<void>(CGLSetCurrentContext(previous_));
                if (context_)
                    CGLReleaseContext(context_);
                if (previous_)
                    CGLReleaseContext(previous_);
            }

            OffscreenCoreContext(const OffscreenCoreContext &) = delete;
            OffscreenCoreContext &operator=(const OffscreenCoreContext &) = delete;

            [[nodiscard]] CGLError Create() noexcept {
                const std::array attributes{kCGLPFAOpenGLProfile,
                                            static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_GL4_Core),
                                            kCGLPFAAccelerated,
                                            kCGLPFAAllowOfflineRenderers,
                                            kCGLPFAColorSize,
                                            static_cast<CGLPixelFormatAttribute>(24),
                                            kCGLPFADepthSize,
                                            static_cast<CGLPixelFormatAttribute>(24),
                                            static_cast<CGLPixelFormatAttribute>(0)};
                CglPixelFormat format;
                GLint count{};
                const auto chosen = CGLChoosePixelFormat(attributes.data(), &format.value, &count);
                if (chosen != kCGLNoError)
                    return chosen;
                if (!format.value || count == 0)
                    return kCGLBadPixelFormat;
                const auto created = CGLCreateContext(format.value, nullptr, &context_);
                if (created != kCGLNoError)
                    return created;
                return CGLSetCurrentContext(context_);
            }

        private:
            CGLContextObj previous_{}, context_{};
        };

        struct NativeShader final {
            GLuint value{};
            NativeShader() = default;
            NativeShader(const NativeShader &) = delete;
            NativeShader &operator=(const NativeShader &) = delete;

            NativeShader(NativeShader &&other) noexcept : value(std::exchange(other.value, 0)) {}

            ~NativeShader() {
                if (value)
                    glDeleteShader(value);
            }
        };

        struct NativeProgram final {
            GLuint value{};
            NativeProgram() = default;
            NativeProgram(const NativeProgram &) = delete;
            NativeProgram &operator=(const NativeProgram &) = delete;

            NativeProgram(NativeProgram &&other) noexcept : value(std::exchange(other.value, 0)) {}

            ~NativeProgram() {
                if (value)
                    glDeleteProgram(value);
            }
        };

        /** @brief Compiles actual production bytes through CGL's native dispatch without touching global GLAD pointers. */
        NativeShader CompileViewportStage(GLenum stage, const char *source) {
            NativeShader shader;
            shader.value = glCreateShader(stage);
            REQUIRE(shader.value != 0);
            glShaderSource(shader.value, 1, &source, nullptr);
            glCompileShader(shader.value);
            GLint compiled{};
            glGetShaderiv(shader.value, GL_COMPILE_STATUS, &compiled);
            std::array<char, 4096> log{};
            glGetShaderInfoLog(shader.value, static_cast<GLsizei>(log.size()), nullptr, log.data());
            INFO(log.data());
            REQUIRE(compiled == GL_TRUE);
            return shader;
        }

        /** @brief Links the bootstrap geometry attribute recipe using only the actual production stage strings. */
        NativeProgram LinkViewportProgram(const char *vertexSource, const char *fragmentSource) {
            const auto vertex = CompileViewportStage(GL_VERTEX_SHADER, vertexSource);
            const auto fragment = CompileViewportStage(GL_FRAGMENT_SHADER, fragmentSource);
            NativeProgram program;
            program.value = glCreateProgram();
            REQUIRE(program.value != 0);
            glAttachShader(program.value, vertex.value);
            glAttachShader(program.value, fragment.value);
            glBindAttribLocation(program.value, 0, "aPosition");
            glBindAttribLocation(program.value, 1, "aNormal");
            glBindAttribLocation(program.value, 2, "aUv");
            glLinkProgram(program.value);
            GLint linked{};
            glGetProgramiv(program.value, GL_LINK_STATUS, &linked);
            std::array<char, 4096> log{};
            glGetProgramInfoLog(program.value, static_cast<GLsizei>(log.size()), nullptr, log.data());
            INFO(log.data());
            REQUIRE(linked == GL_TRUE);
            return program;
        }

        /** @brief Uses actual driver reflection to prove cascade array capacity and sampler/scalar shape. */
        void RequireUniform(GLuint program, const char *name, GLenum expectedType, GLint expectedCount = 1) {
            GLuint index = GL_INVALID_INDEX;
            glGetUniformIndices(program, 1, &name, &index);
            INFO(name);
            REQUIRE(index != GL_INVALID_INDEX);
            GLint type{}, count{};
            glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_TYPE, &type);
            glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_SIZE, &count);
            CHECK(type == static_cast<GLint>(expectedType));
            CHECK(count == expectedCount);
            CHECK(glGetUniformLocation(program, name) >= 0);
        }

        void RequireCascadeUniforms(GLuint program) {
            RequireUniform(program, "uShadowViewProjection[0]", GL_FLOAT_MAT4, 4);
            RequireUniform(program, "uCascadeSplits[0]", GL_FLOAT_VEC4, 4);
            RequireUniform(program, "uAtlasRects[0]", GL_FLOAT_VEC4, 4);
            RequireUniform(program, "uShadowMap", GL_SAMPLER_2D);
            RequireUniform(program, "uShadowEnabled", GL_INT);
            RequireUniform(program, "uShadowLightIndex", GL_INT);
            RequireUniform(program, "uShadowCascadeCount", GL_INT);
            RequireUniform(program, "uShadowBiasFilter", GL_FLOAT_VEC4);
            RequireUniform(program, "uShadowTexel", GL_FLOAT_VEC2);
            RequireUniform(program, "uCameraForward", GL_FLOAT_VEC3);
            RequireUniform(program, "uLightPositionKind[0]", GL_FLOAT_VEC4, 16);
            RequireUniform(program, "uLightDirectionRange[0]", GL_FLOAT_VEC4, 16);
            RequireUniform(program, "uLightColorIntensity[0]", GL_FLOAT_VEC4, 16);
            RequireUniform(program, "uLightCone[0]", GL_FLOAT_VEC2, 16);
        }
    }  // namespace

    namespace {
        /** @brief Keeps every native shader owner inside the isolated context lifetime. */
        void RequireProductionViewportShaders() {
            OffscreenCoreContext context;
            const auto created = context.Create();
            if (created != kCGLNoError)
                SKIP("No real CGL 4.1 core context is available: " << CGLErrorString(created));
            GLint major{}, minor{}, profile{};
            glGetIntegerv(GL_MAJOR_VERSION, &major);
            glGetIntegerv(GL_MINOR_VERSION, &minor);
            glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
            if (major < 4 || (major == 4 && minor < 1) || (profile & GL_CONTEXT_CORE_PROFILE_BIT) == 0)
                SKIP("The actual CGL context does not satisfy the required 4.1 core profile.");
            INFO(reinterpret_cast<const char *>(glGetString(GL_VERSION)));
            const auto main = LinkViewportProgram(ViewportVertexShader, ViewportFragmentShader);
            RequireCascadeUniforms(main.value);
            // The production bootstrap Grid uses this same Main program with full selection color.
            RequireUniform(main.value, "uMvp", GL_FLOAT_MAT4);
            RequireUniform(main.value, "uModel", GL_FLOAT_MAT4);
            RequireUniform(main.value, "uSelectionColor", GL_FLOAT_VEC3);
            RequireUniform(main.value, "uSelectionStrength", GL_FLOAT);
            const auto shadow = LinkViewportProgram(ShadowVertexShader, ShadowFragmentShader);
            RequireUniform(shadow.value, "uShadowMvp", GL_FLOAT_MAT4);
            CHECK(glGetError() == GL_NO_ERROR);
        }
    }  // namespace

    TEST_CASE("Production OpenGL viewport cascaded shadow stages link and expose the actual native uniform ABI",
              "[integration][renderer][opengl][gpu][shadow][shader]") {
        const auto previous = CGLGetCurrentContext();
        RequireProductionViewportShaders();
        CHECK(CGLGetCurrentContext() == previous);
    }
}  // namespace Horo::Editor::Detail
