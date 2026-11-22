#include "SourceEditorSession.h"

#include "SourceReviewOverlay.h"
#include "editor/language/LspResultProjection.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        /** @brief Projects severity using the source widget's supported marker styles. */
        SourceWidgetDecorationKind DiagnosticStyle(const Language::LanguageSeverity severity) {
            using enum Language::LanguageSeverity;
            switch (severity) {
                case Error:
                    return SourceWidgetDecorationKind::Error;
                case Warning:
                    return SourceWidgetDecorationKind::Warning;
                default:
                    return SourceWidgetDecorationKind::Information;
            }
        }
    }  // namespace

    /** @copydoc SourceEditorView::Publish */
    void SourceEditorView::Publish(const SourceDocumentSnapshot &source) {
        widget.Publish(source);
        if (pendingReview) {
            if (!widget.PresentReview(*pendingReview))
                widget.ShowReviewConflict();
            pendingReview.reset();
        }
        if (syntax.Publish(source) || !widget.HasHighlight(source.Revision()))
            static_cast<void>(widget.Highlight(source.Revision(), syntax.Syntax()));
    }

    /** @copydoc SourceEditorView::PublishDiagnostics */
    void SourceEditorView::PublishDiagnostics(const SourceDocumentSnapshot &source,
                                              std::shared_ptr<const Language::LanguageResult> result) {
        if (result && (!result->context.source.IsCurrent() || !source.SharesStateWith(result->context.source)))
            result.reset();
        if (result == diagnostics)
            return;
        std::vector<SourceWidgetDecoration> markers;
        std::optional<SourceReviewDiagnosticCounts> counts;
        if (result) {
            if (const auto *published = std::get_if<Language::LanguageDiagnostics>(&result->payload)) {
                counts.emplace();
                markers.reserve(published->items.size());
                for (const auto &diagnostic : published->items) {
                    const auto style = DiagnosticStyle(diagnostic.severity);
                    counts->errors += std::size_t{style == SourceWidgetDecorationKind::Error};
                    counts->warnings += std::size_t{style == SourceWidgetDecorationKind::Warning};
                    markers.emplace_back(diagnostic.span.start, diagnostic.span.end, style, diagnostic.message);
                }
            }
        }
        if (widget.Decorate(source.Revision(), markers)) {
            diagnostics = std::move(result);
            diagnosticCounts_ = counts;
        }
    }

    /** @copydoc SourceEditorView::CurrentDiagnostics */
    std::optional<SourceReviewDiagnosticCounts> SourceEditorView::CurrentDiagnostics(const SourceDocumentSnapshot &source) const {
        if (!diagnostics || !source.IsCurrent() || !source.SharesStateWith(diagnostics->context.source))
            return std::nullopt;
        return diagnosticCounts_;
    }

    /** @copydoc SourceEditorView::ApplyLanguageFocus */
    bool SourceEditorView::ApplyLanguageFocus(const SourceDocumentSnapshot &source,
                                              const std::shared_ptr<const Language::LanguageSourceFocus> &focus) {
        if (!focus || !source.IsCurrent() || !source.SharesStateWith(focus->source) || lastLanguageFocus.lock() == focus)
            return false;
        if (!widget.Select({focus->selection.start, focus->selection.end}))
            return false;
        lastLanguageFocus = focus;
        return true;
    }

    /** @copydoc SourceEditorSession::Acquire */
    SourceEditorView *SourceEditorSession::Acquire(const DocumentInstanceId instance) {
        if (!instance.Value())
            return nullptr;
        if (const auto found = views_.find(instance.Value()); found != views_.end())
            return &found->second;
        if (!nextViewId_)
            return nullptr;
        return &views_.try_emplace(instance.Value(), SourceEditingViewId{nextViewId_++}).first->second;
    }

    /** @copydoc SourceEditorSession::StageReview */
    bool SourceEditorSession::StageReview(std::shared_ptr<const SourceReviewOverlay> overlay) {
        if (!overlay || !overlay->Source().IsCurrent())
            return false;
        auto *view = Acquire(overlay->Source().Identity().instance);
        if (!view)
            return false;
        view->pendingReview = std::move(overlay);
        return true;
    }

    /** @copydoc SourceEditorSession::ReadSelection */
    std::optional<SourceContextRange> SourceEditorSession::ReadSelection(const SourceDocumentSnapshot &snapshot) const {
        const auto found = views_.find(snapshot.Identity().instance.Value());
        return found == views_.end() ? std::nullopt : found->second.widget.ReadSelection(snapshot);
    }

    /** @copydoc SourceEditorSession::Retain */
    void SourceEditorSession::Retain(const std::span<const WorkspaceDocumentTab> tabs) {
        std::erase_if(views_, [tabs](const auto &entry) {
            return std::ranges::none_of(tabs, [&entry](const WorkspaceDocumentTab &candidate) {
                return candidate.identity.key.kind == DocumentKind::Source && candidate.identity.instance.Value() == entry.first;
            });
        });
        if (focused_ && !views_.contains(focused_->Value()))
            focused_.reset();
    }

    /** @copydoc SourceEditorSession::Reset */
    void SourceEditorSession::Reset() noexcept {
        views_.clear();
        focused_.reset();
    }

    /** @copydoc SourceEditorSession::NeedsFocus */
    bool SourceEditorSession::NeedsFocus(const DocumentInstanceId instance) const noexcept {
        return focused_ != instance;
    }

    /** @copydoc SourceEditorSession::Presented */
    void SourceEditorSession::Presented(const DocumentInstanceId instance) noexcept {
        focused_ = instance;
    }

    /** @copydoc SourceEditorSession::ClearFocus */
    void SourceEditorSession::ClearFocus() noexcept {
        focused_.reset();
    }
}  // namespace Horo::Editor
