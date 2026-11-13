#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <algorithm>
#include <limits>

namespace Horo::Render {
    namespace {
        /** @brief Checks the tight RGBA8 product before multiplication can overflow the host byte extent. */
        bool UploadBytesValid(const RenderGraphTextureUpload &upload) noexcept {
            if (!upload.extent.IsValid())
                return false;
            const auto pixels = std::uint64_t{upload.extent.width} * upload.extent.height;
            return pixels <= std::numeric_limits<std::size_t>::max() / 4U && upload.byteCount == pixels * 4U;
        }

        /** @brief Admits exactly one whole single-sample RGBA8 color base level. */
        bool UploadTextureValid(const RenderGraphTextureUpload &upload, const RenderTextureDescriptor &texture) noexcept {
            return texture.IsValid() && texture.format == RenderTextureFormat::Rgba8Unorm &&
                   texture.dimension == RenderTextureDimension::TwoD && texture.extent == upload.extent && texture.depth == 1 &&
                   texture.mipCount == 1 && texture.layerCount == 1 && texture.sampleCount == 1 &&
                   HasTextureUsage(texture.usage, RenderTextureUsage::CopyDestination);
        }

        /** @brief Resolves only an exact graph-local resource kind before any indexed access. */
        bool UploadResourceKind(const RenderGraphExecutionRequest &request, RenderGraphResourceId id,
                                RenderGraphResourceKind kind) noexcept {
            return id.owner == request.graph.Owner() && id.value > 0 && id.value <= request.graph.Resources().size() &&
                   request.graph.Resources()[id.value - 1].kind == kind;
        }

        /** @brief Matches one exact source/destination transfer use; duplicate or extra uses are denied by the enclosing count. */
        bool UploadUse(std::span<const RenderGraphResourceUsage> uses, RenderGraphResourceId id, RenderGraphUsageKind kind,
                       RenderGraphAccess access) noexcept {
            const auto found = std::ranges::find(uses, id, &RenderGraphResourceUsage::resource);
            return found != uses.end() && found->kind == kind && found->access == access;
        }
    }  // namespace

    /** @copydoc ValidateTextureUploadDescriptors */
    Result<void> ValidateTextureUploadDescriptors(const RenderGraphTextureUpload &upload, const RenderBufferDescriptor &source,
                                                  const RenderTextureDescriptor &destination) {
        if (!source.IsValid() || source.access != RenderBufferAccess::HostVisible ||
            !HasBufferUsage(source.usage, RenderBufferUsage::CopySource) || source.byteSize != upload.byteCount)
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph,
                                                   "Texture upload requires the exact whole registered host-visible CopySource buffer."));
        if (!UploadBytesValid(upload) || !UploadTextureValid(upload, destination))
            return Result<void>::Failure(
                MakeError(RenderGraphExecutionErrors::InvalidGraph,
                          "Texture upload requires exact tightly packed whole RGBA8 2D base-level bounds and CopyDestination usage."));
        return Result<void>::Success();
    }

    /** @copydoc ValidateTextureUploadGraphWorkload */
    Result<void> ValidateTextureUploadGraphWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                                    const RenderGraphTextureUpload &upload) {
        if (pass.kind != RenderPassKind::Copy || !pass.usages.IsValidFor(request.graph.Usages().size()) || !UploadBytesValid(upload))
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        if (!UploadResourceKind(request, upload.source, RenderGraphResourceKind::Buffer) ||
            !UploadResourceKind(request, upload.destination, RenderGraphResourceKind::Texture))
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        if (const auto uses = request.graph.Usages().subspan(pass.usages.offset, pass.usages.count);
            uses.size() != 2 || !UploadUse(uses, upload.source, RenderGraphUsageKind::CopySource, RenderGraphAccess::Read) ||
            !UploadUse(uses, upload.destination, RenderGraphUsageKind::CopyDestination, RenderGraphAccess::Write))
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        return Result<void>::Success();
    }
}  // namespace Horo::Render
