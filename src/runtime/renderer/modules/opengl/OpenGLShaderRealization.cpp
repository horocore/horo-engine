#include "OpenGLShaderRealization.h"

#include "OpenGLRenderBackendErrors.h"

#include <algorithm>
#include <array>
#include <glad/gl.h>
#include <limits>
#include <string>
#include <type_traits>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Deletes an unpublished candidate on every exit, including diagnostic allocation failure. */
        template <typename Destroy> class Candidate final {
            static_assert(std::is_nothrow_invocable_v<Destroy, std::uint32_t> && std::is_nothrow_copy_constructible_v<Destroy>);

        public:
            Candidate(const std::uint32_t object, Destroy destroy) noexcept : object_(object), destroy_(destroy) {}

            ~Candidate() noexcept {
                if (object_ != 0)
                    destroy_(object_);
            }

            Candidate(const Candidate &) = delete;
            Candidate &operator=(const Candidate &) = delete;
            Candidate(Candidate &&) = delete;
            Candidate &operator=(Candidate &&) = delete;

            /** @brief Transfers sole deletion responsibility after successful preparation. */
            [[nodiscard]] std::uint32_t Release() noexcept {
                const std::uint32_t result = object_;
                object_ = 0;
                return result;
            }

        private:
            std::uint32_t object_;
            Destroy destroy_;
        };

        using NativeCandidate = Candidate<decltype(OpenGLShaderFunctions::deleteShader)>;
        static_assert(!std::is_copy_constructible_v<NativeCandidate> && !std::is_copy_assignable_v<NativeCandidate> &&
                      !std::is_move_constructible_v<NativeCandidate> && !std::is_move_assignable_v<NativeCandidate>);

        /** @brief Preserves at most 8191 original driver bytes without trusting a driver-reported allocation size. */
        template <typename ReadLog>
        [[nodiscard]] Error DriverFailure(const ErrorCodeDescriptor &code, const std::uint32_t object, ReadLog readLog) {
            static_assert(std::is_nothrow_invocable_r_v<std::size_t, ReadLog, std::uint32_t, std::span<char>>);
            std::array<char, 8'192> log{};
            const std::size_t reported = std::min(readLog(object, log), log.size() - 1);
            const auto written = std::span{log}.first(reported);
            const auto terminator = std::ranges::find(written, '\0');
            const auto count = static_cast<std::size_t>(terminator - written.begin());
            return MakeError(code, std::string{log.data(), count});
        }

        std::uint32_t CreateShader(const std::uint32_t stage) noexcept {
            return glCreateShader(stage);
        }

        void ShaderSource(const std::uint32_t shader, const std::span<const std::uint8_t> source) noexcept {
            const auto *text = reinterpret_cast<const char *>(source.data());
            const auto length = static_cast<GLint>(source.size());
            glShaderSource(shader, 1, &text, &length);
        }

        void CompileShader(const std::uint32_t shader) noexcept {
            glCompileShader(shader);
        }

        void ShaderParameter(const std::uint32_t shader, const std::uint32_t parameter, std::int32_t &value) noexcept {
            glGetShaderiv(shader, parameter, &value);
        }

        std::size_t ShaderLog(const std::uint32_t shader, const std::span<char> destination) noexcept {
            GLsizei written = 0;
            glGetShaderInfoLog(shader, static_cast<GLsizei>(destination.size()), &written, destination.data());
            return written > 0 ? static_cast<std::size_t>(written) : 0;
        }

        void DeleteShader(const std::uint32_t shader) noexcept {
            glDeleteShader(shader);
        }

        std::uint32_t CreateProgram() noexcept {
            return glCreateProgram();
        }

        void AttachShader(const std::uint32_t program, const std::uint32_t shader) noexcept {
            glAttachShader(program, shader);
        }

        void LinkProgram(const std::uint32_t program) noexcept {
            glLinkProgram(program);
        }

        void ProgramParameter(const std::uint32_t program, const std::uint32_t parameter, std::int32_t &value) noexcept {
            glGetProgramiv(program, parameter, &value);
        }

        std::size_t ProgramLog(const std::uint32_t program, const std::span<char> destination) noexcept {
            GLsizei written = 0;
            glGetProgramInfoLog(program, static_cast<GLsizei>(destination.size()), &written, destination.data());
            return written > 0 ? static_cast<std::size_t>(written) : 0;
        }

        void DeleteProgram(const std::uint32_t program) noexcept {
            glDeleteProgram(program);
        }

        std::uint32_t ErrorValue() noexcept {
            return glGetError();
        }

        /** @brief Checks actual shader entry-point readiness after the context owner loads native dispatch. */
        bool IsAvailable() noexcept {
            const std::array ready{glCreateShader != nullptr,  glShaderSource != nullptr,      glCompileShader != nullptr,
                                   glGetShaderiv != nullptr,   glGetShaderInfoLog != nullptr,  glDeleteShader != nullptr,
                                   glCreateProgram != nullptr, glAttachShader != nullptr,      glLinkProgram != nullptr,
                                   glGetProgramiv != nullptr,  glGetProgramInfoLog != nullptr, glDeleteProgram != nullptr,
                                   glGetError != nullptr};
            return std::ranges::all_of(ready, [](const bool available) {
                return available;
            });
        }
    }  // namespace

    /** @copydoc OpenGLShaderFunctions::IsValid */
    bool OpenGLShaderFunctions::IsValid() const noexcept {
        return isAvailable && createShader && shaderSource && compileShader && shaderParameter && shaderLog && deleteShader &&
               createProgram && attachShader && linkProgram && programParameter && programLog && deleteProgram && error;
    }

    /** @copydoc CompileOpenGLCookedStage */
    Result<std::uint32_t> CompileOpenGLCookedStage(const ShaderArtifactStageView &stage, const OpenGLShaderFunctions &functions) {
        if (!functions.IsValid() || !functions.isAvailable() ||
            (stage.stage != ShaderStage::Vertex && stage.stage != ShaderStage::Fragment))
            return Result<std::uint32_t>::Failure(
                MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                          "OpenGL 4.1 cooked graphics shader dispatch is unavailable or the stage is unsupported."));
        if (stage.bytes.empty() || stage.bytes.size() > 64U * 1024U * 1024U || stage.entryPoint.empty())
            return Result<std::uint32_t>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid,
                                                            "Cooked stage data is empty or exceeds the finite preparation envelope."));
        const std::uint32_t shader = functions.createShader(stage.stage == ShaderStage::Vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
        Candidate candidate{shader, functions.deleteShader};
        if (shader == 0)
            return Result<std::uint32_t>::Failure(
                MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL could not allocate the admitted shader candidate."));
        functions.shaderSource(shader, stage.bytes);
        functions.compileShader(shader);
        std::int32_t compiled = GL_FALSE;
        functions.shaderParameter(shader, GL_COMPILE_STATUS, compiled);
        if (compiled != GL_TRUE)
            return Result<std::uint32_t>::Failure(DriverFailure(OpenGLBackendErrors::ShaderCompileFailed, shader, functions.shaderLog));
        if (functions.error() != GL_NO_ERROR)
            return Result<std::uint32_t>::Failure(
                MakeError(OpenGLBackendErrors::CommandFailed, "OpenGL reported an error while compiling the cooked shader candidate."));
        return Result<std::uint32_t>::Success(candidate.Release());
    }

    /** @copydoc LinkOpenGLCookedGraphicsProgram */
    Result<std::uint32_t> LinkOpenGLCookedGraphicsProgram(const std::uint32_t vertex, const std::uint32_t fragment,
                                                          const OpenGLShaderFunctions &functions) {
        if (!functions.IsValid() || !functions.isAvailable())
            return Result<std::uint32_t>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
        if (vertex == 0 || fragment == 0 || vertex == fragment)
            return Result<std::uint32_t>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
        const std::uint32_t program = functions.createProgram();
        Candidate candidate{program, functions.deleteProgram};
        if (program == 0)
            return Result<std::uint32_t>::Failure(
                MakeError(OpenGLBackendErrors::ResourceCreationFailed, "OpenGL could not allocate the admitted program candidate."));
        functions.attachShader(program, vertex);
        functions.attachShader(program, fragment);
        functions.linkProgram(program);
        std::int32_t linked = GL_FALSE;
        functions.programParameter(program, GL_LINK_STATUS, linked);
        if (linked != GL_TRUE)
            return Result<std::uint32_t>::Failure(DriverFailure(OpenGLBackendErrors::PipelineLinkFailed, program, functions.programLog));
        if (functions.error() != GL_NO_ERROR)
            return Result<std::uint32_t>::Failure(
                MakeError(OpenGLBackendErrors::CommandFailed, "OpenGL reported an error while linking the graphics program candidate."));
        return Result<std::uint32_t>::Success(candidate.Release());
    }

    /** @copydoc ProductionOpenGLShaderFunctions */
    OpenGLShaderFunctions ProductionOpenGLShaderFunctions() noexcept {
        return {.isAvailable = &IsAvailable,
                .createShader = &CreateShader,
                .shaderSource = &ShaderSource,
                .compileShader = &CompileShader,
                .shaderParameter = &ShaderParameter,
                .shaderLog = &ShaderLog,
                .deleteShader = &DeleteShader,
                .createProgram = &CreateProgram,
                .attachShader = &AttachShader,
                .linkProgram = &LinkProgram,
                .programParameter = &ProgramParameter,
                .programLog = &ProgramLog,
                .deleteProgram = &DeleteProgram,
                .error = &ErrorValue};
    }
}  // namespace Horo::Render::Detail
