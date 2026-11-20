#include "AgentTurn.h"
#include "AgentTurnMcp.h"
#include "Horo/Mcp/McpController.h"
#include "Horo/Mcp/McpInProcessAdapter.h"
#include "McpAuthorizationTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <future>
#include <thread>

namespace Horo::Agent {
    namespace {
        using namespace std::chrono_literals;

        /** @brief Supplies provider events without performing I/O or interpreting tool results. */
        struct Model final : IModelProvider {
            std::function<ModelOutcome(std::size_t, const ModelEventSink &, std::stop_token)> action;
            std::vector<ModelRequest> requests;

            ProviderDiscovery Discover(std::stop_token) override {
                return {};
            }

            ModelOutcome Stream(const ModelRequest &request, const ModelEventSink &sink, std::stop_token token) override {
                requests.push_back(request);
                return action(requests.size(), sink, token);
            }
        };

        /** @brief Emits until the collector cancels this scripted provider. */
        ModelOutcome Emit(const ModelEventSink &sink, const std::initializer_list<ModelEvent> events) {
            for (const auto &event : events)
                if (!sink(event))
                    return {ModelError{ModelErrorCode::Cancelled, "Stopped by callback", std::nullopt}};
            return {};
        }

        ModelRequest Request(bool useTool = false) {
            ModelRequest request{.model = "qualification", .messages = {{.role = ModelRole::User, .text = "Inspect the scene"}}};
            if (useTool) {
                request.tools.push_back({"scene.inspect", "Read a bounded engine-owned scene summary", R"({"type":"object"})"});
                request.requiredFeatures.bits |= static_cast<std::uint32_t>(ModelFeature::Tools);
            }
            return request;
        }

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
                auto opened = Mcp::McpInProcessAdapter::Start(std::move(manager).Value(),
                                                              Mcp::Test::Authenticate({.clientIdentity = "agent-test",
                                                                                       .registryRevision = revision.Value()},
                                                                                      auth));
                REQUIRE(opened.HasValue());
                adapter = std::move(opened).Value();
            }

