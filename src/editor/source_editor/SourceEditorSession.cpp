#include "SourceEditorSession.h"

#include <algorithm>

namespace Horo::Editor {
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
