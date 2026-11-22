#pragma once

/** @file ClangdProvider.h @brief Host-side discovery and compilation metadata for the Clangd adapter. */

#include "ProjectLanguageWorkspace.h"

#include <stop_token>

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

    /** @brief Immutable inputs for a one-file diagnostic preview in a separate language session. */
    struct ClangdPreviewSource final {
        const std::filesystem::path &projectRoot;
        const SourceDocumentId &path;
        const SourceDocumentSnapshot &base; /**< Inert for a new file, current for an edit or rename. */
        std::string_view text;
    };

    /** @brief Checks detached proposal bytes with the host's Clangd and compilation database.
     * @param runner Native process authority retained by the worker.
     * @param settings Trusted Clangd discovery choices. Background indexing is disabled for previews.
     * @param source Exact proposed file and current base evidence.
     * @param stop Cooperative worker retirement.
     * @return A versioned diagnostic result projected onto the proposed bytes, or a typed failure.
     * @details Call off the editor lane. No preview is saved and the active language session is untouched.
     */
    [[nodiscard]] Result<LanguageDiagnostics> CheckClangdPreview(IExternalProcessRunner &runner, ClangdSettings settings,
                                                                 const ClangdPreviewSource &source, std::stop_token stop);
}  // namespace Horo::Editor::Language
