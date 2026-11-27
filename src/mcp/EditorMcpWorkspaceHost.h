#pragma once

/** @file EditorMcpWorkspaceHost.h @brief Product ownership of editor MCP sessions across workspace transitions. */

#include "EditorCommandMcpSession.h"

namespace Horo::Security {
    class SecureRandomSource;
}

namespace Horo::Editor {
    /** @brief Admits at most one explicitly approved embedded client for the current workspace. */
    class EditorMcpWorkspaceHost final : public IEditorWorkspaceCommandHost, public IWorkspaceMcpControls {
    public:
        /** @brief Supplies platform entropy without opening a session. @param random Native entropy provider. */
        explicit EditorMcpWorkspaceHost(std::shared_ptr<Security::SecureRandomSource> random);
        ~EditorMcpWorkspaceHost() override;

        /** @copydoc IEditorWorkspaceCommandHost::Attach */
        [[nodiscard]] Result<void> Attach(EditorWorkspaceCommandAccess access) override;
        /** @copydoc IEditorWorkspaceCommandHost::PumpWorkspaceCommands */
        [[nodiscard]] Result<void> PumpWorkspaceCommands() override;
        /** @copydoc IEditorWorkspaceCommandHost::Detach */
        [[nodiscard]] Result<void> Detach() override;

        /** @copydoc IWorkspaceMcpControls::ReadMcpStatus */
        [[nodiscard]] std::optional<WorkspaceMcpStatus> ReadMcpStatus(WorkspaceMcpRevision known) const override;
        /** @copydoc IWorkspaceMcpControls::CancelMcpRequest */
        [[nodiscard]] Result<void> CancelMcpRequest(WorkspaceMcpSessionKey session, std::uint64_t request) override;
        /** @copydoc IWorkspaceMcpControls::RevokeMcpSession */
        [[nodiscard]] Result<void> RevokeMcpSession(WorkspaceMcpSessionKey session) override;

        /** @brief Opens a session after the local operator authorizes the exact client and grants.
         * @param client Host-issued client identity. @param grants Exact approved command capabilities.
         * @return Embedded protocol channel or a typed admission error.
         */
        [[nodiscard]] Result<std::shared_ptr<const Mcp::McpInProcessAdapter>> Open(std::string client, std::vector<std::string> grants);

        /** @brief Borrows the approval owner until session closure or workspace detach. */
        [[nodiscard]] EditorCommandMcpSession *ActiveSession() const noexcept;

        /** @brief Retires the client and its native operations while keeping the committed workspace available. @return Drain outcome. */
        [[nodiscard]] Result<void> CloseSession();

    private:
        /** @brief Checks that the displayed authority still owns the current session. */
        [[nodiscard]] bool OwnsSession(WorkspaceMcpSessionKey session) const noexcept;

        std::shared_ptr<Security::SecureRandomSource> random_;
        EditorWorkspaceCommandAccess workspace_;
        std::unique_ptr<EditorCommandMcpSession> session_;
        std::uint64_t trustRevision_{};
    };
}  // namespace Horo::Editor
