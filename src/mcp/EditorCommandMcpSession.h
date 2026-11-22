#pragma once

/** @file EditorCommandMcpSession.h @brief Host-owned MCP session for one editor workspace. */

#include "EditorWorkspaceCommandAccess.h"
#include "Horo/Editor/WorkspaceMcpControls.h"
#include "Horo/Mcp/McpSession.h"

#include <string_view>

namespace Horo::Mcp {
    class McpController;
    class McpInProcessAdapter;
    class McpToolSnapshot;
    enum class McpToolEffect : std::uint8_t;
}  // namespace Horo::Mcp

namespace Horo::Editor {
    /** @brief A local host's preapproved client and project binding. */
    struct EditorCommandMcpAdmission {
        std::string client;
        std::string project;
        std::uint64_t trustRevision{};
        std::vector<std::string> grants;
    };

    /** @brief Owns publication, the editor-thread pump and exact approvals for an embedded client.
     * @details Only the local host receives this object. The client receives Client() and observes the
     * same protocol as other MCP transports. The host closes the session before releasing workspace owners.
     * Host lifecycle and approval calls belong to the editor owner. The authenticated client channel can
     * submit requests from another lane without acquiring access to approval decisions.
     */
    class EditorCommandMcpSession final {
    public:
        /** @brief Publishes the available command packs and admits the explicitly authorized client.
         * @param endpoints Borrowed endpoints for the current owner thread, project and trust revision.
         * @param admission Host-issued selection of client, project and grants.
         * @param policy Host authorization with the exact project revision already trusted.
         * @return Owning session or a typed configuration or authorization failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<EditorCommandMcpSession>> Create(EditorWorkspaceCommandEndpoints endpoints,
                                                                                     EditorCommandMcpAdmission admission,
                                                                                     std::shared_ptr<Mcp::McpAuthorization> policy);

        ~EditorCommandMcpSession();
        EditorCommandMcpSession(const EditorCommandMcpSession &) = delete;
        EditorCommandMcpSession &operator=(const EditorCommandMcpSession &) = delete;

        /** @brief Returns the authenticated request channel with no host approval authority. */
        [[nodiscard]] std::shared_ptr<const Mcp::McpInProcessAdapter> Client() const noexcept;

        /** @brief Resolves the effect of a tool permitted for this client. @param tool Exact advertised tool ID. @return Effect or denial.
         */
        [[nodiscard]] Result<Mcp::McpToolEffect> ToolEffect(std::string_view tool) const;

        /** @brief Identifies this exact live session for local controls. @return Opaque scope, or empty when retired. */
        [[nodiscard]] WorkspaceMcpSessionKey SessionKey() const noexcept;
        /** @copydoc IWorkspaceMcpControls::ReadMcpStatus */
        [[nodiscard]] std::optional<WorkspaceMcpStatus> ReadMcpStatus(WorkspaceMcpRevision known) const;
        /** @brief Cancels one request through this session's authenticated controller. @param request Controller identity.
         * @return Result of the cancellation attempt.
         */
        [[nodiscard]] Result<void> CancelRequest(std::uint64_t request) const;

        /** @brief Creates a short-lived challenge for an exact visible presentation or mutation request.
         * @param request The original request to show to the local operator.
         * @return Challenge identity, or typed denial when authority or tool access is missing.
         */
        [[nodiscard]] Result<std::uint64_t> RequestApproval(const Mcp::McpRequest &request);

        /** @brief Applies only a local operator's decision to a challenge issued by this session.
         * @param challenge Exact challenge identity. @param approved Local operator decision. @return Typed outcome.
         */
        [[nodiscard]] Result<void> Decide(std::uint64_t challenge, bool approved);

        /** @brief Runs a bounded batch of accepted commands on the editor owner. @return Count or typed failure. */
        [[nodiscard]] Result<std::size_t> Pump() const;

        /** @brief Revokes admission and drains the session before workspace teardown. @return Typed drain result. */
        [[nodiscard]] Result<void> Close();

    private:
        EditorCommandMcpSession(std::shared_ptr<Mcp::McpAuthorization> policy, std::shared_ptr<Mcp::McpController> controller,
                                std::shared_ptr<Mcp::McpSessionManager> manager, std::shared_ptr<Mcp::McpInProcessAdapter> client,
                                std::shared_ptr<const Mcp::McpToolSnapshot> tools, Mcp::McpSessionAdmission admission);

        std::shared_ptr<Mcp::McpAuthorization> policy_;
        std::shared_ptr<Mcp::McpController> controller_;
        std::shared_ptr<Mcp::McpSessionManager> manager_;
        std::shared_ptr<Mcp::McpInProcessAdapter> client_;
        std::shared_ptr<const Mcp::McpToolSnapshot> tools_;
        Mcp::McpRequestContext sessionContext_;
        std::optional<std::uint64_t> pendingApproval_;
    };
}  // namespace Horo::Editor
