/** @file SourceSyntaxProjection.h
 * @brief Exact document-to-display lexical projection at the private widget boundary.
 */
#pragma once
#include "Horo/Editor/SourceDocumentService.h"
#include "editor/source_editor/SourceSyntax.h"

#include <optional>

namespace Horo::Editor {
    /** @brief Projects original UTF-8 spans into the widget's BOM-free, LF-normalized byte coordinates in one pass.
     * @param source Exact authoritative source publication. @param syntax Original document-relative spans.
     * @return A normalized copy, or no value for unordered, oversized, scalar/BOM/CRLF-splitting input.
     * @note Empty/fallback spans require no source-byte traversal. Call on publication changes, never per glyph/frame.
     */
    [[nodiscard]] std::optional<SourceSyntax> ProjectSourceSyntaxForDisplay(const SourceDocumentSnapshot &source,
                                                                            const SourceSyntax &syntax);
}  // namespace Horo::Editor
