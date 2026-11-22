#pragma once

/** @file HoroScriptModuleLanguage.h @brief Revision-bound editor queries over registered script API descriptions. */

#include "editor/language/LspResultProjection.h"

#include <span>

namespace Horo::Extensions {
    struct ScriptModuleApiMetadata;
}

namespace Horo::Editor::Language {
    /** @brief Projects completion or hover for an explicit Lua import member expression.
     * @param source Current source publication.
     * @param kind Completion or hover.
     * @param offset UTF-8 byte position of the caret.
     * @param imports Read-only descriptions of the selected, authenticated module's actual imports.
     * @return Exact source result, an empty feature result when no matching member exists, or a typed query failure.
     * @details Only reads source text and metadata. The caller owns import generation and project lifetime.
     */
    [[nodiscard]] Result<LanguageResult> QueryHoroScriptImports(const SourceDocumentSnapshot &source, SourceLanguageRequestKind kind,
                                                                std::size_t offset,
                                                                std::span<const Extensions::ScriptModuleApiMetadata> imports);
}  // namespace Horo::Editor::Language
