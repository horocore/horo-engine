#include "AgentTurn.h"
#include "AgentTurnTestTools.h"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
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

        using TestSupport::Tools;

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
                return Emit(sink,
                            {Intent("inspect-1"), {.kind = ModelEventKind::Usage, .usage = {8, 3}}, {.kind = ModelEventKind::Completed}});
            return Emit(sink, {{.kind = ModelEventKind::TextDelta, .text = "Three objects"},
                               {.kind = ModelEventKind::Usage, .usage = {24, 2}},
                               {.kind = ModelEventKind::Completed}});
        };
        const auto result = tools.Run(model, Request(true));
        REQUIRE(result.status == AgentTurnStatus::Completed);
        REQUIRE(result.messages.size() == 4);
        REQUIRE(model.requests.size() == 2);
        CHECK(tools.scene->calls == 1);
        CHECK(model.requests.back().messages.back().toolCallId == "inspect-1");
        CHECK(model.requests.back().messages.back().text == R"({"count":3})");
        CHECK(result.messages.back().text == "Three objects");
        REQUIRE(result.finalUsage);
        CHECK(result.finalUsage->inputTokens == 24);
        CHECK(result.finalUsage->outputTokens == 2);
    }

    TEST_CASE("Editor Agent plans registered tool intents without creating operations", "[agent][turn][plan]") {
        using enum AgentTurnStatus;
        AgentTurnRuntime runtime;
        Model model;
        auto sibling = Intent("second");
        auto expected = Completed;
        std::stop_source stop;
        bool cancelDuringReview{};
        auto registryEffect = Mcp::McpToolEffect::Mutation;
        auto expectedEffect = AgentPlannedEffect::Mutation;
        SECTION("mutation proposals remain inert") {}
        SECTION("queries publish the registered read effect") {
            registryEffect = Mcp::McpToolEffect::Query;
            expectedEffect = AgentPlannedEffect::Read;
        }
        SECTION("viewport operations publish the registered presentation effect") {
            registryEffect = Mcp::McpToolEffect::PresentationSideEffect;
            expectedEffect = AgentPlannedEffect::Presentation;
        }
        SECTION("a registered tool rejects arguments that violate its schema") {
            sibling.tool.argumentsJson = R"({"unknown":true})";
            expected = InvalidModelResponse;
        }
        SECTION("a tool omitted by the host cannot appear in a plan") {
            sibling.tool.name = "scene.unregistered";
            expected = ToolDenied;
        }
        SECTION("cancellation during review discards the whole draft") {
            cancelDuringReview = true;
            expected = Cancelled;
        }
        Tools tools{registryEffect};
        model.action = [&](std::size_t, const ModelEventSink &sink, std::stop_token) {
            return Emit(sink,
                        {Intent("first"), sibling, {.kind = ModelEventKind::Usage, .usage = {8, 2}}, {.kind = ModelEventKind::Completed}});
        };
        const auto result = runtime.Plan(model, 1, Request(true), {}, [&](const ModelToolIntent &intent) {
            const auto inspected = InspectMcpPlanTool(*tools.snapshot, {}, intent);
            if (cancelDuringReview)
                stop.request_stop();
            return inspected;
        }, stop.get_token());
        CHECK(result.status == expected);
        CHECK(model.requests.size() == 1);
        CHECK(tools.scene->calls == 0);
        REQUIRE(result.draft.has_value() == (expected == Completed));
        if (result.draft) {
            CHECK(result.draft->toolIntents.size() == 2);
            REQUIRE(result.tools.size() == 2);
            CHECK(result.tools[0].effect == expectedEffect);
            CHECK(result.tools[1].effect == expectedEffect);
            REQUIRE(result.usage);
            CHECK(result.usage->outputTokens == 2);
        } else
            CHECK(result.tools.empty());
    }

    TEST_CASE("Editor Agent observes only accepted text deltas from a tool-free turn", "[agent][turn]") {
        Model model;
        model.action = [](std::size_t, const ModelEventSink &sink, std::stop_token) {
            return Emit(sink, {{.kind = ModelEventKind::TextDelta, .text = "First\n"},
                               {.kind = ModelEventKind::Usage, .usage = {4, 2}},
                               {.kind = ModelEventKind::TextDelta, .text = "Second"},
                               {.kind = ModelEventKind::Completed}});
        };
        AgentTurnRuntime runtime;
        std::vector<std::string> deltas;
        const AgentTurnTextObserver observe = [&](std::string_view delta) {
            deltas.emplace_back(delta);
        };
        const auto result = runtime.Run(model, nullptr, 1, Request(), {}, {}, nullptr, observe);
        REQUIRE(result.status == AgentTurnStatus::Completed);
        CHECK((deltas == std::vector<std::string>{"First\n", "Second"}));
        CHECK(runtime.Run(model, nullptr, 2, Request(true), {}, {}, nullptr, observe).status == AgentTurnStatus::InvalidRequest);
        CHECK(model.requests.size() == 1);
    }

    TEST_CASE("Editor Agent checks turn revocation after announcing and before dispatching a tool", "[agent][turn]") {
        Tools tools;
        const AgentTurnMcp bridge{tools.adapter};
        Model model;
        model.action = [](std::size_t, const ModelEventSink &sink, std::stop_token) {
            return Emit(sink, {Intent("inspect"), {.kind = ModelEventKind::Usage, .usage = {8, 3}}, {.kind = ModelEventKind::Completed}});
        };
        bool current = true;
        AgentTurnRuntime runtime{[&current] {
            return current;
        }};
        const auto result =
            runtime.Run(model, &bridge, 1, Request(true), {.timeout = 2s}, {}, nullptr, {}, [&](const AgentToolActivity &activity) {
            if (!activity.outcome)
                current = false;
        });
        CHECK_FALSE(current);
        CHECK(result.status == AgentTurnStatus::ContextDenied);
        CHECK(result.messages.empty());
        CHECK(tools.scene->calls == 0);
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
        const auto result = tools.Run(model, Request(true));
        CHECK(result.status == AgentTurnStatus::InvalidModelResponse);
        CHECK(result.messages.empty());
        CHECK_FALSE(result.finalUsage);
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
        const auto result = tools.Run(model, Request(true), budgets);
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

    TEST_CASE("Editor Agent settles cancellation and bridge failure within a tool round", "[agent][turn]") {
        struct Bridge final : IAgentTurnTools {
            explicit Bridge(std::stop_source &parent) : parent(parent) {}

            std::stop_source &parent;
            bool cancelDuringAdmission{};
            bool failDuringDispatch{};
            mutable std::size_t calls{};

            bool ValidArguments(std::string_view) const override {
                if (cancelDuringAdmission)
                    parent.request_stop();
                return true;
            }

            AgentTurnStatus Run(std::uint64_t, std::size_t, const ModelToolIntent &, std::chrono::steady_clock::time_point, std::stop_token,
                                std::string &result) const override {
                ++calls;
                if (failDuringDispatch)
                    throw std::runtime_error("Bridge failed");
                parent.request_stop();
                result = "{}";
                return AgentTurnStatus::Completed;
            }
        };

        std::stop_source parent;
        Bridge bridge{parent};
        auto expected = AgentTurnStatus::Cancelled;
        std::size_t expectedCalls = 1;
        bool observerThrows{};
        bool cancelAtStart{};
        SECTION("cancellation during admission stops before dispatch") {
            bridge.cancelDuringAdmission = true;
            expectedCalls = 0;
        }
        SECTION("cancellation during dispatch prevents further tools") {}
        SECTION("presentation exceptions do not change tool settlement") {
            observerThrows = true;
        }
        SECTION("cancellation after admission prevents bridge execution") {
            cancelAtStart = true;
            expectedCalls = 0;
        }
        SECTION("a throwing bridge produces one tool failure") {
            bridge.failDuringDispatch = true;
            expected = AgentTurnStatus::ToolFailed;
        }

        Model model;
        model.action = [](std::size_t, const ModelEventSink &sink, std::stop_token) {
            return Emit(sink, {Intent("first"), Intent("second"), {.kind = ModelEventKind::Completed}});
        };
        AgentTurnRuntime runtime;
        std::vector<AgentToolActivity> activity;
        const auto result =
            runtime.Run(model, &bridge, 51, Request(true), {}, parent.get_token(), nullptr, {}, [&](const AgentToolActivity event) {
            activity.push_back(event);
            if (cancelAtStart && !event.outcome)
                parent.request_stop();
            if (observerThrows)
                throw std::runtime_error("Presentation failed");
        });
        CHECK(result.status == expected);
        CHECK(result.messages.empty());
        CHECK(bridge.calls == expectedCalls);
        CHECK(model.requests.size() == 1);
        const bool observed = expectedCalls != 0 || cancelAtStart;
        REQUIRE(activity.size() == (observed ? 2 : 0));
        if (observed) {
            CHECK(activity.front().ordinal == 1);
            CHECK_FALSE(activity.front().outcome);
            CHECK(activity.back().ordinal == 1);
            CHECK(activity.back().outcome == expected);
        }
    }

    TEST_CASE("Editor Agent deadline cancels the exact still queued authoring tool", "[agent][turn]") {
        Tools tools;
        Model model;
        model.action = [](std::size_t, const ModelEventSink &sink, std::stop_token) {
            return Emit(sink, {Intent("inspect-1"), {.kind = ModelEventKind::Completed}});
        };
        const AgentTurnMcp bridge{tools.adapter};
        AgentTurnRuntime runtime;
        std::vector<AgentToolActivity> activity;
        const auto result = runtime.Run(model, &bridge, 52, Request(true), AgentTurnLimits{.timeout = 30ms}, {}, nullptr, {},
                                        [&](const AgentToolActivity event) {
            activity.push_back(event);
        });
        CHECK(result.status == AgentTurnStatus::TimedOut);
        CHECK(result.messages.empty());
        REQUIRE(activity.size() == 2);
        CHECK_FALSE(activity.front().outcome);
        CHECK(activity.back().outcome == AgentTurnStatus::TimedOut);
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
