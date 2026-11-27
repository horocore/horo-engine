#include "EditorCommandMcpSession.h"

#include "EditorBuildMcpTools.h"
#include "EditorDebugCaptureMcpTools.h"
#include "EditorInspectionMcpTools.h"
#include "EditorSourceMcpTools.h"
#include "EditorTestMcpTools.h"
#include "Horo/Mcp/McpAuthorization.h"
#include "Horo/Mcp/McpController.h"
#include "Horo/Mcp/McpErrors.h"
#include "Horo/Mcp/McpInProcessAdapter.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <thread>

namespace Horo::Editor {
    namespace {
        using namespace std::chrono_literals;
        using SessionResult = Result<std::unique_ptr<EditorCommandMcpSession>>;

        /** @brief Builds immutable registrations from one existing workspace owner. */
        template <typename Endpoint>
        Result<void> AppendCommands(const std::weak_ptr<const Endpoint> &borrowed, const EditorCommandMcpAdmission &admission,
                                    Result<std::vector<Mcp::McpToolRegistration>> (*make)(std::weak_ptr<const Endpoint>),
                                    std::vector<Mcp::McpToolRegistration> &tools) {
            const auto endpoint = borrowed.lock();
            if (!endpoint)
                return Result<void>::Success();
            if (endpoint->project != admission.project || endpoint->authorizationRevision != admission.trustRevision ||
                endpoint->owner != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(Mcp::McpErrors::OwnerUnavailable));
            auto registered = make(borrowed);
            if (registered.HasError())
                return Result<void>::Failure(registered.ErrorValue());
            auto entries = std::move(registered).Value();
            tools.insert(tools.end(), std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
            return Result<void>::Success();
        }

        /** @brief Collects the grants described by the available packs so missing endpoints cannot be admitted. */
        std::vector<std::string> DeclaredGrants(const std::vector<Mcp::McpToolRegistration> &tools) {
            std::vector<std::string> grants;
            for (const auto &tool : tools)
                for (const auto &grant : tool.descriptor.requiredCapabilities)
                    if (std::ranges::find(grants, grant) == grants.end())
                        grants.push_back(grant);
            return grants;
        }

        /** @brief Validates the project-bound command packs and publishes their single immutable registry. */
        Result<std::uint64_t> PublishCommands(Mcp::McpToolRegistry &registry, const EditorWorkspaceCommandEndpoints &endpoints,
                                              const EditorCommandMcpAdmission &admission) {
            using Outcome = Result<std::uint64_t>;
            if (admission.project.empty() || admission.grants.empty())
                return Outcome::Failure(MakeError(Mcp::McpErrors::ConfigurationInvalid));
            std::vector<Mcp::McpToolRegistration> tools;
            if (const auto source = AppendCommands(endpoints.source, admission, MakeEditorSourceMcpTools, tools); source.HasError())
                return Outcome::Failure(source.ErrorValue());
            if (const auto build = AppendCommands(endpoints.build, admission, MakeEditorBuildMcpTools, tools); build.HasError())
                return Outcome::Failure(build.ErrorValue());
            if (const auto test = AppendCommands(endpoints.test, admission, MakeEditorTestMcpTools, tools); test.HasError())
                return Outcome::Failure(test.ErrorValue());
            if (const auto read = AppendCommands(endpoints.inspection, admission, MakeEditorInspectionMcpTools, tools); read.HasError())
                return Outcome::Failure(read.ErrorValue());
            if (const auto debug = AppendCommands(endpoints.debugCapture, admission, MakeEditorDebugCaptureMcpTools, tools);
                debug.HasError())
                return Outcome::Failure(debug.ErrorValue());
            const auto available = DeclaredGrants(tools);
            if (!std::ranges::all_of(admission.grants, [&](const auto &grant) {
                return std::ranges::find(available, grant) != available.end();
            }))
                return Outcome::Failure(MakeError(Mcp::McpErrors::AuthorizationDenied));
            return registry.Publish(std::move(tools), available);
        }

        /** @brief Authenticates the exact host-supplied client admission using a private one-use credential. */
        Result<Mcp::McpSessionAdmission> Authenticate(const Mcp::McpAuthorization &policy, EditorCommandMcpAdmission approved,
                                                      const std::uint64_t generation) {
            Mcp::McpSessionAdmission admission{.clientIdentity = std::move(approved.client),
                                               .capabilities = std::move(approved.grants),
                                               .projectIdentity = std::move(approved.project),
                                               .authorizationRevision = approved.trustRevision,
                                               .registryRevision = generation};
            auto credential = policy.IssueCredential(admission, 1h);
            if (credential.HasError())
                return Result<Mcp::McpSessionAdmission>::Failure(credential.ErrorValue());
            auto principal = policy.Authenticate(admission, std::move(credential).Value());
            if (principal.HasError())
                return Result<Mcp::McpSessionAdmission>::Failure(principal.ErrorValue());
            admission.authority = std::move(principal).Value();
            return Result<Mcp::McpSessionAdmission>::Success(std::move(admission));
        }
    }  // namespace

