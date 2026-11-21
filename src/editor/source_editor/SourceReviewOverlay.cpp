#include "SourceReviewOverlay.h"

#include "SourceWidgetText.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        /** @brief Keeps a tooltip on one logical line without splitting UTF-8. */
        std::string Excerpt(const std::string_view text) {
            auto end = std::min({text.size(), std::size_t{160}, text.find_first_of("\r\n\t\f")});
            while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U)
                --end;
            auto excerpt = std::string{text.substr(0, end)};
            if (end < text.size())
                excerpt += "…";
            return excerpt;
        }

        /** @brief Exposes actual removed and added text without reproducing the provider patch. */
        std::string Description(const std::string_view before, const std::string_view after) {
            std::string description;
            if (!before.empty())
                description = "- " + Excerpt(before);
            if (!after.empty()) {
                if (!description.empty())
                    description += '\n';
                description += "+ " + Excerpt(after);
            }
            return description;
        }

        /** @brief Maps original bytes to drawable scalar and line-ending boundaries. */
        SourceReviewChange Change(const SourceDocumentSnapshot &source, const std::size_t offset, const std::string_view before,
                                  const std::string_view after) {
            const auto begin = SourceWidgetText::ClampOffset(source, offset);
            auto end = offset + before.size();
            if (!SourceWidgetText::Boundary(source, end))
                ++end;  // A validated scalar boundary can be inside a CRLF pair.
            const auto kind = before.empty()  ? SourceReviewKind::Added
                              : after.empty() ? SourceReviewKind::Removed
                                              : SourceReviewKind::Modified;
            return {begin, std::max(end, begin), kind, Description(before, after)};
        }

        /** @brief Projects only changed bytes; unified hunk context remains ordinary source text. */
        SourceReviewChange HunkChange(const SourceDocumentSnapshot &source, const Proposals::TextHunk &hunk) {
            const auto [prefix, suffix] = SourceWidgetText::Compare(hunk.before, hunk.after);
            return Change(source, static_cast<std::size_t>(hunk.offset) + prefix,
                          std::string_view{hunk.before}.substr(prefix, hunk.before.size() - prefix - suffix),
                          std::string_view{hunk.after}.substr(prefix, hunk.after.size() - prefix - suffix));
        }

        /** @brief Resolves file-backed edit coordinates and their original path. */
        std::span<const Proposals::TextHunk> Hunks(const Proposals::FileOperation &operation) {
            if (const auto *edit = std::get_if<Proposals::EditFile>(&operation))
                return edit->hunks;
            if (const auto *rename = std::get_if<Proposals::RenameFile>(&operation))
                return rename->hunks;
            return {};
        }
    }  // namespace

    /** @copydoc SourceReviewOverlay::Capture */
    Result<SourceReviewOverlay> SourceReviewOverlay::Capture(const Proposals::ChangeSet &proposal, const Proposals::WorkspaceEditPlan &plan,
                                                             const std::size_t operationIndex) {
        using ResultType = Result<SourceReviewOverlay>;
        const auto files = proposal.Files();
        if (proposal.Digest() != plan.proposal || operationIndex >= files.size() || files.size() != plan.entries.size())
            return ResultType::Failure(MakeError(Proposals::Errors::Invalid));
        const auto *preview = std::get_if<Proposals::FilePreview>(&plan.entries[operationIndex]);
        if (!preview || !preview->before.IsCurrent())
            return ResultType::Failure(MakeError(Proposals::Errors::StaleBase));
        const auto &operation = files[operationIndex];
        const auto *base = std::visit([](const auto &file) -> const Proposals::FileBase * {
            if constexpr (requires { file.base; })
                return &file.base;
            return nullptr;
        }, operation);
        if (!base)
            return ResultType::Failure(MakeError(Proposals::Errors::Invalid));
        const auto hunks = Hunks(operation);
        const auto validated = preview->ValidateSource(*base, hunks);
        if (validated.HasError())
            return ResultType::Failure(validated.ErrorValue());
        SourceReviewOverlay overlay;
        overlay.source_ = preview->before;
        overlay.proposal_ = plan.proposal;
        if (std::holds_alternative<Proposals::DeleteFile>(operation)) {
            const auto begin = overlay.source_.Metadata().utf8Bom ? 3U : 0U;
            auto removed = Change(overlay.source_, begin, overlay.source_.Text().substr(begin), {});
            removed.kind = SourceReviewKind::Removed;
            overlay.changes_.push_back(std::move(removed));
        } else {
            overlay.changes_.reserve(hunks.size());
            for (const auto &hunk : hunks)
                overlay.changes_.push_back(HunkChange(overlay.source_, hunk));
        }
        return ResultType::Success(std::move(overlay));
    }
}  // namespace Horo::Editor
