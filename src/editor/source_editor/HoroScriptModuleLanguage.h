#pragma once

/** @file HoroScriptModuleLanguage.h @brief Revision-bound editor queries over registered script API descriptions. */

#include "editor/language/LspResultProjection.h"

#include <span>

namespace Horo::Extensions {
    struct ScriptModuleApiMetadata;
}

namespace Horo::Editor::Language {
    /** @brief Projects completion or hover for Lua host members, import aliases and explicit imported members.
     * @param source Current source publication.
     * @param kind Completion or hover.
     * @param offset UTF-8 byte position of the caret.
     * @param imports Read-only descriptions of the selected, authenticated module's actual imports.
     * @return Exact source result, an empty feature result when no matching member exists, or a typed query failure.
     * @details Only reads source text and metadata. The caller owns import generation and project lifetime.
     */
    [[nodiscard]] Result<LanguageResult> QueryHoroScriptApi(const SourceDocumentSnapshot &source, SourceLanguageRequestKind kind,
                                                            std::size_t offset,
                                                            std::span<const Extensions::ScriptModuleApiMetadata> imports);

    /** @brief Finds mismatched namespaces and members in direct Lua imports with available schemas.
     * @param text Syntactically valid Lua source bytes, including the original line endings.
     * @param imports Copied, inert schemas for the registered importing module.
     * @return Bounded warnings with half-open UTF-8 byte ranges in the input.
     */
    [[nodiscard]] LanguageDiagnostics DiagnoseHoroScriptImports(std::string_view text,
                                                                std::span<const Extensions::ScriptModuleApiMetadata> imports);
}  // namespace Horo::Editor::Language