    /** @copydoc EditorCommandMcpSession::Create */
    SessionResult EditorCommandMcpSession::Create(EditorWorkspaceCommandEndpoints endpoints, EditorCommandMcpAdmission admission,
                                                  std::shared_ptr<Mcp::McpAuthorization> policy) {
        if (!policy)
            return SessionResult::Failure(MakeError(Mcp::McpErrors::ConfigurationInvalid));
        auto registry = std::make_shared<Mcp::McpToolRegistry>(policy);
        const auto published = PublishCommands(*registry, endpoints, admission);
        if (published.HasError())
            return SessionResult::Failure(published.ErrorValue());
        auto controller = Mcp::McpController::Create(registry, {}, policy);
        if (controller.HasError())
            return SessionResult::Failure(controller.ErrorValue());
        if (const auto bound = controller.Value()->BindOwner(Mcp::McpOwnerContext::Editor); bound.HasError())
            return SessionResult::Failure(bound.ErrorValue());
        auto manager = Mcp::McpSessionManager::Create(controller.Value(), {}, policy);
        if (manager.HasError())
            return SessionResult::Failure(manager.ErrorValue());
        auto authenticated = Authenticate(*policy, std::move(admission), published.Value());
        if (authenticated.HasError())
            return SessionResult::Failure(authenticated.ErrorValue());
        auto client = Mcp::McpInProcessAdapter::Start(manager.Value(), authenticated.Value());
        if (client.HasError()) {
            policy->Revoke(authenticated.Value().authority);
            return SessionResult::Failure(client.ErrorValue());
        }
        return SessionResult::Success(std::unique_ptr<EditorCommandMcpSession>(
            new EditorCommandMcpSession(std::move(policy), std::move(controller).Value(), std::move(manager).Value(),
                                        std::move(client).Value(), registry->Read(), std::move(authenticated).Value())));
    }

    EditorCommandMcpSession::EditorCommandMcpSession(std::shared_ptr<Mcp::McpAuthorization> policy,
                                                     std::shared_ptr<Mcp::McpController> controller,
                                                     std::shared_ptr<Mcp::McpSessionManager> manager,
                                                     std::shared_ptr<Mcp::McpInProcessAdapter> client,
                                                     std::shared_ptr<const Mcp::McpToolSnapshot> tools, Mcp::McpSessionAdmission admission)
        : policy_(std::move(policy)), controller_(std::move(controller)), manager_(std::move(manager)), client_(std::move(client)),
          tools_(std::move(tools)), sessionContext_{.session = client_->Session(),
                                                    .clientIdentity = std::move(admission.clientIdentity),
                                                    .capabilities = std::move(admission.capabilities),
                                                    .projectIdentity = std::move(admission.projectIdentity),
                                                    .authorizationRevision = admission.authorizationRevision,
                                                    .registryRevision = admission.registryRevision,
                                                    .deadline = admission.authority->ExpiresAt(),
                                                    .authority = std::move(admission.authority),
                                                    .authorization = policy_} {}

    EditorCommandMcpSession::~EditorCommandMcpSession() {
        static_cast<void>(Close());
    }

    /** @copydoc EditorCommandMcpSession::Client */
    std::shared_ptr<const Mcp::McpInProcessAdapter> EditorCommandMcpSession::Client() const noexcept {
        return client_;
    }

    /** @copydoc EditorCommandMcpSession::CapturePlanningSource */
    std::optional<EditorMcpPlanningSource> EditorCommandMcpSession::CapturePlanningSource() const {
        const auto session = sessionContext_.session;
        auto isCurrent = [client = std::weak_ptr<const Mcp::McpInProcessAdapter>{client_},
                          principal = std::weak_ptr<const Mcp::McpAuthority>{sessionContext_.authority}, session] {
            const auto channel = client.lock();
            const auto authority = principal.lock();
            return channel && authority && channel->Session() == session && !authority->IsStopRequested();
        };
        if (!sessionContext_.projectIdentity || !isCurrent())
            return std::nullopt;
        return EditorMcpPlanningSource{tools_, sessionContext_.capabilities, *sessionContext_.projectIdentity, std::move(isCurrent)};
    }

