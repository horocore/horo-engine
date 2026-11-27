#include "Horo/Editor/SourceEditingCommands.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Excludes the retained encoding marker from user-editable text. */
        std::size_t TextStart(const std::string_view text) {
            return text.starts_with("\xef\xbb\xbf") ? 3 : 0;
        }

        /** @brief Validates scalar, BOM and CRLF cursor boundaries. */
        bool Boundary(const std::string_view text, const std::size_t at) {
            return at >= TextStart(text) && at <= text.size() &&
                   (at == text.size() || (static_cast<std::byte>(text[at]) & std::byte{0xc0}) != std::byte{0x80}) &&
                   !(at > 0 && at < text.size() && text[at - 1] == '\r' && text[at] == '\n');
        }

        /** @brief Validates an exact view without allowing nonfinite scroll positions. */
        bool ValidView(const SourceDocumentSnapshot &snapshot, const SourceEditingView &view) {
            return snapshot.Revision() != 0 && Boundary(snapshot.Text(), view.anchor) && Boundary(snapshot.Text(), view.caret) &&
                   std::isfinite(view.scrollLines);
        }

        /** @brief Rejects invalid scalars and binary controls before planning a document edit. */
        bool ValidText(const std::string_view text) {
            return IsValidUtf8ScalarSequence(text) && std::ranges::none_of(text, [](const unsigned char c) {
                return c < 32 && c != '\t' && c != '\r' && c != '\n' && c != '\f';
            });
        }

        /** @brief Returns a stable source-domain failure without source bytes. */
        Result<SourceEditingPlan> Failure(const ErrorCodeDescriptor &error) {
            return Result<SourceEditingPlan>::Failure(MakeError(error));
        }

        /** @brief Finds the beginning of a logical line, never preceding the BOM. */
        std::size_t LineStart(const std::string_view text, std::size_t at) {
            const auto start = TextStart(text);
            while (at > start && text[at - 1] != '\r' && text[at - 1] != '\n')
                --at;
            return at;
        }

        /** @brief Advances past a logical newline without splitting CRLF. */
        std::size_t NextLine(const std::string_view text, std::size_t at) {
            const auto end = text.find_first_of("\r\n", at);
            if (end == std::string_view::npos)
                return text.size();
            return end + (text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n' ? 2 : 1);
        }

        /** @brief Captures the local newline family, falling back to the document's uniform family. */
        std::string NewlineAt(const SourceDocumentSnapshot &snapshot, const std::size_t at) {
            const auto text = snapshot.Text();
            if (const auto end = text.find_first_of("\r\n", at); end != std::string_view::npos) {
                if (text[end] == '\n')
                    return "\n";
                return end + 1 < text.size() && text[end + 1] == '\n' ? "\r\n" : "\r";
            }
            if (snapshot.Metadata().newlines == SourceNewlines::CrLf)
                return "\r\n";
            return snapshot.Metadata().newlines == SourceNewlines::Cr ? "\r" : "\n";
        }

        /** @brief Counts one leading tab or a bounded space indentation prefix. */
        std::size_t IndentationBytes(const std::string_view text, const std::size_t at, const unsigned width) {
            if (at < text.size() && text[at] == '\t')
                return 1;
            std::size_t removed = 0;
            while (removed < width && at + removed < text.size() && text[at + removed] == ' ')
                ++removed;
            return removed;
        }

        /** @brief Advances one complete scalar in already validated UTF-8 text. */
        std::size_t NextScalar(const std::string_view text, std::size_t at) {
            ++at;
            while (at < text.size() && (static_cast<std::byte>(text[at]) & std::byte{0xc0}) == std::byte{0x80})
                ++at;
            return at;
        }

        /** @brief Searches forward from selection end and wraps through the complementary interval once. */
        std::size_t FindForward(const std::string_view text, const std::string_view query, const std::size_t high, const bool wrap) {
            auto found = text.find(query, high);
            if (found == std::string_view::npos && wrap) {
                const auto start = TextStart(text);
                found = text.substr(start, high - start).find(query);
                if (found != std::string_view::npos)
                    found += start;
            }
            return found;
        }

        /** @brief Searches backward before selection start and wraps once at the document end. */
        std::size_t FindBackward(const std::string_view text, const std::string_view query, const std::size_t low, const bool wrap) {
            const auto start = TextStart(text);
            auto found = std::string_view::npos;
            if (query.size() <= low - start)
                found = text.rfind(query, low - query.size());
            if ((found == std::string_view::npos || found < start) && wrap)
                found = text.rfind(query);
            return found;
        }
    }  // namespace

    /** @copydoc SourceEditingPlan::Edit */
    SourceTextEdit SourceEditingPlan::Edit() const noexcept {
        return {revision, offset, eraseBytes, insert, before, after};
    }

    /** @copydoc SubmitSourceEditingCommand */
    Result<SourceEditingPublication> SubmitSourceEditingCommand(SourceDocumentService &documents, const SourceEditingCommand &command,
                                                                const CancellationToken cancellation) {
        SourceDocumentSnapshot snapshot;
        std::optional<SourceEditingView> restoration;
        DocumentInstanceId instance;
        if (const auto *edit = std::get_if<SourceEditSubmission>(&command)) {
            instance = edit->instance;
            auto result = documents.Edit(instance, edit->plan.Edit(), cancellation);
            if (result.HasError())
                return Result<SourceEditingPublication>::Failure(result.ErrorValue());
            snapshot = std::move(result).Value();
            if (snapshot.Revision() != edit->plan.revision)
                restoration = edit->plan.after;
        } else {
            const auto &history = std::get<SourceHistoryRequest>(command);
            instance = history.instance;
            auto result = history.direction == SourceHistoryDirection::Undo
                              ? documents.Undo(instance, history.expectedRevision, cancellation)
                              : documents.Redo(instance, history.expectedRevision, cancellation);
            if (result.HasError())
                return Result<SourceEditingPublication>::Failure(result.ErrorValue());
            snapshot = result.Value().snapshot;
            restoration = result.Value().view;
        }
        const auto history = documents.History(instance);
        if (history.HasError())
            return Result<SourceEditingPublication>::Failure(history.ErrorValue());
        return Result<SourceEditingPublication>::Success({std::move(snapshot), restoration, history.Value()});
    }

    /** @copydoc PlanSourceReplacement */
    Result<SourceEditingPlan> PlanSourceReplacement(const SourceDocumentSnapshot &snapshot, const SourceEditingView &view,
                                                    const std::string_view text) {
        try {
            if (!ValidView(snapshot, view) || !ValidText(text))
                return Failure(SourceDocumentErrors::Invalid);
            const auto begin = std::min(view.anchor, view.caret);
            const auto end = std::max(view.anchor, view.caret);
            if (const auto retained = snapshot.Text().size() - (end - begin);
                text.size() > SourceDocumentLimits{}.maximumDocumentBytes - retained)
                return Failure(SourceDocumentErrors::TooLarge);
            const SourceEditingView after{begin + text.size(), begin + text.size(), view.scrollLines, view.viewId};
            return Result<SourceEditingPlan>::Success({snapshot.Revision(), begin, end - begin, std::string(text), view, after});
        } catch (const std::bad_alloc &) {
            return Failure(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc PlanSourceNewline */
    Result<SourceEditingPlan> PlanSourceNewline(const SourceDocumentSnapshot &snapshot, const SourceEditingView &view) {
        try {
            if (!ValidView(snapshot, view))
                return Failure(SourceDocumentErrors::Invalid);
            const auto text = snapshot.Text();
            const auto low = std::min(view.anchor, view.caret);
            const auto start = LineStart(text, low);
            auto indentEnd = start;
            while (indentEnd < low && (text[indentEnd] == ' ' || text[indentEnd] == '\t'))
                ++indentEnd;
            auto newline = NewlineAt(snapshot, low);
            newline.append(text.substr(start, indentEnd - start));
            return PlanSourceReplacement(snapshot, view, newline);
        } catch (const std::bad_alloc &) {
            return Failure(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc PlanSourceIndentation */
    Result<SourceEditingPlan> PlanSourceIndentation(const SourceDocumentSnapshot &snapshot, const SourceEditingView &view,
                                                    const unsigned width, const bool outdent) {
        try {
            if (!ValidView(snapshot, view) || width == 0 || width > 8)
                return Failure(SourceDocumentErrors::Invalid);
            const auto text = snapshot.Text();
            const auto low = std::min(view.anchor, view.caret);
            const auto high = std::max(view.anchor, view.caret);
            const auto first = LineStart(text, low);
            const auto last = high > low && LineStart(text, high) == high ? high : NextLine(text, high);
            std::string replacement;
            std::size_t anchor = view.anchor;
            std::size_t caret = view.caret;
            std::size_t removedTotal = 0;
            std::size_t insertedTotal = 0;
            auto at = first;
            do {
                const auto next = NextLine(text, at);
                const auto removed = outdent ? IndentationBytes(text, at, width) : 0;
                const auto inserted = outdent ? 0U : width;
                const auto adjust = [&](const std::size_t original, std::size_t &result) {
                    if (original >= at) {
                        result = original - removedTotal + insertedTotal - std::min(removed, original - at) + inserted;
                    }
                };
                adjust(view.anchor, anchor);
                adjust(view.caret, caret);
                replacement.append(inserted, ' ');
                replacement.append(text.substr(at + removed, next - at - removed));
                if (replacement.size() > SourceDocumentLimits{}.maximumDocumentBytes)
                    return Failure(SourceDocumentErrors::TooLarge);
                insertedTotal += inserted;
                removedTotal += removed;
                at = next;
            } while (at < last);
            if (const auto retained = text.size() - (last - first);
                replacement.size() > SourceDocumentLimits{}.maximumDocumentBytes - retained)
                return Failure(SourceDocumentErrors::TooLarge);
            return Result<SourceEditingPlan>::Success(
                {snapshot.Revision(), first, last - first, std::move(replacement), view, {anchor, caret, view.scrollLines, view.viewId}});
        } catch (const std::bad_alloc &) {
            return Failure(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc FindSourceText */
    std::optional<SourceEditingView> FindSourceText(const SourceDocumentSnapshot &snapshot, const SourceEditingView &view,
                                                    const std::string_view query, const SourceSearchDirection direction, const bool wrap) {
        if (!ValidView(snapshot, view) || query.empty() || !ValidText(query))
            return {};
        const auto text = snapshot.Text();
        const auto low = std::min(view.anchor, view.caret);
        const auto high = std::max(view.anchor, view.caret);
        const auto found =
            direction == SourceSearchDirection::Forward ? FindForward(text, query, high, wrap) : FindBackward(text, query, low, wrap);
        if (found == std::string_view::npos || !Boundary(text, found) || !Boundary(text, found + query.size()))
            return {};
        return SourceEditingView{found, found + query.size(), view.scrollLines, view.viewId};
    }

    /** @copydoc PlanSourceReplaceAll */
    Result<SourceEditingPlan> PlanSourceReplaceAll(const SourceDocumentSnapshot &snapshot, const SourceEditingView &view,
                                                   const std::string_view query, const std::string_view replacement) {
        try {
            if (!ValidView(snapshot, view) || query.empty() || !ValidText(query) || !ValidText(replacement))
                return Failure(SourceDocumentErrors::Invalid);
            const auto text = snapshot.Text();
            const auto start = TextStart(text);
            const auto maximum = SourceDocumentLimits{}.maximumDocumentBytes - start;
            std::string output;
            auto append = [&](const std::string_view part) {
                if (part.size() > maximum - output.size())
                    return false;
                output.append(part);
                return true;
            };
            auto at = start;
            while (at < text.size()) {
                const auto found = text.find(query, at);
                if (found == std::string_view::npos) {
                    if (!append(text.substr(at)))
                        return Failure(SourceDocumentErrors::TooLarge);
                    break;
                }
                if (!Boundary(text, found) || !Boundary(text, found + query.size()))
                    return Failure(SourceDocumentErrors::Invalid);
                if (!append(text.substr(at, found - at)) || !append(replacement))
                    return Failure(SourceDocumentErrors::TooLarge);
                at = found + query.size();
            }
            return Result<SourceEditingPlan>::Success(
                {snapshot.Revision(), start, text.size() - start, std::move(output), view, {start, start, view.scrollLines, view.viewId}});
        } catch (const std::bad_alloc &) {
            return Failure(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc NavigateSourceLine */
    std::optional<SourceEditingView> NavigateSourceLine(const SourceDocumentSnapshot &snapshot, const std::size_t line,
                                                        const std::size_t column) {
        if (snapshot.Revision() == 0 || line == 0 || column == 0)
            return {};
        const auto text = snapshot.Text();
        auto at = TextStart(text);
        auto remainingLines = line - 1;
        while (remainingLines != 0 && at < text.size()) {
            at = NextLine(text, at);
            --remainingLines;
        }
        auto remainingColumns = column - 1;
        while (remainingColumns != 0 && at < text.size() && text[at] != '\r' && text[at] != '\n') {
            at = NextScalar(text, at);
            --remainingColumns;
        }
        return SourceEditingView{at, at, 0};
    }
}  // namespace Horo::Editor
