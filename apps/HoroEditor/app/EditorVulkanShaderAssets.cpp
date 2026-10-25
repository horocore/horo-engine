#include "EditorVulkanShaderAssets.h"

#include <bit>
#include <fstream>

namespace Horo::Editor {
    namespace {
        Error BundleError() {
            return Error{.code = ErrorCode{"editor.renderer.viewport_shader_unavailable"},
                         .domain = ErrorDomainId{"horo.editor.renderer"},
                         .severity = ErrorSeverity::Critical,
                         .message = "The packaged Vulkan viewport shader bundle is missing, oversized or unreadable. Run the admitted "
                                    "offline cook and package both artifacts."};
        }

        Result<std::vector<std::uint8_t>> ReadArtifact(const std::filesystem::path &path, const std::size_t maximum) {
            std::ifstream input{path, std::ios::binary | std::ios::ate};
            if (!input)
                return Result<std::vector<std::uint8_t>>::Failure(BundleError());
            const auto size = input.tellg();
            if (size <= 0 || static_cast<std::uint64_t>(size) > maximum)
                return Result<std::vector<std::uint8_t>>::Failure(BundleError());
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            input.seekg(0);
            if (!input.read(std::bit_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                return Result<std::vector<std::uint8_t>>::Failure(BundleError());
            return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
        }
    }  // namespace

    /** @copydoc LoadPackagedVulkanViewportShader */
    Result<EditorVulkanViewportCook> LoadPackagedVulkanViewportShader(const std::filesystem::path &assetRoot) {
        const auto payload = ReadArtifact(assetRoot / "shaders/editor/viewport-vulkan.horoshdr", 16U * 1024U * 1024U);
        if (payload.HasError())
            return Result<EditorVulkanViewportCook>::Failure(payload.ErrorValue());
        const auto metadata = ReadArtifact(assetRoot / "shaders/editor/viewport-vulkan.reflection.json", 1024U * 1024U);
        if (metadata.HasError())
            return Result<EditorVulkanViewportCook>::Failure(metadata.ErrorValue());
        return LoadEditorVulkanViewportCook(payload.Value(), metadata.Value());
    }
}  // namespace Horo::Editor