    /** @copydoc EditorCommandMcpSession::ToolEffect */
    Result<Mcp::McpToolEffect> EditorCommandMcpSession::ToolEffect(const std::string_view tool) const {
        return tools_->Effect({std::string{tool}}, sessionContext_.capabilities);
    }

    /** @copydoc EditorCommandMcpSession::SessionKey */
    WorkspaceMcpSessionKey EditorCommandMcpSession::SessionKey() const noexcept {
        const auto handle = client_->Session();
        return handle.IsValid() && !sessionContext_.authority->IsStopRequested()
                   ? WorkspaceMcpSessionKey{sessionContext_.authority->AuditIdentity(), handle.id, handle.generation}
                   : WorkspaceMcpSessionKey{};
    }

    /** @copydoc EditorCommandMcpSession::ReadMcpStatus */
    std::optional<WorkspaceMcpStatus> EditorCommandMcpSession::ReadMcpStatus(const WorkspaceMcpRevision known) const {
        const auto key = SessionKey();
        if (!key.principal)
            return known.session.principal ? std::optional{WorkspaceMcpStatus{}} : std::nullopt;
        auto active = controller_->ReadActiveRequests(sessionContext_, known.session == key ? known.requests : 0);
        if (active.HasError())
            return known.session.principal ? std::optional{WorkspaceMcpStatus{}} : std::nullopt;
        if (!active.Value())
            return std::nullopt;
        auto snapshot = std::move(active).Value().value();
        WorkspaceMcpStatus status{{key, snapshot.revision}, sessionContext_.capabilities};
        status.requests.reserve(snapshot.requests.size());
        for (auto &request : snapshot.requests)
            status.requests.push_back({request.id, std::move(request.tool), request.running});
        return status;
    }

    /** @copydoc EditorCommandMcpSession::CancelRequest */
    Result<void> EditorCommandMcpSession::CancelRequest(const std::uint64_t request) const {
        const auto cancelled =
            client_->Call({.id = "local-mcp-cancel", .method = "operations/cancel", .params = {{"operationId", request}}});
        if (cancelled.HasError())
            return Result<void>::Failure(cancelled.ErrorValue());
        if (cancelled.Value().at("status") != "cancelled")
            return Result<void>::Failure(MakeError(Mcp::McpErrors::OperationUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc EditorCommandMcpSession::RequestApproval */
    Result<std::uint64_t> EditorCommandMcpSession::RequestApproval(const Mcp::McpRequest &request) {
        using Outcome = Result<std::uint64_t>;
        if (const auto valid = Mcp::McpAuthorization::ValidateCallRequest(request); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        const auto effect = ToolEffect(request.params.at("name").get_ref<const std::string &>());
        if (effect.HasError())
            return Outcome::Failure(effect.ErrorValue());
        if (effect.Value() == Mcp::McpToolEffect::Query)
            return Outcome::Failure(MakeError(Mcp::McpErrors::RequestInvalid));
        auto context = sessionContext_;
        context.session = client_->Session();
        context.requestIdentity = request.id;
        auto challenge = policy_->Challenge(context, request, 1min);
        if (challenge.HasValue()) {
            if (pendingApproval_)
                static_cast<void>(policy_->Decide(*pendingApproval_, false));
            pendingApproval_ = challenge.Value();
        }
        return challenge;
    }

    /** @copydoc EditorCommandMcpSession::Decide */
    Result<void> EditorCommandMcpSession::Decide(const std::uint64_t challenge, const bool approved) {
        if (pendingApproval_ != challenge)
            return Result<void>::Failure(MakeError(Mcp::McpErrors::ApprovalRequired));
        pendingApproval_.reset();
        return policy_->Decide(challenge, approved);
    }

    /** @copydoc EditorCommandMcpSession::Pump */
    Result<std::size_t> EditorCommandMcpSession::Pump() const {
        return controller_->Pump(Mcp::McpOwnerContext::Editor);
    }

    /** @copydoc EditorCommandMcpSession::Close */
    Result<void> EditorCommandMcpSession::Close() {
        pendingApproval_.reset();
        policy_->Revoke(sessionContext_.authority);
        client_->Disconnect();
        const auto sessions = manager_->Shutdown();
        auto drained = controller_->Shutdown();
        return sessions.HasError() ? sessions : drained;
    }
}  // namespace Horo::Editor
