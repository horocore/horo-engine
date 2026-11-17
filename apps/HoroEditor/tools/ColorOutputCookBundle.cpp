#include "ColorOutputCookInternal.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

namespace Horo::Editor::Offline::ColorCookDetail {
    /** @copydoc Require */
    void Require(bool valid) {
        if (!valid)
            throw Failure{Invalid()};
    }

    /** @copydoc Invalid */
    Error Invalid() {
        return MakeError(Render::ShaderCompilerPipelineErrors::ToolOutputInvalid,
                         "Color output generation is malformed, inconsistent or escapes its explicit root.");
    }

    /** @copydoc Root */
    std::filesystem::path Root(const std::filesystem::path &root) {
        return Take(Render::Internal::ResolveColorPackageRoot(root));
    }

    /** @copydoc Read */
    Bytes Read(const std::filesystem::path &root, std::string_view relative, std::size_t maximum) {
        return Take(Render::Internal::ReadColorPackageFile(root, relative, maximum));
    }

    /** @copydoc Hash */
    Sha256Digest Hash(const Bytes &bytes) noexcept {
        return ComputeSha256(std::as_bytes(std::span{bytes}));
    }
}  // namespace Horo::Editor::Offline::ColorCookDetail

namespace Horo::Editor::Offline {
    /** @copydoc ColorOutputAnalyticFilenames */
    std::span<const std::string_view, 7> ColorOutputAnalyticFilenames() noexcept {
        return Render::Internal::ColorOutputAnalyticFilenames();
    }

    /** @copydoc LoadColorOutputAnalyticBundle */
    Result<ColorOutputAnalyticBundle> LoadColorOutputAnalyticBundle(const std::filesystem::path &root) {
        return Render::Internal::LoadColorOutputAnalyticBundle(root);
    }

    /** @copydoc LoadColorOutputCookPackage */
    Result<ColorOutputCookPackage> LoadColorOutputCookPackage(const std::filesystem::path &root) {
        return Render::Internal::LoadColorOutputCookPackage(root);
    }
}  // namespace Horo::Editor::Offline
