#include "OpenGLGraphWorkloads.h"

#include "OpenGLExecutionState.h"
#include "OpenGLRenderBackendErrors.h"

#include <glad/gl.h>
#include <limits>
#include <memory>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Looks up exact graph/native identity without narrowing unknown native tokens. */
        [[nodiscard]] std::uint32_t Instance(const RenderGraphResourceId id, const RenderGraphExecutionRequest &request) {
            if (id.owner != request.graph.Owner() || id.value == 0 || id.value > request.resources.size())
                return 0;
            const auto &resolved = request.resources[id.value - 1U];
            return resolved.resource == id && resolved.instance <= std::numeric_limits<std::uint32_t>::max()
                       ? static_cast<std::uint32_t>(resolved.instance)
                       : 0;
        }

        /** @brief Restores the caller's draw target after preparation-only attachment construction. */
        class DrawTargetRestore final {
        public:
            explicit DrawTargetRestore(const OpenGLCommandFunctions &functions) noexcept : functions_(functions) {
                functions_.state.getInteger(GL_DRAW_FRAMEBUFFER_BINDING, std::span{&framebuffer_, 1});
            }

            ~DrawTargetRestore() noexcept {
                functions_.state.bindDrawFramebuffer(static_cast<std::uint32_t>(framebuffer_));
            }

        private:
            const OpenGLCommandFunctions &functions_;
            std::int32_t framebuffer_{};
        };

        /** @brief Restores buffer-copy bindings on every command and error path. */
        class CopyBindingsRestore final {
        public:
            explicit CopyBindingsRestore(const OpenGLCommandFunctions &functions) noexcept : functions_(functions) {
                functions_.state.getInteger(GL_COPY_READ_BUFFER, std::span{&source_, 1});
                functions_.state.getInteger(GL_COPY_WRITE_BUFFER, std::span{&destination_, 1});
            }

            ~CopyBindingsRestore() noexcept {
                functions_.buffers.bindBuffer(GL_COPY_READ_BUFFER, static_cast<std::uint32_t>(source_));
                functions_.buffers.bindBuffer(GL_COPY_WRITE_BUFFER, static_cast<std::uint32_t>(destination_));
            }

        private:
            const OpenGLCommandFunctions &functions_;
            std::int32_t source_{};
            std::int32_t destination_{};
        };

        /** @brief Verifies native copy flags, distinct storage and representable byte ranges. */
        [[nodiscard]] bool ValidCopy(const RenderGraphBufferCopy &copy, const RenderGraphExecutionRequest &request,
                                     const OpenGLGraphResources &resources) {
            const auto sourceId = Instance(copy.source, request);
            const auto destinationId = Instance(copy.destination, request);
            const auto source = resources.buffers.find(sourceId);
            const auto destination = resources.buffers.find(destinationId);
            constexpr auto maximum = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
            return source != resources.buffers.end() && destination != resources.buffers.end() && sourceId != destinationId &&
                   HasBufferUsage(source->second.usage, RenderBufferUsage::CopySource) &&
                   HasBufferUsage(destination->second.usage, RenderBufferUsage::CopyDestination) && copy.sourceOffset <= maximum &&
                   copy.destinationOffset <= maximum && copy.byteCount <= maximum && copy.sourceOffset <= source->second.byteSize &&
                   copy.byteCount <= source->second.byteSize - copy.sourceOffset &&
                   copy.destinationOffset <= destination->second.byteSize &&
                   copy.byteCount <= destination->second.byteSize - copy.destinationOffset;
        }

        /** @brief Checks the actual prepared whole-color attachment rather than trusting a graph-local token. */
        [[nodiscard]] bool ValidColor(const RenderGraphColorAttachment &color, const RenderGraphExecutionRequest &request,
                                      const OpenGLGraphResources &resources) {
            const auto texture = resources.textures.find(Instance(color.texture, request));
            return texture != resources.textures.end() && !texture->second.destroyRequested && texture->second.graphFramebuffer != 0;
        }

        /** @brief Encodes a previously admitted whole-color attachment with complete scoped state isolation. */
        [[nodiscard]] bool EncodeColor(const PrimaryOutputAttachment &operations, const FramebufferExtent extent,
                                       const std::uint32_t framebuffer, const OpenGLCommandFunctions &functions) {
            OpenGLExecutionState state{functions};
            if (!state.Apply(extent))
                return false;
            if (framebuffer != 0) {
                functions.state.bindDrawFramebuffer(framebuffer);
                functions.state.drawBuffer(GL_COLOR_ATTACHMENT0);
            }
            if (operations.loadOperation == AttachmentLoadOperation::Clear) {
                const auto &color = operations.clearColor;
                functions.clearColor(color.red, color.green, color.blue, color.alpha);
                functions.clear(GL_COLOR_BUFFER_BIT);
            }
            return true;
        }
    }  // namespace

    /** @copydoc CreateOpenGLGraphFramebuffer */
    Result<std::uint32_t> CreateOpenGLGraphFramebuffer(const OpenGLCommandFunctions &functions, const std::uint32_t texture) {
        DrawTargetRestore restore{functions};
        std::uint32_t framebuffer{};
        const auto destroy = [&functions](const std::uint32_t *object) {
            if (*object != 0)
                functions.framebuffers.deleteFramebuffers(1, object);
        };
        std::unique_ptr<std::uint32_t, decltype(destroy)> rollback{&framebuffer, destroy};
        functions.framebuffers.generateFramebuffers(1, &framebuffer);
        if (framebuffer != 0) {
            functions.framebuffers.bindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
            functions.framebuffers.framebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0);
            functions.framebuffers.drawBuffer(GL_COLOR_ATTACHMENT0);
        }
        if (framebuffer == 0 || functions.framebuffers.checkFramebuffer(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
            functions.state.error() != GL_NO_ERROR)
            return Result<std::uint32_t>::Failure(MakeError(OpenGLBackendErrors::ResourceCreationFailed));
        static_cast<void>(rollback.release());
        return Result<std::uint32_t>::Success(framebuffer);
    }

    /** @copydoc ValidateOpenGLGraphWorkloads */
    Result<void> ValidateOpenGLGraphWorkloads(const RenderGraphExecutionRequest &request, const OpenGLGraphResources &resources) {
        if (!request.resources.empty() && request.lease == nullptr)
            return Result<void>::Failure(MakeError(OpenGLBackendErrors::InvalidExecutionPlan));
        if (const auto portable = ValidateRenderGraphExecutionRequest(request); portable.HasError())
            return portable;
        if (request.lease != nullptr && request.transfer == nullptr)
            return Result<void>::Failure(MakeError(OpenGLBackendErrors::InvalidExecutionPlan));
        for (std::size_t index = 0; index < request.resources.size(); ++index) {
            const auto instance = request.resources[index].instance;
            if (instance == 0)
                continue;
            if (instance > std::numeric_limits<std::uint32_t>::max())
                return Result<void>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            const auto kind = request.graph.Resources()[index].kind;
            const auto name = static_cast<std::uint32_t>(instance);
            if ((kind == RenderGraphResourceKind::Buffer && !resources.buffers.contains(name)) ||
                (kind == RenderGraphResourceKind::Texture && !resources.textures.contains(name)))
                return Result<void>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
        }
        for (const auto &binding : request.workloads) {
            if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&binding.workload)) {
                if (!ValidCopy(*copy, request, resources))
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::InvalidExecutionPlan));
            } else if (const auto *color = std::get_if<RenderGraphColorAttachment>(&binding.workload)) {
                if (!ValidColor(*color, request, resources))
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc ExecuteOpenGLGraphWorkloads */
    Result<void> ExecuteOpenGLGraphWorkloads(const RenderGraphExecutionRequest &request, const OpenGLGraphResources &resources,
                                             const OpenGLCommandFunctions &functions, const FramebufferExtent outputExtent) {
        if (functions.state.error() != GL_NO_ERROR)
            return Result<void>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
        for (const auto &binding : request.workloads) {
            if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&binding.workload)) {
                CopyBindingsRestore restore{functions};
                functions.buffers.bindBuffer(GL_COPY_READ_BUFFER, Instance(copy->source, request));
                functions.buffers.bindBuffer(GL_COPY_WRITE_BUFFER, Instance(copy->destination, request));
                functions.buffers.copyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, copy->sourceOffset, copy->destinationOffset,
                                                    copy->byteCount);
            } else if (const auto *color = std::get_if<RenderGraphColorAttachment>(&binding.workload)) {
                const auto &texture = resources.textures.at(Instance(color->texture, request));
                if (!EncodeColor(color->operations, texture.descriptor.extent, texture.graphFramebuffer, functions))
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
            } else if (const auto *primary = std::get_if<PrimaryOutputAttachment>(&binding.workload)) {
                if (!EncodeColor(*primary, outputExtent, 0, functions))
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
            }
            if (functions.state.error() != GL_NO_ERROR)
                return Result<void>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
