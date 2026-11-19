#include "EditorVulkanShaderAssets.h"

#include <bit>
#include <fstream>

namespace Horo::Editor {
    namespace {
        Error BundleError() {
            return Error{.code = ErrorCode{"editor.renderer.viewport_shader_unavailable"},
                         .domain = ErrorDomainId{"horo.editor.renderer"},
                         .severity = ErrorSeverity::Critical,
                         .message = "The packaged Vulkan shader bundle is missing, oversized or unreadable. Run the admitted "
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

        Result<EditorVulkanShadowCook> LoadPackagedShadow(const std::filesystem::path &assetRoot) {
            const auto payload = ReadArtifact(assetRoot / "shaders/editor/viewport-shadow-vulkan.horoshdr", 8U * 1024U * 1024U);
            if (payload.HasError())
                return Result<EditorVulkanShadowCook>::Failure(payload.ErrorValue());
            const auto metadata = ReadArtifact(assetRoot / "shaders/editor/viewport-shadow-vulkan.reflection.json", 1024U * 1024U);
            if (metadata.HasError())
                return Result<EditorVulkanShadowCook>::Failure(metadata.ErrorValue());
            return LoadEditorVulkanShadowCook(payload.Value(), metadata.Value());
        }

        /** @brief Reads a bounded runtime pair and delegates identity/interface validation to its actual program loader. */
        template <typename Cook>
        Result<Cook> LoadPackagedProgram(const std::filesystem::path &prefix,
                                         Result<Cook> (*load)(std::span<const std::uint8_t>, std::span<const std::uint8_t>)) {
            auto payloadPath = prefix;
            payloadPath += ".horoshdr";
            const auto payload = ReadArtifact(payloadPath, 16U * 1024U * 1024U);
            if (payload.HasError())
                return Result<Cook>::Failure(payload.ErrorValue());
            auto metadataPath = prefix;
            metadataPath += ".reflection.json";
            const auto metadata = ReadArtifact(metadataPath, 1024U * 1024U);
            if (metadata.HasError())
                return Result<Cook>::Failure(metadata.ErrorValue());
            return load(payload.Value(), metadata.Value());
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
        auto viewport = LoadEditorVulkanViewportCook(payload.Value(), metadata.Value());
        if (viewport.HasError())
            return viewport;
        auto shadow = LoadPackagedShadow(assetRoot);
        if (shadow.HasError())
            return Result<EditorVulkanViewportCook>::Failure(shadow.ErrorValue());
        auto cooked = std::move(viewport).Value();
        cooked.shadow.emplace(std::move(shadow).Value());
        return Result<EditorVulkanViewportCook>::Success(std::move(cooked));
    }

    /** @copydoc LoadPackagedVulkanCameraCrossFadeShader */
    Result<EditorVulkanCameraCrossFadeCook> LoadPackagedVulkanCameraCrossFadeShader(const std::filesystem::path &assetRoot) {
        return LoadPackagedProgram(assetRoot / "shaders/editor/viewport-cross-fade-vulkan", &LoadEditorVulkanCameraCrossFadeCook);
    }

    /** @copydoc LoadPackagedVulkanSceneLinearShader */
    Result<EditorVulkanSceneLinearCook> LoadPackagedVulkanSceneLinearShader(const std::filesystem::path &assetRoot) {
        return LoadPackagedProgram(assetRoot / "shaders/editor/viewport-scene-linear-vulkan", &LoadEditorVulkanSceneLinearCook);
    }

    /** @copydoc LoadPackagedVulkanMultiviewShader */
    Result<EditorVulkanMultiviewCook> LoadPackagedVulkanMultiviewShader(const std::filesystem::path &assetRoot) {
        return LoadPackagedProgram(assetRoot / "shaders/runtime/multiview-vulkan", &LoadEditorVulkanMultiviewCook);
    }

    /** @copydoc LoadPackagedVulkanMultiviewMotionShader */
    Result<EditorVulkanMultiviewCook> LoadPackagedVulkanMultiviewMotionShader(const std::filesystem::path &assetRoot) {
        return LoadPackagedProgram(assetRoot / "shaders/runtime/multiview-motion-vulkan", &LoadEditorVulkanMultiviewMotionCook);
    }

    /** @copydoc LoadPackagedVulkanMultiviewCoverageShader */
    Result<EditorVulkanMultiviewCook> LoadPackagedVulkanMultiviewCoverageShader(const std::filesystem::path &assetRoot) {
        return LoadPackagedProgram(assetRoot / "shaders/runtime/multiview-coverage-vulkan", &LoadEditorVulkanMultiviewCoverageCook);
    }

    /** @copydoc LoadPackagedVulkanCoverageReductionShader */
    Result<EditorVulkanCoverageReductionCook> LoadPackagedVulkanCoverageReductionShader(const std::filesystem::path &assetRoot) {
        return LoadPackagedProgram(assetRoot / "shaders/runtime/multiview-coverage-reduction-vulkan",
                                   &LoadEditorVulkanCoverageReductionCook);
    }
}  // namespace Horo::Editor
