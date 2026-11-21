#pragma once

/** @file ClangdProvider.h @brief Host-side discovery and compilation metadata for the Clangd adapter. */

#include "ProjectLanguageWorkspace.h"

namespace Horo::Editor::Language {
    inline constexpr auto ClangdProviderId = "clangd";

    /** @brief Trusted host inputs for Clangd resolution, independent of project-authored settings. */
    struct ClangdSettings final {
        std::optional<std::filesystem::path> executable;
        std::optional<std::filesystem::path> compileCommands;
        std::vector<std::filesystem::path> searchDirectories;
        bool backgroundIndex{true};
    };

    /** @brief Captures process-level Clangd choices and native host executable search paths. */
    [[nodiscard]] ClangdSettings CaptureClangdHostSettings();

    /** @brief Resolves a native Clangd executable and compilation database before creating a language provider.
     * @param sources Project source owner supplying the canonical root and native suffix policy.
     * @param settings Host-supplied settings. Relative compile commands are resolved within the project.
     * @return Launch descriptor or an actionable, non-fatal availability failure.
     * @details Runs on project activation or explicit reconfiguration. It performs filesystem queries
     * but launches nothing and never treats workspace files as executable configuration.
     */
    [[nodiscard]] Result<ProjectLanguageProvider> PrepareClangdProvider(const SourceFileOpenService &sources,
                                                                        const ClangdSettings &settings);
}  // namespace Horo::Editor::Language
