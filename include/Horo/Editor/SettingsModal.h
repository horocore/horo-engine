#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/SettingsModalDraft.h"
#include "Horo/Foundation/Logging/LogContext.h"

#include <cstdint>
#include <memory>

namespace Horo::Extensions {
    class ExtensionInventory;
    class ExtensionMarketplaceService;
}  // namespace Horo::Extensions

namespace Horo::Editor {
    /** @brief Host-owned editor settings workflow and its transient settings draft. */
    class SettingsModal final : public EditorModal {
    public:
        static constexpr std::uint64_t kModalId = 0x53455454494E4753ULL;

        /**
         * @brief Constructs the settings workflow with its settings and optional extension authorities.
         * @param context Borrowed editor presentation context.
         * @param settings Authoritative editor-settings service.
         * @param logo Presentation logo texture.
         * @param extensionInventory Optional installed-extension inventory.
         * @param extensionMarketplace Optional registry-backed extension marketplace.
         * @param navigationSettings Optional live session borrowed only during construction to capture its revocable opening.
         */
        SettingsModal(const EditorGuiContext &context, EditorSettingsService &settings, std::uintptr_t logo,
                      Extensions::ExtensionInventory *extensionInventory = nullptr,
                      Extensions::ExtensionMarketplaceService *extensionMarketplace = nullptr,
                      NavigationProjectSettingsSession *navigationSettings = nullptr) noexcept;

        [[nodiscard]] ModalId Id() const override;
        [[nodiscard]] ModalPresentation Presentation() const override;
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &context) override;
        [[nodiscard]] ModalFrameResult Draw() override;
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason reason) override;
        void OnClose(ModalCloseReason reason) override;

        /** @brief Returns the modal-owned draft used by the settings presentation. */
        [[nodiscard]] SettingsState &Draft() noexcept;
        /** @brief Applies the current draft through the authoritative settings service. */
        [[nodiscard]] bool ApplyDraft();

    private:
        const EditorGuiContext &m_context;
        EditorSettingsService &m_settings;
        std::uintptr_t m_logo = 0;
        Extensions::ExtensionInventory *m_extensionInventory = nullptr;
        Extensions::ExtensionMarketplaceService *m_extensionMarketplace = nullptr;
        NavigationProjectSettingsSessionBorrow m_navigationSettings;
        EditorDataBus *m_events = nullptr;
        SettingsState m_draft;
        bool m_revertedPublished = false;
        /// @brief RAII MDC frame active for the lifetime of this modal.
        std::unique_ptr<Log::LogContext> m_logCtx;
    };
}  // namespace Horo::Editor
