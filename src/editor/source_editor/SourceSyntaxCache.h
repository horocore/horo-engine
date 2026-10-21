/** @file SourceSyntaxCache.h
 * @brief View-owner lexical projection cache, separate from widget interaction and language intelligence.
 */
#pragma once
#include "Horo/Editor/SourceDocumentService.h"
#include "editor/source_editor/SourceSyntax.h"

namespace Horo::Editor {
    /** @brief One publication lease and its lexical spans, owned by a source view on the editor thread. */
    class SourceSyntaxCache final {
    public:
        /** @brief Updates only for a different authoritative publication, including same-revision document switches.
         * @param source Current document publication; inert snapshots detach the projection.
         * @return True when a projection was replaced; false for unchanged publication roots.
         */
        bool Publish(const SourceDocumentSnapshot &source) {
            if (source_.SharesStateWith(source))
                return false;
            syntax_ = source.Revision() == 0 ? SourceSyntax{}
                                             : TokenizeSource(ClassifySourceLanguage(source.Identity().key.source.Value()), source.Text());
            source_ = source;
            return true;
        }

        /** @brief Returns immutable decorations of the retained exact publication. @return Original document-byte spans. */
        [[nodiscard]] const SourceSyntax &Syntax() const noexcept {
            return syntax_;
        }

    private:
        SourceDocumentSnapshot source_;
        SourceSyntax syntax_;
    };
}  // namespace Horo::Editor
