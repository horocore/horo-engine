#pragma once

/** @file LspDocumentSync.h @brief Encoding of source-document synchronization notifications. */

#include "Horo/Editor/SourceDocumentService.h"
#include "LspLifecycle.h"

namespace Horo::Editor::Language {
    /** @brief Constructs a wire name from a validated file root URI and project-relative source identity. */
    [[nodiscard]] std::string DocumentUri(std::string_view root, std::string_view path);

    /** @brief Translates a byte boundary in validated UTF-8 source text to UTF-16 coordinates.
     * @param text Snapshot bytes. @param offset Byte boundary in that snapshot.
     * @return Position object or no position for an offset inside a code point or CRLF pair.
     */
    [[nodiscard]] std::optional<nlohmann::json> LspPositionAt(std::string_view text, std::size_t offset);

    /** @brief An open or change frame, optionally preceded by retirement of the previous URI. */
    struct LspDocumentUpdate final {
        std::optional<std::string> close;
        std::string update;
    };

    /** @brief Prepares document frames without advancing the source session.
     * @param rootUri Host-supplied absolute project file URI. @param languageId Adapter-selected LSP language ID.
     * @param current Current source snapshot. @param previous Exact peer baseline, if any.
     * @param mode Negotiated writable synchronization strategy. @return Prepared frames or a typed refusal.
     */
    [[nodiscard]] Result<LspDocumentUpdate> BuildLspDocumentUpdate(std::string_view rootUri, std::string_view languageId,
                                                                   const SourceDocumentSnapshot &current,
                                                                   const std::optional<SourceDocumentSnapshot> &previous, LspTextSync mode);

    /** @brief Encodes retirement of one exact project-relative source URI.
     * @param rootUri Host-supplied absolute project file URI. @param source Canonical source identity.
     * @return A complete didClose frame or a typed refusal.
     */
    [[nodiscard]] Result<std::string> BuildLspDocumentClose(std::string_view rootUri, const SourceDocumentId &source);
}  // namespace Horo::Editor::Language
