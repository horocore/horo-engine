#pragma once
/** @file SettingsModal.h @brief Host-owned settings workflow with explicitly composed page presentation. */

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/SettingsModalDraft.h"
#include "Horo/Foundation/Logging/LogContext.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace Horo::Extensions {
    class ExtensionInventory;
    class EditorPageHost;
    class EditorActivityHost;
    struct EditorThemeFrame;
    struct EditorSurfaceProviderIdentity;
    class ExtensionMarketplaceService;
}  // namespace Horo::Extensions

namespace Horo::Editor {
    class EditorPageView;

    /** @brief Paired page authority and theme snapshot supplied by the GUI composition owner. */
    struct SettingsModalPagePresentation final {
        std::shared_ptr<Extensions::EditorPageHost> host;          /**< Shared page authority; empty disables extension pages. */
        std::shared_ptr<const Extensions::EditorThemeFrame> theme; /**< Owner theme capture; required when host is supplied. */
        std::shared_ptr<Extensions::EditorActivityHost> commands;  /**< Optional live command metadata/localization authority. */
    };

    /** @brief Explicit project openings captured as revocable borrows during modal construction. */
    struct SettingsModalProjectOpening final {
        const NavigationProjectSettingsSession *navigation{};     /**< Borrowed only until its revocable opening is captured. */
        std::weak_ptr<CollisionProjectSettingsSession> collision; /**< Never extends the project or Scene owner lifetime. */
        std::optional<Error> collisionError;                      /**< Preserves the original typed opening failure for presentation. */
        std::weak_ptr<PrefabProjectSettingsSession> prefab;       /**< Exact project opening, revoked by owner shutdown. */
    };

    /** @brief Host-owned editor settings workflow and its transient settings draft. */
    class SettingsModal final : public EditorModal {
    public:
        /** @brief Catalog scope shared by this surface and its view helpers. */
        static constexpr GuiLocalizationScope kTextScope{"settings"};

        static constexpr std::uint64_t kModalId = 0x53455454494E4753ULL;

        /**
         * @brief Constructs the settings workflow with its settings and optional extension authorities.
         * @param context Borrowed editor presentation context.
         * @param settings Authoritative editor-settings service.
         * @param logo Presentation logo texture.
         * @param extensionInventory Optional installed-extension inventory.
         * @param extensionMarketplace Optional registry-backed extension marketplace.
         * @param projects Explicit project openings and original admission diagnostics; no owner lifetime is extended.
         * @param pages Optional explicitly composed restricted page authority and its GUI owner theme capture.
         * @throws std::bad_alloc When the supplied page authority cannot allocate its owned presentation.
         */
        SettingsModal(const EditorGuiContext &context, EditorSettingsService &settings, std::uintptr_t logo,
                      Extensions::ExtensionInventory *extensionInventory = nullptr,
                      Extensions::ExtensionMarketplaceService *extensionMarketplace = nullptr, SettingsModalProjectOpening projects = {},
                      SettingsModalPagePresentation pages = {});

        /** @brief Selects an already-open exact extension settings page for actual modal realization. */
        void SelectExtensionPage(const Extensions::EditorSurfaceProviderIdentity &provider, std::string id);
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
        std::shared_ptr<EditorPageView> m_extensionPages;
        bool m_openExtensionPage{};
        bool m_pagesUnsupported{};
        NavigationProjectSettingsSessionBorrow m_navigationSettings;
        EditorDataBus *m_events = nullptr;
        SettingsState m_draft;
        bool m_revertedPublished = false;
        /// @brief RAII MDC frame active for the lifetime of this modal.
        std::unique_ptr<Log::LogContext> m_logCtx;
    };
}  // namespace Horo::Editor
