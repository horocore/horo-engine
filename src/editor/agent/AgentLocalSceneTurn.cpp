#include "AgentLocalSceneTurn.h"

#include "AgentTurnMcp.h"
#include "Horo/Agent/ReferenceModelProviders.h"
#include "Horo/Mcp/McpAuthorization.h"
#include "Horo/Mcp/McpController.h"

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

        /** @brief Authenticates an embedded session scoped to one immutable result and this worker. */
        Result<AgentTurnMcp> OpenSceneTool(std::size_t count, const Json &input, std::shared_ptr<Security::SecureRandomSource> entropy) {
            using Outcome = Result<AgentTurnMcp>;
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
            auto embedded = Mcp::McpInProcessAdapter::Start(std::move(sessions).Value(), std::move(admission));
            if (embedded.HasError())
                return Outcome::Failure(embedded.ErrorValue());
            return Outcome::Success(AgentTurnMcp{std::move(embedded).Value(), {}, [controller = std::move(configured)] {
                const auto result = controller->Pump(Owner);
                return result.HasValue() ? Result<void>::Success() : Result<void>::Failure(result.ErrorValue());
            }});
        }
    }  // namespace

    /** @copydoc RunApprovedLocalSceneTurn */
    AgentTurnResult RunApprovedLocalSceneTurn(ModelProviderConfig config, ModelRequest request, std::uint64_t turnId,
                                              std::size_t objectCount, std::shared_ptr<Security::SecureRandomSource> entropy,
                                              std::stop_token stop, const AgentToolActivityObserver &observeTool) {
        if (!request.tools.empty() || turnId == 0)
            return {AgentTurnStatus::InvalidRequest};
        if (const auto supported = ConfirmOllamaModelTools(config, request.model, stop); !supported.Succeeded())
            return {.status = AgentTurnStatus::ProviderFailed, .providerError = supported.error->code};
        const Json input = {{"type", "object"}, {"additionalProperties", false}};
        const auto bridge = OpenSceneTool(objectCount, input, std::move(entropy));
        if (bridge.HasError())
            return {AgentTurnStatus::ToolDenied};
        const auto tools = static_cast<std::uint32_t>(ModelFeature::Tools);
        config.enabledFeatures.bits |= tools;
        request.requiredFeatures.bits |= tools;
        request.tools.emplace_back(std::string{LocalSceneCountTool}, std::string{SceneToolDescription}, input.dump());
        auto provider = CreateOllamaModelProvider(std::move(config));
        AgentTurnRuntime runtime;
        return runtime.Run(*provider, &bridge.Value(), turnId, std::move(request), {.maximumToolCalls = 1}, stop, nullptr, {}, observeTool);
    }
}  // namespace Horo::Agent
