#include "Horo/Editor/EditorSettingsEvents.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Foundation/Logging/LogContext.h"
#include "Horo/Foundation/Logging/Logger.h"

#include <memory>

namespace Horo::Editor {
    SettingsModal::SettingsModal(const EditorGuiContext &context, EditorSettingsService &settings, const std::uintptr_t logo,
                                 Extensions::ExtensionInventory *extensionInventory,
                                 Extensions::ExtensionMarketplaceService *extensionMarketplace,
                                 NavigationProjectSettingsSession *navigationSettings) noexcept
        : m_context(context), m_settings(settings), m_logo(logo), m_extensionInventory(extensionInventory),
          m_extensionMarketplace(extensionMarketplace),
          m_navigationSettings(navigationSettings ? navigationSettings->Borrow() : NavigationProjectSettingsSessionBorrow{}) {}

    ModalId SettingsModal::Id() const {
        return ModalId{kModalId};
    }

    ModalPresentation SettingsModal::Presentation() const {
        return {.size = ModalSizePolicy::Large, .dimWorkspace = true};
    }

    ModalClosePolicy SettingsModal::ClosePolicy() const {
        return {.allowCloseButton = true, .allowEscape = true, .allowOutsideClick = false, .allowApplicationShutdown = true};
    }

    Result<void> SettingsModal::OnOpen(EditorModalContext &context) {
        m_events = &context.events;
        m_revertedPublished = false;
        LoadSettingsForModal(m_draft, m_settings);
        m_draft.navigationSettings = m_navigationSettings.Resolve() ? m_navigationSettings : NavigationProjectSettingsSessionBorrow{};
        m_draft.extensionInventory = m_extensionInventory;
        m_draft.extensionMarketplace = m_extensionMarketplace;
        const auto rev = m_settings.Snapshot().revision;
        m_logCtx =
            std::make_unique<Log::LogContext>("modal", "settings", "modal_id", std::to_string(kModalId), "revision", std::to_string(rev));
        LOG_INFO("editor.settings", "SettingsModal opened (revision=%llu).", rev);
        return Result<void>::Success();
    }

    CloseDecision SettingsModal::CanClose(const ModalCloseReason reason) {
        m_draft.dirty = CollectDraftSettings(m_draft) != m_draft.committed;
        if (reason == ModalCloseReason::Cancelled || (!m_draft.dirty && !m_draft.navigationProjectDraftDirty))
            return CloseDecision::Allow;
        return CloseDecision::Deny;
    }

    void SettingsModal::OnClose(const ModalCloseReason reason) {
        m_draft.dirty = CollectDraftSettings(m_draft) != m_draft.committed;
        const char *reasonStr = "app_shutdown";
        if (reason == ModalCloseReason::Completed) {
            reasonStr = "completed";
        } else if (reason == ModalCloseReason::Cancelled) {
            reasonStr = "cancelled";
        }
        LOG_INFO("editor.settings", "SettingsModal closed (reason=%s, dirty=%s).", reasonStr, m_draft.dirty ? "yes" : "no");

        if (m_draft.dirty && !m_revertedPublished && m_events) {
            LOG_DEBUG("editor.settings", "Settings reverted — publishing Reverted event.");
            m_events->Publish(
                EditorSettingsChangedEvent{m_settings.Snapshot().revision, SettingsChangePhase::Reverted, SettingsDomain::All});
            m_revertedPublished = true;
        }
        m_draft.navigationSettings = {};
        m_draft.navigationDraft.reset();
        m_navigationSettings = {};
        m_logCtx.reset();  // pop MDC frame
    }

    SettingsState &SettingsModal::Draft() noexcept {
        return m_draft;
    }

    bool SettingsModal::ApplyDraft() {
        return ApplySettings(m_draft, m_settings);
    }
}  // namespace Horo::Editor
