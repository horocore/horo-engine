#include "EditorMcpWorkspaceHost.h"

#include "Horo/Mcp/McpAuthorization.h"
#include "Horo/Mcp/McpErrors.h"

#include <limits>

namespace Horo::Editor {
    /** @copydoc EditorMcpWorkspaceHost::EditorMcpWorkspaceHost */
    EditorMcpWorkspaceHost::EditorMcpWorkspaceHost(std::shared_ptr<Security::SecureRandomSource> random) : random_(std::move(random)) {}

    EditorMcpWorkspaceHost::~EditorMcpWorkspaceHost() {
        static_cast<void>(Detach());
    }

    /** @copydoc EditorMcpWorkspaceHost::Attach */
    Result<void> EditorMcpWorkspaceHost::Attach(EditorWorkspaceCommandAccess access) {
        if (!workspace_.projectIdentity.empty() || access.projectIdentity.empty() || !access.bind || !access.revoke)
            return Result<void>::Failure(MakeError(Mcp::McpErrors::ConfigurationInvalid));
        workspace_ = std::move(access);
        return Result<void>::Success();
    }

    /** @copydoc EditorMcpWorkspaceHost::PumpWorkspaceCommands */
    Result<void> EditorMcpWorkspaceHost::PumpWorkspaceCommands() {
        if (!session_)
            return Result<void>::Success();
        if (!session_->SessionKey().principal)
            return CloseSession();
        const auto pumped = session_->Pump();
        return pumped.HasValue() ? Result<void>::Success() : Result<void>::Failure(pumped.ErrorValue());
    }

    /** @copydoc EditorMcpWorkspaceHost::Detach */
    Result<void> EditorMcpWorkspaceHost::Detach() {
        auto closed = CloseSession();
        workspace_ = {};
        return closed;
    }

    /** @copydoc EditorMcpWorkspaceHost::Open */
    Result<std::shared_ptr<const Mcp::McpInProcessAdapter>> EditorMcpWorkspaceHost::Open(std::string client,
                                                                                         std::vector<std::string> grants) {
        using Outcome = Result<std::shared_ptr<const Mcp::McpInProcessAdapter>>;
        if (!random_ || workspace_.projectIdentity.empty())
            return Outcome::Failure(MakeError(Mcp::McpErrors::OwnerUnavailable));
        if (session_ || trustRevision_ == std::numeric_limits<std::uint64_t>::max())
            return Outcome::Failure(MakeError(Mcp::McpErrors::SessionCapacityExceeded));
        const auto revision = ++trustRevision_;
        auto policy = std::make_shared<Mcp::McpAuthorization>(random_);
        if (const auto trusted = policy->SetTrust(workspace_.projectIdentity, revision, true); trusted.HasError())
            return Outcome::Failure(trusted.ErrorValue());
        auto opened = EditorCommandMcpSession::Create(workspace_.bind(revision),
                                                      {std::move(client), workspace_.projectIdentity, revision, std::move(grants)},
                                                      std::move(policy));
        if (opened.HasError()) {
            workspace_.revoke();
            return Outcome::Failure(opened.ErrorValue());
        }
        session_ = std::move(opened).Value();
        return Outcome::Success(session_->Client());
    }

    /** @copydoc EditorMcpWorkspaceHost::ActiveSession */
    EditorCommandMcpSession *EditorMcpWorkspaceHost::ActiveSession() const noexcept {
        return session_.get();
    }

    /** @copydoc EditorMcpWorkspaceHost::ReadMcpStatus */
    std::optional<WorkspaceMcpStatus> EditorMcpWorkspaceHost::ReadMcpStatus(const WorkspaceMcpRevision known) const {
        if (session_)
            return session_->ReadMcpStatus(known);
        return known.session.principal ? std::optional{WorkspaceMcpStatus{}} : std::nullopt;
    }

    /** @copydoc EditorMcpWorkspaceHost::OwnsSession */
    bool EditorMcpWorkspaceHost::OwnsSession(const WorkspaceMcpSessionKey session) const noexcept {
        return session.principal && session_ && session_->SessionKey() == session;
    }

    /** @copydoc EditorMcpWorkspaceHost::CancelMcpRequest */
    Result<void> EditorMcpWorkspaceHost::CancelMcpRequest(const WorkspaceMcpSessionKey session, const std::uint64_t request) {
        return OwnsSession(session) ? session_->CancelRequest(request)
                                    : Result<void>::Failure(MakeError(Mcp::McpErrors::SessionUnavailable));
    }

    /** @copydoc EditorMcpWorkspaceHost::RevokeMcpSession */
    Result<void> EditorMcpWorkspaceHost::RevokeMcpSession(const WorkspaceMcpSessionKey session) {
        return OwnsSession(session) ? CloseSession() : Result<void>::Failure(MakeError(Mcp::McpErrors::SessionUnavailable));
    }

    /** @copydoc EditorMcpWorkspaceHost::CloseSession */
    Result<void> EditorMcpWorkspaceHost::CloseSession() {
        if (!session_)
            return Result<void>::Success();
        auto closed = session_->Close();
        workspace_.revoke();
        session_.reset();
        return closed;
    }
}  // namespace Horo::Editor
