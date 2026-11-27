#include "AgentLocalSceneTurn.h"

#include "AgentTurnMcp.h"
#include "Horo/Agent/ReferenceModelProviders.h"
#include "Horo/Mcp/McpAuthorization.h"
#include "Horo/Mcp/McpController.h"

#include <algorithm>
#include <limits>

namespace Horo::Agent {
    namespace {
        constexpr std::string_view SceneToolDescription = "Read the object count approved by the editor user for this turn.";
        constexpr auto Owner = Mcp::McpOwnerContext::Background;
        using Json = nlohmann::json;

        /** @brief Captured approval data has no path, source, or mutable editor ownership. */
        class ApprovedSceneTool final : public Mcp::IMcpToolAdapter {
        public:
            explicit ApprovedSceneTool(std::size_t count) : count_(count) {}

            Result<Json> Invoke(const Json &, const Mcp::McpRequestContext &) override {
                return Result<Json>::Success({{"count", count_}});
            }

        private:
            const std::size_t count_;
        };

        /** @brief Owns the exact principal for one turn, including out-of-band revocation. */
        struct ApprovedSceneSession {
            std::shared_ptr<Mcp::McpAuthorization> authorization;
            std::shared_ptr<const Mcp::McpAuthority> principal;
            AgentTurnMcp bridge;
        };

        /** @brief Authenticates an embedded session scoped to one immutable result and this worker. */
        Result<ApprovedSceneSession> OpenSceneTool(std::size_t count, const Json &input,
                                                   std::shared_ptr<Security::SecureRandomSource> entropy,
                                                   const LocalSceneAuditObserver &observeAudit) {
            using Outcome = Result<ApprovedSceneSession>;
            using namespace std::chrono_literals;
            auto authorization = std::make_shared<Mcp::McpAuthorization>(std::move(entropy));
            if (const auto trusted = authorization->SetTrust({}, 1, true); trusted.HasError())
                return Outcome::Failure(trusted.ErrorValue());
            const auto registry = std::make_shared<Mcp::McpToolRegistry>(authorization);
            const auto revision = registry->Publish({{.descriptor = {.id = {std::string{LocalSceneCountTool}},
                                                                     .description = std::string{SceneToolDescription},
                                                                     .inputSchema = input,
                                                                     .outputSchema = {{"type", "object"},
                                                                                      {"properties", {{"count", {{"type", "integer"}}}}},
                                                                                      {"required", Json::array({"count"})},
                                                                                      {"additionalProperties", false}},
                                                                     .effect = Mcp::McpToolEffect::Query},
                                                      .adapter = std::make_shared<ApprovedSceneTool>(count),
                                                      .owner = Owner}});
            if (revision.HasError())
                return Outcome::Failure(revision.ErrorValue());
            auto controller = Mcp::McpController::Create(registry, {}, authorization);
            if (controller.HasError())
                return Outcome::Failure(controller.ErrorValue());
            auto configured = std::move(controller).Value();
            if (const auto bound = configured->BindOwner(Owner); bound.HasError())
                return Outcome::Failure(bound.ErrorValue());
            auto sessions = Mcp::McpSessionManager::Create(configured, {}, authorization);
            if (sessions.HasError())
                return Outcome::Failure(sessions.ErrorValue());
            Mcp::McpSessionAdmission admission{.clientIdentity = "editor-local-scene", .registryRevision = revision.Value()};
            auto credential = authorization->IssueCredential(admission, 1min);
            if (credential.HasError())
                return Outcome::Failure(credential.ErrorValue());
            auto principal = authorization->Authenticate(admission, std::move(credential).Value());
            if (principal.HasError())
                return Outcome::Failure(principal.ErrorValue());
            admission.authority = std::move(principal).Value();
            const auto authority = admission.authority;
            const auto auditPrincipal = authority->AuditIdentity();
            auto embedded = Mcp::McpInProcessAdapter::Start(std::move(sessions).Value(), std::move(admission));
            if (embedded.HasError())
                return Outcome::Failure(embedded.ErrorValue());
            const auto session = embedded.Value()->Session();
            std::function<void(std::uint64_t)> observeOperation;
            if (observeAudit)
                observeOperation = [observeAudit, auditPrincipal, session](const std::uint64_t operation) {
                    observeAudit({auditPrincipal, session.id, session.generation, operation});
                };
            AgentTurnMcp bridge{std::move(embedded).Value(), {}, [controller = std::move(configured)] {
                const auto result = controller->Pump(Owner);
                return result.HasValue() ? Result<void>::Success() : Result<void>::Failure(result.ErrorValue());
            }, std::move(observeOperation)};
            return Outcome::Success({std::move(authorization), authority, std::move(bridge)});
        }
    }  // namespace

    /** @copydoc RunApprovedLocalSceneTurn */
    AgentTurnResult RunApprovedLocalSceneTurn(ModelProviderConfig config, ModelRequest request, std::uint64_t turnId,
                                              std::size_t objectCount, std::shared_ptr<Security::SecureRandomSource> entropy,
                                              std::stop_token stop, const AgentToolActivityObserver &observeTool,
                                              const LocalSceneAuditObserver &observeAudit) {
        if (!request.tools.empty() || turnId == 0)
            return {AgentTurnStatus::InvalidRequest};
        if (stop.stop_requested())
            return {AgentTurnStatus::Cancelled};
        if (config.timeout.count() <= 0 || config.timeout.count() > (std::numeric_limits<long>::max)())
            return {.status = AgentTurnStatus::ProviderFailed, .providerError = ModelErrorCode::InvalidRequest};

        using Clock = std::chrono::steady_clock;
        constexpr AgentTurnLimits limits{.maximumToolCalls = 1};
        config.timeout = std::min(config.timeout, limits.timeout);
        const auto deadline = Clock::now() + config.timeout;
        const auto stopped = [&]() -> std::optional<AgentTurnStatus> {
            if (stop.stop_requested())
                return AgentTurnStatus::Cancelled;
            if (Clock::now() >= deadline)
                return AgentTurnStatus::TimedOut;
            return std::nullopt;
        };
        const auto supported = ConfirmOllamaModelTools(config, request.model, stop);
        if (const auto status = stopped())
            return {*status};
        if (!supported.Succeeded()) {
            const auto &failure = *supported.error;
            return {.status = AgentTurnStatus::ProviderFailed, .providerError = failure.code, .retryAfter = failure.retryAfter};
        }
        const Json input = {{"type", "object"}, {"additionalProperties", false}};
        const auto session = OpenSceneTool(objectCount, input, std::move(entropy), observeAudit);
        if (const auto status = stopped())
            return {*status};
        if (session.HasError())
            return {AgentTurnStatus::ToolDenied};
        const auto &admitted = session.Value();
        const std::stop_callback revoke{stop, [&admitted] {
            admitted.authorization->Revoke(admitted.principal);
        }};
        const auto tools = static_cast<std::uint32_t>(ModelFeature::Tools);
        config.enabledFeatures.bits |= tools;
        request.requiredFeatures.bits |= tools;
        request.tools.emplace_back(std::string{LocalSceneCountTool}, std::string{SceneToolDescription}, input.dump());
        using namespace std::chrono_literals;
        config.timeout = std::max(1ms, std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()));
        auto provider = CreateOllamaModelProvider(std::move(config));
        AgentTurnRuntime runtime;
        return runtime.RunBefore(*provider, admitted.bridge, turnId, std::move(request), limits, deadline, stop, observeTool);
    }
}  // namespace Horo::Agent
