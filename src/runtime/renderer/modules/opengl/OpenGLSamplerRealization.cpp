#include "OpenGLRenderBackendErrors.h"
#include "OpenGLShaderRealization.h"

#include <array>
#include <glad/gl.h>
#include <memory>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Checks actual entry points after context initialization. */
        bool Available() noexcept {
            return glGenSamplers && glDeleteSamplers && glSamplerParameteri && glSamplerParameterf && glGetError;
        }

        /** @brief Creates one candidate without selecting a texture unit. */
        void Generate(std::uint32_t &sampler) noexcept {
            glGenSamplers(1, &sampler);
        }

        /** @brief Deletes the exact unpublished or retired sampler. */
        void Destroy(const std::uint32_t sampler) noexcept {
            glDeleteSamplers(1, &sampler);
        }

        /** @brief Sets one exact integer sampler policy. */
        void Integer(const std::uint32_t sampler, const std::uint32_t parameter, const std::int32_t value) noexcept {
            glSamplerParameteri(sampler, parameter, value);
        }

        /** @brief Sets one admitted finite sampler LOD policy. */
        void Floating(const std::uint32_t sampler, const std::uint32_t parameter, const float value) noexcept {
            glSamplerParameterf(sampler, parameter, value);
        }

        /** @brief Consumes the current native error without synchronization. */
        std::uint32_t ErrorValue() noexcept {
            return glGetError();
        }

        /** @brief Maps already validated Horo addressing policy to its core API constant. */
        std::int32_t Address(const RenderSamplerAddressMode mode) noexcept {
            constexpr std::array values{GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER};
            return static_cast<std::int32_t>(values[static_cast<std::size_t>(mode)]);
        }
    }  // namespace

    /** @copydoc CreateOpenGLSampler */
    Result<std::uint32_t> CreateOpenGLSampler(const RenderSamplerDescriptor &descriptor, const OpenGLSamplerFunctions &functions) {
        if (const auto valid = ValidateRenderSamplerDescriptor(descriptor); valid.HasError())
            return Result<std::uint32_t>::Failure(valid.ErrorValue());
        if (!functions.IsValid() || !functions.isAvailable() || descriptor.maximumAnisotropy != 1.0F)
            return Result<std::uint32_t>::Failure(
                MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                          "OpenGL sampler preparation requires complete core dispatch and anisotropy exactly one."));
        if (functions.error() != GL_NO_ERROR)
            return Result<std::uint32_t>::Failure(
                MakeError(OpenGLBackendErrors::CommandFailed, "OpenGL has an outstanding error before sampler preparation."));
        std::uint32_t sampler = 0;
        const auto destroy = [&functions](const std::uint32_t *candidate) noexcept {
            if (*candidate != 0)
                functions.destroy(*candidate);
        };
        std::unique_ptr<std::uint32_t, decltype(destroy)> rollback{&sampler, destroy};
        functions.generate(sampler);
        if (sampler == 0)
            return Result<std::uint32_t>::Failure(MakeError(OpenGLBackendErrors::ResourceCreationFailed));
        const bool linearMin = descriptor.minFilter == RenderSamplerFilter::Linear;
        const bool linearMip = descriptor.mipmapMode == RenderSamplerMipmapMode::Linear;
        const auto linearMipmap = linearMin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
        const auto nearestMipmap = linearMin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
        const auto minification = linearMip ? linearMipmap : nearestMipmap;
        functions.parameterInteger(sampler, GL_TEXTURE_MIN_FILTER, static_cast<std::int32_t>(minification));
        functions.parameterInteger(sampler, GL_TEXTURE_MAG_FILTER,
                                   descriptor.magFilter == RenderSamplerFilter::Linear ? GL_LINEAR : GL_NEAREST);
        functions.parameterInteger(sampler, GL_TEXTURE_WRAP_S, Address(descriptor.addressU));
        functions.parameterInteger(sampler, GL_TEXTURE_WRAP_T, Address(descriptor.addressV));
        functions.parameterInteger(sampler, GL_TEXTURE_WRAP_R, Address(descriptor.addressW));
        functions.parameterFloat(sampler, GL_TEXTURE_MIN_LOD, descriptor.minimumLod);
        functions.parameterFloat(sampler, GL_TEXTURE_MAX_LOD, descriptor.maximumLod);
        functions.parameterInteger(sampler, GL_TEXTURE_COMPARE_MODE, descriptor.compareEnabled ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE);
        constexpr std::array compare{GL_NEVER, GL_LESS, GL_LEQUAL, GL_EQUAL, GL_GEQUAL, GL_GREATER, GL_NOTEQUAL, GL_ALWAYS};
        functions.parameterInteger(sampler, GL_TEXTURE_COMPARE_FUNC,
                                   static_cast<std::int32_t>(compare[static_cast<std::size_t>(descriptor.compare)]));
        if (functions.error() != GL_NO_ERROR)
            return Result<std::uint32_t>::Failure(
                MakeError(OpenGLBackendErrors::CommandFailed, "OpenGL could not realize the complete admitted sampler policy."));
        static_cast<void>(rollback.release());
        return Result<std::uint32_t>::Success(sampler);
    }

    /** @copydoc ProductionOpenGLSamplerFunctions */
    OpenGLSamplerFunctions ProductionOpenGLSamplerFunctions() noexcept {
        return {.isAvailable = &Available,
                .generate = &Generate,
                .destroy = &Destroy,
                .parameterInteger = &Integer,
                .parameterFloat = &Floating,
                .error = &ErrorValue};
    }
}  // namespace Horo::Render::Detail
