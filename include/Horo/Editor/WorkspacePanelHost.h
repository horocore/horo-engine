#pragma once

#include "Horo/Editor/WorkspaceLayout.h"
#include "Horo/Editor/WorkspaceLayoutPersistence.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    namespace WorkspaceDocumentErrors {
        extern const ErrorCodeDescriptor DirtyDocument;   /**< The document has unsaved changes and close was not confirmed. */
        extern const ErrorCodeDescriptor DocumentUnknown; /**< The requested document instance is not open in the host. */
    }  // namespace WorkspaceDocumentErrors

    /** @brief Policy applied when a persistent workspace document is closed. */
    enum class WorkspaceDocumentClosePolicy : std::uint8_t {
        RequireClean,
        DiscardChanges,
    };

    /** @brief Workspace-owned projection of one persistent document tab. */
    struct WorkspaceDocumentTab final {
        DocumentIdentity identity;
        bool dirty{false}; /**< True when the owning document has unsaved authored state. */
    };

    /** @brief Owns the editor workspace layout and applies validated panel mutations. */
    class WorkspacePanelHost {
    public:
        enum class DropKind : std::uint8_t {
            TabCenter,
            SplitLeft,
            SplitRight,
            SplitTop,
            SplitBottom,
        };

        WorkspacePanelHost();

        /**
         * @brief Attaches the workspace to a shared document identity registry.
         * @param registry Workspace/session registry used by source and asset open routes.
         * @pre No document tab may already be open in this host.
         * @post The registry is borrowed and must outlive this host.
         */
        void AttachDocumentIdentityRegistry(DocumentIdentityRegistry &registry) noexcept;

        [[nodiscard]] WorkspaceLayout &Layout() noexcept {
            return m_layout;
        }

        [[nodiscard]] const WorkspaceLayout &Layout() const noexcept {
            return m_layout;
        }

        /** @brief Opens a panel in an existing tab stack and activates it. */
        [[nodiscard]] WorkspaceLayoutOperationResult OpenPanel(std::string_view panelId, std::string_view stackId);

        /** @brief Moves an existing panel tab to another stack and activates it. */
        [[nodiscard]] WorkspaceLayoutOperationResult MovePanel(std::string_view panelId, const TabPlacement &placement);

        /** @brief Closes a panel tab while preserving the stack active-tab invariant. */
        [[nodiscard]] WorkspaceLayoutOperationResult ClosePanel(std::string_view panelId);

        /** @brief Activates an existing tab in a stack. */
        [[nodiscard]] WorkspaceLayoutOperationResult SetActiveTab(std::string_view stackId, std::string_view panelId);

        /** @brief Docks a panel into a tab stack or creates a split around a target node. */
        [[nodiscard]] WorkspaceLayoutOperationResult DockPanel(std::string_view panelId, std::string_view targetNodeId, DropKind kind);

        /** @brief Opens or focuses one typed persistent document tab. */
        [[nodiscard]] Result<DocumentOpenResult> OpenDocument(const DocumentOpenKey &key);

        /**
         * @brief Closes one document tab after applying its dirty-state policy.
         * @param instance Session-local document instance to close.
         * @param policy Whether dirty content must be clean or may be discarded.
         * @return Success, or a typed dirty/unknown-instance error.
         */
        [[nodiscard]] Result<void> CloseDocument(DocumentInstanceId instance,
                                                 WorkspaceDocumentClosePolicy policy = WorkspaceDocumentClosePolicy::RequireClean);

        /** @brief Changes the dirty projection for one currently open document. */
        [[nodiscard]] Result<void> SetDocumentDirty(DocumentInstanceId instance, bool dirty);

        /** @brief Activates one currently open document without changing its source identity. */
        [[nodiscard]] Result<void> FocusDocument(DocumentInstanceId instance);

        /** @brief Returns the current document tabs in workspace order. */
        [[nodiscard]] std::span<const WorkspaceDocumentTab> DocumentTabs() const noexcept {
            return m_documentTabs;
        }

        /** @brief Returns the active document instance, if a document tab is open. */
        [[nodiscard]] std::optional<DocumentInstanceId> ActiveDocument() const noexcept {
            return m_activeDocument;
        }

        [[nodiscard]] bool SaveLayout(const std::filesystem::path &path, std::string *error = nullptr) const;
        [[nodiscard]] bool RestoreLayout(const std::filesystem::path &path, std::string *error = nullptr);

    private:
        [[nodiscard]] bool CloseDocumentTabs(std::span<const WorkspaceDocumentTab> tabs, std::string *error);
        [[nodiscard]] std::vector<WorkspaceDocumentTab> ReopenDocumentTabs(std::span<const WorkspaceDocumentTab> tabs);
        void RestoreDocumentSnapshot(const WorkspaceLayout &layout, std::span<const WorkspaceDocumentTab> tabs,
                                     const std::optional<DocumentOpenKey> &activeKey);
        [[nodiscard]] Result<std::vector<WorkspaceDocumentTab>> OpenRestoredDocumentTabs(
            std::span<const SerializedDocumentOpenKey> documents, std::string *error);

        WorkspaceLayout m_layout;
        DocumentIdentityRegistry m_ownedDocumentRegistry;
        DocumentIdentityRegistry *m_documentRegistry_{&m_ownedDocumentRegistry};
        std::vector<WorkspaceDocumentTab> m_documentTabs;
        std::optional<DocumentInstanceId> m_activeDocument;
    };
}  // namespace Horo::Editor
