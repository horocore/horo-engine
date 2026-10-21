#include "SourceSyntaxProjection.h"

#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Rejects syntax positions that split one authored encoding or newline unit. */
        [[nodiscard]] bool Boundary(const std::string_view text, const std::size_t offset, const std::size_t origin) noexcept {
            if (offset < origin || offset > text.size())
                return false;
            if (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U)
                return false;
            return offset == 0 || offset == text.size() || text[offset - 1] != '\r' || text[offset] != '\n';
        }

        /** @brief Validates one ordered lexical range against the exact document encoding. */
        [[nodiscard]] bool ValidSpan(const std::string_view text, const SourceSyntaxSpan &span, const std::size_t previous,
                                     const std::size_t origin) noexcept {
            if (span.begin < previous || span.end <= span.begin || span.role >= SourceSyntaxRole::Count)
                return false;
            return Boundary(text, span.begin, origin) && Boundary(text, span.end, origin);
        }

        /** @brief Validates the complete external projection before constructing its replacement. */
        [[nodiscard]] bool Valid(const SourceDocumentSnapshot &source, const SourceSyntax &syntax, const std::size_t origin) noexcept {
            if (syntax.spans.empty())
                return true;
            if (source.Revision() == 0 || source.Text().size() > SourceSyntaxLimits{}.maximumBytes ||
                syntax.spans.size() > SourceSyntaxLimits{}.maximumSpans)
                return false;
            std::size_t previous = origin;
            for (const auto &span : syntax.spans) {
                if (!ValidSpan(source.Text(), span, previous, origin))
                    return false;
                previous = span.end;
            }
            return true;
        }
    }  // namespace

    /** @copydoc ProjectSourceSyntaxForDisplay */
    std::optional<SourceSyntax> ProjectSourceSyntaxForDisplay(const SourceDocumentSnapshot &source, const SourceSyntax &syntax) {
        const auto text = source.Text();
        std::size_t raw = source.Metadata().utf8Bom ? 3 : 0;
        if (!Valid(source, syntax, raw))
            return std::nullopt;
        SourceSyntax result = syntax;
        std::size_t display = 0;
        const auto project = [&](const std::size_t target) {
            while (raw < target) {
                if (text[raw] == '\r' && raw + 1 < target && text[raw + 1] == '\n')
                    ++raw;
                ++raw;
                ++display;
            }
            return display;
        };
        for (auto &span : result.spans) {
            span.begin = project(span.begin);
            span.end = project(span.end);
        }
        return result;
    }

    /** @copydoc SourceSyntaxPresentation::Publish */
    bool SourceSyntaxPresentation::Publish(const SourceDocumentSnapshot &source, const std::uint64_t mirrorUpdate,
                                           const SourceSyntax &syntax) {
        auto projected = ProjectSourceSyntaxForDisplay(source, syntax);
        if (!projected)
            return false;
        syntax_ = std::move(*projected);
        revision_ = source.Revision();
        mirrorUpdate_ = mirrorUpdate;
        return true;
    }

    /** @copydoc SourceSyntaxPresentation::Contains */
    bool SourceSyntaxPresentation::Contains(const std::uint64_t revision, const std::uint64_t mirrorUpdate) const noexcept {
        return revision_ != 0 && revision_ == revision && mirrorUpdate_ == mirrorUpdate;
    }

    /** @copydoc SourceSyntaxPresentation::ForMirror */
    const SourceSyntax *SourceSyntaxPresentation::ForMirror(const std::uint64_t mirrorUpdate) const noexcept {
        return Contains(revision_, mirrorUpdate) ? &syntax_ : nullptr;
    }

    /** @copydoc SourceSyntaxPresentation::Reset */
    void SourceSyntaxPresentation::Reset() noexcept {
        syntax_ = {};
        revision_ = 0;
        mirrorUpdate_ = 0;
    }
}  // namespace Horo::Editor
