#pragma once
/** @file ShaderHotReloadSource.h @brief Explicit worker-only bounded canonical shader source capture. */
#include "Horo/Application/ShaderHotReloadService.h"

#include <filesystem>

namespace Horo::Application::Internal {
    struct ShaderHotReloadSourceDependency final {
        std::string logicalPath;
        std::filesystem::path relativePath;
    };

    /** @brief Compiler blueprint contains metadata only; source and diagnostic maps are captured by this sole producer. */
    struct ShaderHotReloadSourceProgram final {
        Render::ShaderHotReloadProgramId id;
        ShaderBuildRequest blueprint;
        std::filesystem::path relativeSource;
        std::vector<ShaderHotReloadSourceDependency> dependencies;
    };

    struct ShaderHotReloadSourceRecipe final {
        std::filesystem::path root;
        std::vector<ShaderHotReloadSourceProgram> programs;
        std::size_t maximumCapturedBytes{16U * 1024U * 1024U};
    };

    /** @brief Validates the entire metadata envelope without reading files or copying its closure. */
    [[nodiscard]] Result<void> ValidateShaderHotReloadSourceRecipe(const ShaderHotReloadSourceRecipe &recipe);
    /** @brief Captures an immutable observed program family on a worker; no compiler or native work runs here.
     * @details All counts, metadata lengths and file sizes are admitted before copying any blueprint/source bytes.
     * Each read is additionally bounded by the remaining aggregate allowance through Assets' canonical source reader.
     * Temporary byte conversion retains at most one additional bounded file. Reads do not claim an OS-atomic multi-file
     * snapshot or instantaneous disk freshness: the host's latest accepted source-generation fence governs adoption.
     */
    [[nodiscard]] Result<ShaderHotReloadRequest> CaptureShaderHotReloadSources(const ShaderHotReloadSourceRecipe &recipe,
                                                                               const ShaderHotReloadFence &fence,
                                                                               const CancellationToken &cancellation = {});
}  // namespace Horo::Application::Internal
