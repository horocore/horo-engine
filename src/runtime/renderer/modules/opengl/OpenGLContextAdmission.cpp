#include "OpenGLBackendInternal.h"
#include "OpenGLRenderBackendErrors.h"

namespace Horo::Render::Detail {
    namespace {
        /** @brief Compares the realized desktop context version against the explicit requested version. */
        [[nodiscard]] bool VersionAtLeast(const OpenGLContextFacts &facts, const std::uint16_t major, const std::uint16_t minor) noexcept {
            return facts.majorVersion > major || (facts.majorVersion == major && facts.minorVersion >= minor);
        }

    }  // namespace

    /** @copydoc ValidateOpenGLContextFacts */
    Result<void> ValidateOpenGLContextFacts(const OpenGLContextFacts &facts, const OpenGLBackendOptions &options) {
        if (facts.apiFamily != OpenGLApiFamily::Desktop)
            return Result<void>::Failure(MakeError(OpenGLBackendErrors::UnsupportedApiFamily, "Actual context is not desktop OpenGL."));
        if (!VersionAtLeast(facts, options.majorVersion, options.minorVersion))
            return Result<void>::Failure(
                MakeError(OpenGLBackendErrors::UnsupportedVersion, "Actual OpenGL context is below the requested version."));
        if (facts.profile != OpenGLContextProfile::Core)
            return Result<void>::Failure(MakeError(OpenGLBackendErrors::UnsupportedProfile, "Actual OpenGL context is not Core profile."));
        if (!facts.requiredEntryPointsAvailable)
            return Result<void>::Failure(
                MakeError(OpenGLBackendErrors::MissingRequiredEntryPoints, "OpenGL 4.1 Core command dispatch is incomplete."));
        if (facts.maxTexture2DSize == 0 || facts.maxColorAttachments == 0 || facts.maxVertexAttributes == 0)
            return Result<void>::Failure(MakeError(OpenGLBackendErrors::InvalidCapabilities, "OpenGL baseline limits must be non-zero."));
        return Result<void>::Success();
    }

    /** @copydoc MakeOpenGLCapabilitySnapshot */
    RenderCapabilitySnapshot MakeOpenGLCapabilitySnapshot(const OpenGLContextFacts &facts, const bool resourcesAvailable) noexcept {
        using enum RenderCapability;
        using enum RenderTextureFormat;
        using enum RenderTextureUsage;
        RenderCapabilitySnapshot snapshot{
            .deviceIncarnation = 1,
            .capabilityRevision = 1,
            .synthetic = false,
            .features = {},
            .queues = {.graphics = true, .compute = false, .copy = false, .present = true},
            .limits = {.maxBufferBytes = resourcesAvailable ? 4ULL * 1024ULL * 1024ULL * 1024ULL : 0,
                       .maxTextureDimension2D = resourcesAvailable ? facts.maxTexture2DSize : 0,
                       .maxColorAttachments = resourcesAvailable ? facts.maxColorAttachments : 0,
                       .maxVertexAttributes = resourcesAvailable ? facts.maxVertexAttributes : 0,
                       .maxFramesInFlight = 8},
            .formats = {},
        };
        snapshot.features.Enable(Presentation);
        if (resourcesAvailable) {
            for (const RenderCapability capability :
                 {OffscreenTargets, BufferResources, MeshResources, TextureResources, RenderTargetResources})
                snapshot.features.Enable(capability);
            snapshot.formats.usages[static_cast<std::size_t>(Rgba8Unorm)] = Sampled | RenderAttachment;
            snapshot.formats.usages[static_cast<std::size_t>(Depth24Stencil8)] = RenderAttachment;
            snapshot.formats.usages[static_cast<std::size_t>(Depth32Float)] = Sampled | RenderAttachment;
            snapshot.formats.sampleCountMask = std::uint64_t{1} << 1U;
        }
        return snapshot;
    }

}  // namespace Horo::Render::Detail
