/** @file SourceSyntaxProjection.h
 * @brief Exact document-to-display lexical projection at the private widget boundary.
 */
#pragma once
#include "Horo/Editor/SourceDocumentService.h"
#include "SourceSyntax.h"

#include <cstdint>
#include <optional>

namespace Horo::Editor {
    /** @brief Projects original UTF-8 spans into the widget's BOM-free, LF-normalized byte coordinates in one pass.
     * @param source Exact authoritative source publication. @param syntax Original document-relative spans.
     * @return A normalized copy, or no value for unordered, oversized, scalar/BOM/CRLF-splitting input.
     * @note Empty/fallback spans require no source-byte traversal. Call on publication changes, never per glyph/frame.
     */
    [[nodiscard]] std::optional<SourceSyntax> ProjectSourceSyntaxForDisplay(const SourceDocumentSnapshot &source,
                                                                            const SourceSyntax &syntax);

    /** @brief Couples one validated display projection to its authoritative revision and exact mirror update. */
    class SourceSyntaxPresentation final {
    public:
        /** @brief Replaces the projection atomically after validating its original coordinates.
         * @param source Authoritative publication. @param mirrorUpdate Current mirror generation. @param syntax Original spans.
         * @return Whether the replacement was admitted; rejection retains the previous projection.
         */
        bool Publish(const SourceDocumentSnapshot &source, std::uint64_t mirrorUpdate, const SourceSyntax &syntax);
        /** @brief Tests both publication and mirror fences. @param revision Source revision. @param mirrorUpdate Mirror generation.
         * @return Whether the stored projection belongs to both fences.
         */
        [[nodiscard]] bool Contains(std::uint64_t revision, std::uint64_t mirrorUpdate) const noexcept;
        /** @brief Borrows styling only for the unchanged mirror. @param mirrorUpdate Mirror generation.
         * @return Stored projection, or null while an edit awaits publication.
         */
        [[nodiscard]] const SourceSyntax *ForMirror(std::uint64_t mirrorUpdate) const noexcept;
        /** @brief Clears styling when replacing the widget's document owner. */
        void Reset() noexcept;

    private:
        SourceSyntax syntax_;
        std::uint64_t revision_{};
        std::uint64_t mirrorUpdate_{};
    };
}  // namespace Horo::Editor
