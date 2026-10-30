#include "../extension_pages/EditorPageView.h"
#include "Horo/Editor/EditorSettingsEvents.h"
#include "Horo/Editor/GuiLiveReload.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Logging/LogContext.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "SettingsModalInternal.h"

#include <memory>

namespace Horo::Editor {
    /** @copydoc SettingsModal::SettingsModal */
    SettingsModal::SettingsModal(const EditorGuiContext &context, EditorSettingsService &settings, const std::uintptr_t logo,
                                 Extensions::ExtensionInventory *extensionInventory,
                                 Extensions::ExtensionMarketplaceService *extensionMarketplace, SettingsModalProjectOpening projects,
                                 SettingsModalPagePresentation pages)
        : EditorModal(kTextScope), m_context(context), m_settings(settings), m_logo(logo), m_extensionInventory(extensionInventory),
          m_extensionMarketplace(extensionMarketplace),
          m_navigationSettings(projects.navigation ? projects.navigation->Borrow() : NavigationProjectSettingsSessionBorrow{}) {
        m_draft.extensionCommands = std::move(pages.commands);
        m_draft.collision.owner = std::move(projects.collision);
        m_draft.collision.error = std::move(projects.collisionError);
        m_pagesUnsupported = pages.host && !pages.theme;
        if (pages.host && pages.theme)
            m_extensionPages = std::make_shared<EditorPageView>(context, std::move(pages.host), std::move(pages.theme));
    }

    /** @copydoc SettingsModal::SelectExtensionPage */
    void SettingsModal::SelectExtensionPage(const Extensions::EditorSurfaceProviderIdentity &provider, std::string id) {
        if (m_extensionPages) {
            m_extensionPages->Select(provider, std::move(id));
            m_openExtensionPage = true;
        }
    }

    ModalId SettingsModal::Id() const {
        return ModalId{kModalId};
    }

    ModalPresentation SettingsModal::Presentation() const {
        return {.size = ModalSizePolicy::Large, .dimWorkspace = true};
    }

    ModalClosePolicy SettingsModal::ClosePolicy() const {
        return {.allowCloseButton = true,
                .allowEscape = m_draft.input.recordingAction.empty(),
                .allowOutsideClick = false,
                .allowApplicationShutdown = true};
    }

    Result<void> SettingsModal::OnOpen(EditorModalContext &context) {
        if (m_pagesUnsupported)
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&Extensions::ExtensionErrors::EditorThemeTokenInvalid>()));
        m_events = &context.events;
        m_revertedPublished = false;
        LoadSettingsForModal(m_draft, m_settings);
        m_draft.extensionPages = m_extensionPages;
        if (m_openExtensionPage)
            m_draft.activeTab = static_cast<int>(SettingsModalInternal::SettingsTab::ExtensionPages);
        m_draft.navigationSettings = m_navigationSettings.Resolve() ? m_navigationSettings : NavigationProjectSettingsSessionBorrow{};
        m_draft.extensionInventory = m_extensionInventory;
        m_draft.extensionMarketplace = m_extensionMarketplace;
        if (const auto owner = m_draft.collision.owner.lock()) {
            const auto snapshot = owner->Snapshot();
            if (snapshot.HasError())
                m_draft.collision.error = snapshot.ErrorValue();
            else
                m_draft.collision.input = snapshot.Value()->Input();
        }
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
        if (m_extensionPages)
            m_extensionPages->Cancel();
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
        m_draft.collision = {};
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
