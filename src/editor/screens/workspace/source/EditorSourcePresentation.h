#pragma once

/** @file EditorSourcePresentation.h @brief Shared embedded text navigation over the current source and tab owners. */

#include "../commands/EditorSourceCommandEndpoint.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Editor/WorkspacePanelHost.h"
#include "editor/language/LspResultProjection.h"

namespace Horo::Editor {
    /** @brief Converts an optional source command point into the shared embedded source range. */
    [[nodiscard]] inline std::optional<Language::LanguageLocation> SourceCommandLocation(
        const SourceDocumentId &source, const std::optional<EditorSourceCommandPosition> position) {
        if (!position)
            return std::nullopt;
        const Language::LanguagePosition point{position->line, position->utf16Column};
        return Language::LanguageLocation{source, point, point};
    }

    /** @brief Opens or focuses admitted project text in the embedded editor.
     * @param sources Current project document owner. @param tabs Its workspace tab owner.
     * @param source Portable source path. @param target Optional range in that source.
     * @param navigate Optional host presentation approval. @param stop Live cancellation view.
     * @return A successful focus lease (null for open-only), or a typed error.
     */
    [[nodiscard]] Result<std::shared_ptr<const Language::LanguageSourceFocus>> PresentEditorSource(
        SourceFileOpenService &sources, WorkspacePanelHost &tabs, const SourceDocumentId &source,
        std::optional<Language::LanguageLocation> target, const std::function<bool(const SourceOpenResult &)> &navigate,
        const std::function<bool()> &stop);
}  // namespace Horo::Editor