            AgentTurnResult Run(Model &model, AgentTurnLimits limits = {}) const {
                limits.timeout = 2s;
                const AgentTurnMcp bridge{adapter};
                AgentTurnRuntime runtime;
                auto pending = std::async(std::launch::async, [&] {
                    return runtime.Run(model, &bridge, 23, Request(true), limits);
                });
                while (pending.wait_for(1ms) != std::future_status::ready)
                    REQUIRE(owner->Pump(Mcp::McpOwnerContext::Editor).HasValue());
                return pending.get();
            }
        };

        /** @brief Forms one inert response intent for the registered query. */
        ModelEvent Intent(std::string id, std::string arguments = "{}") {
            return {.kind = ModelEventKind::ToolIntent, .tool = {std::move(id), "scene.inspect", std::move(arguments)}};
        }
    }  // namespace

    TEST_CASE("Editor Agent receives an exact MCP tool result before continuing the conversation", "[agent][turn]") {
        Tools tools;
        Model model;
        model.action = [](const std::size_t round, const ModelEventSink &sink, std::stop_token) {
            if (round == 1)
                return Emit(sink, {Intent("inspect-1"), {.kind = ModelEventKind::Completed}});
            return Emit(sink, {{.kind = ModelEventKind::TextDelta, .text = "Three objects"}, {.kind = ModelEventKind::Completed}});
        };
        const auto result = tools.Run(model);
        REQUIRE(result.status == AgentTurnStatus::Completed);
        REQUIRE(result.messages.size() == 4);
        REQUIRE(model.requests.size() == 2);
        CHECK(tools.scene->calls == 1);
        CHECK(model.requests.back().messages.back().toolCallId == "inspect-1");
        CHECK(model.requests.back().messages.back().text == R"({"count":3})");
        CHECK(result.messages.back().text == "Three objects");
    }

    TEST_CASE("Editor Agent discards malformed sibling intents before making any tool visible", "[agent][turn]") {
        Tools tools;
        Model model;
        bool repeatedId = false;
        SECTION("duplicate ID") {
            repeatedId = true;
        }
        model.action = [repeatedId](std::size_t, const ModelEventSink &sink, std::stop_token) {
            return Emit(sink,
                        {Intent("first"), repeatedId ? Intent("first") : Intent("second", "[]"), {.kind = ModelEventKind::Completed}});
        };
        const auto result = tools.Run(model);
        CHECK(result.status == AgentTurnStatus::InvalidModelResponse);
        CHECK(result.messages.empty());
        CHECK(tools.scene->calls == 0);
    }

    TEST_CASE("Editor Agent tool denial and exhaustion produce one terminal outcome", "[agent][turn]") {
        auto expected = AgentTurnStatus::BudgetExceeded;
        auto effect = Mcp::McpToolEffect::Query;
        auto budgets = AgentTurnLimits{.maximumRounds = 1};
        SECTION("bound stops before tool effects when no continuation would be possible") {}
        SECTION("mutation needs separate host approval") {
            effect = Mcp::McpToolEffect::Mutation;
            expected = AgentTurnStatus::ToolDenied;
            budgets.maximumRounds = 2;
        }
        Tools tools{effect};
        Model model;
        model.action = [](std::size_t, const ModelEventSink &sink, std::stop_token) {
            return Emit(sink, {Intent("inspect-1"), {.kind = ModelEventKind::Completed}});
        };
        const auto result = tools.Run(model, budgets);
        CHECK(result.status == expected);
        CHECK(result.messages.empty());
        CHECK(model.requests.size() == 1);
        CHECK(tools.scene->calls == 0);
    }

    TEST_CASE("Editor Agent cancels cooperative providers and charges emitted data before retaining it", "[agent][turn]") {
        Model model;
        std::stop_source parent;
        AgentTurnLimits limits;
        auto expected = AgentTurnStatus::ProviderFailed;
        model.action = [](std::size_t, const ModelEventSink &, std::stop_token) {
            return ModelOutcome{ModelError{ModelErrorCode::Unavailable, "Provider unavailable", std::nullopt}};
        };
        SECTION("cancellation before admission") {
            expected = AgentTurnStatus::Cancelled;
            parent.request_stop();
        }
        SECTION("deadline during inference") {
            expected = AgentTurnStatus::TimedOut;
            limits.timeout = 30ms;
            model.action = [](std::size_t, const ModelEventSink &, const std::stop_token token) {
                const auto failsafe = std::chrono::steady_clock::now() + 2s;
                while (!token.stop_requested() && std::chrono::steady_clock::now() < failsafe)
                    std::this_thread::sleep_for(1ms);
                return ModelOutcome{ModelError{ModelErrorCode::Cancelled, "Stopped", std::nullopt}};
            };
        }
        SECTION("oversized output") {
            expected = AgentTurnStatus::BudgetExceeded;
            limits.maximumHistoryBytes = 40;
            model.action = [](std::size_t, const ModelEventSink &sink, std::stop_token) {
                return Emit(sink, {{.kind = ModelEventKind::TextDelta, .text = std::string(40, 'a')}});
            };
        }
        SECTION("provider failure") {}
        AgentTurnRuntime runtime;
        const auto result = runtime.Run(model, nullptr, 23, Request(), limits, parent.get_token());
        CHECK(result.status == expected);
        CHECK(result.messages.empty());
    }

    TEST_CASE("Editor Agent deadline cancels the exact still queued authoring tool", "[agent][turn]") {
        Tools tools;
        Model model;
        model.action = [](std::size_t, const ModelEventSink &sink, std::stop_token) {
            return Emit(sink, {Intent("inspect-1"), {.kind = ModelEventKind::Completed}});
        };
        const AgentTurnMcp bridge{tools.adapter};
        AgentTurnRuntime runtime;
        const auto result = runtime.Run(model, &bridge, 52, Request(true), AgentTurnLimits{.timeout = 30ms});
        CHECK(result.status == AgentTurnStatus::TimedOut);
        CHECK(result.messages.empty());
        REQUIRE(tools.owner->Pump(Mcp::McpOwnerContext::Editor).HasValue());
        CHECK(tools.scene->calls == 0);
    }

    TEST_CASE("Editor Agent rejects overlapping authoring turns and releases admission on completion", "[agent][turn]") {
        AgentTurnRuntime runtime;
        Model model;
        AgentTurnStatus nested{};
        model.action = [&](std::size_t, const ModelEventSink &sink, std::stop_token) {
            nested = runtime.Run(model, nullptr, 24, Request(), {}).status;
            return Emit(sink, {{.kind = ModelEventKind::TextDelta, .text = "Ready"}, {.kind = ModelEventKind::Completed}});
        };
        CHECK(runtime.Run(model, nullptr, 23, Request(), {}).status == AgentTurnStatus::Completed);
        CHECK(nested == AgentTurnStatus::Busy);
        CHECK(runtime.Run(model, nullptr, 25, Request(), {}).status == AgentTurnStatus::Completed);
        CHECK(model.requests.size() == 2);
    }
}  // namespace Horo::Agent
