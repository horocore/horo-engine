#pragma once
#include "Horo/Assets/AssetSourcePublication.h"
#include "ShaderHotReloadSource.h"

namespace Horo::Application::Internal::ShaderSourceDetail {
    inline constexpr std::size_t MaximumPrograms = 8;
    inline constexpr std::size_t MaximumCapturedBytes = 64U * 1024U * 1024U;
    inline constexpr std::size_t MaximumPathUnits = 4096;
    inline constexpr std::size_t MaximumIdentityBytes = 256;
    [[nodiscard]] Error SourceError(const char *code, const char *message);
    [[nodiscard]] Result<void> ValidateRecipe(const ShaderHotReloadSourceRecipe &recipe);
    [[nodiscard]] Result<std::filesystem::path> SourcePath(const std::filesystem::path &root, const std::filesystem::path &relative);
    [[nodiscard]] Result<std::size_t> AdmitSourceSizes(const ShaderHotReloadSourceRecipe &recipe, const std::filesystem::path &root,
                                                       const CancellationToken &cancellation);
}  // namespace Horo::Application::Internal::ShaderSourceDetail
