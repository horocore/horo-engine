#pragma once

#include "AgentTurnMcp.h"
#include "Horo/Mcp/McpController.h"
#include "McpAuthorizationTestSupport.h"

#include <future>

namespace Horo::Agent::TestSupport {
    using namespace std::chrono_literals;

    /** @brief Real registered application adapter, invoked only while the owner pumps the MCP controller. */
    struct SceneTool final : Mcp::IMcpToolAdapter {
        std::size_t calls{};

        Result<nlohmann::json> Invoke(const nlohmann::json &, const Mcp::McpRequestContext &) override {
            ++calls;
            return Result<nlohmann::json>::Success({{"count", 3}});
        }
    };

    struct Tools final {
        std::shared_ptr<SceneTool> scene = std::make_shared<SceneTool>();
        std::shared_ptr<Mcp::McpController> owner;
        std::shared_ptr<Mcp::McpInProcessAdapter> adapter;

        explicit Tools(const Mcp::McpToolEffect effect = Mcp::McpToolEffect::Query) {
            const auto auth = Mcp::Test::Authorization();
            auto registry = std::make_shared<Mcp::McpToolRegistry>(auth);
            auto revision = registry->Publish({{.descriptor = {.id = {"scene.inspect"},
                                                               .description = "Test scene summary",
                                                               .inputSchema = {{"type", "object"}},
                                                               .outputSchema = {{"type", "object"}},
                                                               .effect = effect},
                                                .adapter = scene,
                                                .owner = Mcp::McpOwnerContext::Editor}});
            REQUIRE(revision.HasValue());
            auto created = Mcp::McpController::Create(registry, {}, auth);
            REQUIRE(created.HasValue());
            owner = std::move(created).Value();
            REQUIRE(owner->BindOwner(Mcp::McpOwnerContext::Editor).HasValue());
            auto manager = Mcp::McpSessionManager::Create(owner, {}, auth);
            REQUIRE(manager.HasValue());
            auto opened =
                Mcp::McpInProcessAdapter::Start(std::move(manager).Value(), Mcp::Test::Authenticate({.clientIdentity = "agent-test",
                                                                                                     .registryRevision = revision.Value()},
                                                                                                    auth));
            REQUIRE(opened.HasValue());
            adapter = std::move(opened).Value();
        }

        AgentTurnResult Run(IModelProvider &model, ModelRequest request, AgentTurnLimits limits = {}) const {
            limits.timeout = 2s;
            const AgentTurnMcp bridge{adapter};
            AgentTurnRuntime runtime;
            auto pending = std::async(std::launch::async, [&runtime, &model, &bridge, &request, &limits] {
                return runtime.Run(model, &bridge, 23, std::move(request), limits);
            });
            while (pending.wait_for(1ms) != std::future_status::ready)
                REQUIRE(owner->Pump(Mcp::McpOwnerContext::Editor).HasValue());
            return pending.get();
        }
    };
}  // namespace Horo::Agent::TestSupport
