#include "Horo/Agent/ModelProvider.h"
#include "Horo/Agent/ReferenceModelProviders.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace {
    using namespace Horo::Agent;

    std::string Endpoint() {
        const char *value = std::getenv("HORO_MODEL_TEST_ENDPOINT");
        REQUIRE(value != nullptr);
        return value;
    }

    std::unique_ptr<IModelProvider> Create(bool cloud) {
        ModelProviderConfig config{Endpoint(), cloud ? "test-key-ref" : "", std::chrono::milliseconds{2000}};
        config.enabledFeatures.bits |= static_cast<std::uint32_t>(ModelFeature::Tools);
        return cloud ? CreateOpenAIModelProvider(std::move(config),
                                                 [](std::string_view reference) -> std::optional<std::string> {
            if (reference == "test-key-ref")
                return "fixture-token";
            return std::nullopt;
        })
                     : CreateOllamaModelProvider(std::move(config));
    }

    ModelRequest Request(std::string model) {
        ModelRequest request;
        request.model = std::move(model);
        request.messages = {{ModelRole::User, "hello", ""}};
        request.requiredFeatures.bits =
            static_cast<std::uint32_t>(ModelFeature::Streaming) | static_cast<std::uint32_t>(ModelFeature::Usage);
        return request;
    }
}  // namespace

TEST_CASE("Model provider registry is inert and feature admission is explicit", "[agent][model]") {
    ModelProviderRegistry registry;
    REQUIRE(registry.Register("local", [](const ModelProviderConfig &config) {
        return CreateOllamaModelProvider(config);
    }));
    REQUIRE_FALSE(registry.Register("local", [](const ModelProviderConfig &config) {
        return CreateOllamaModelProvider(config);
    }));
    REQUIRE(registry.ProviderIds() == std::vector<std::string>{"local"});
    REQUIRE(registry.Create("missing", {}) == nullptr);
    auto request = Request("normal");
    request.requiredFeatures.bits |= static_cast<std::uint32_t>(ModelFeature::Tools);
    const ModelFeatures onlyStreaming{static_cast<std::uint32_t>(ModelFeature::Streaming)};
    const auto outcome = AdmitModelRequest(request, onlyStreaming);
    REQUIRE_FALSE(outcome.Succeeded());
    REQUIRE(outcome.error->code == ModelErrorCode::UnsupportedCapability);
}

TEST_CASE("Real local and cloud HTTP adapters satisfy one streaming contract", "[agent][model][http]") {
    for (const bool cloud : {false, true}) {
        DYNAMIC_SECTION((cloud ? "cloud" : "local")) {
            auto provider = Create(cloud);
            const auto discovered = provider->Discover({});
            REQUIRE(discovered.outcome.Succeeded());
            REQUIRE(discovered.models.size() == 1);
            REQUIRE(discovered.models.front().id == "normal");
            REQUIRE(discovered.features.Has(ModelFeature::Tools));
            REQUIRE(discovered.residency == (cloud ? ModelResidency::External : ModelResidency::OnDevice));
            REQUIRE(discovered.costClass == (cloud ? ModelCostClass::Metered : ModelCostClass::LocalCompute));

            std::vector<ModelEvent> events;
            const auto outcome = provider->Stream(Request("normal"), [&](const ModelEvent &event) {
                events.push_back(event);
                return true;
            }, {});
            REQUIRE(outcome.Succeeded());
            REQUIRE(events.size() == 4);
            REQUIRE(events[0].kind == ModelEventKind::TextDelta);
            REQUIRE(events[0].text == "hello");
            REQUIRE(events[1].text == " world");
            REQUIRE(events[2].kind == ModelEventKind::Usage);
            REQUIRE(events[2].usage.inputTokens == 7);
            REQUIRE(events[2].usage.outputTokens == 2);
            REQUIRE(events[3].kind == ModelEventKind::Completed);
        }
    }
}

TEST_CASE("OpenAI adapter accepts SSE data fields without optional spaces and with CRLF", "[agent][model][http]") {
    auto provider = Create(true);
    std::vector<ModelEvent> events;
    const auto outcome = provider->Stream(Request("nospace"), [&](const ModelEvent &event) {
        events.push_back(event);
        return true;
    }, {});
    REQUIRE(outcome.Succeeded());
    REQUIRE(events.size() == 4);
    REQUIRE(events[0].text == "hello");
    REQUIRE(events[1].text == " world");
    REQUIRE(events[2].kind == ModelEventKind::Usage);
    REQUIRE(events[3].kind == ModelEventKind::Completed);
}

TEST_CASE("Real adapters assemble tool intents without executing them", "[agent][model][http]") {
    for (const bool cloud : {false, true}) {
        DYNAMIC_SECTION((cloud ? "cloud" : "local")) {
            auto provider = Create(cloud);
            auto request = Request("tools");
            request.requiredFeatures.bits |= static_cast<std::uint32_t>(ModelFeature::Tools);
            request.tools = {{"scene_query", "Read scene", R"({"type":"object","properties":{}})"}};
            std::vector<ModelEvent> events;
            const auto outcome = provider->Stream(request, [&](const ModelEvent &event) {
                events.push_back(event);
                return true;
            }, {});
            REQUIRE(outcome.Succeeded());
            REQUIRE(events.size() == 3);
            REQUIRE(events[0].kind == ModelEventKind::ToolIntent);
            REQUIRE(events[0].tool.name == "scene_query");
            REQUIRE_FALSE(events[0].tool.callId.empty());
            REQUIRE(events[0].tool.argumentsJson == R"({"target":"scene"})");
            REQUIRE(events[1].kind == ModelEventKind::Usage);
            REQUIRE(events[2].kind == ModelEventKind::Completed);
        }
    }
}

TEST_CASE("Real adapters carry the assistant intent and tool result into the next request", "[agent][model][http]") {
    for (const bool cloud : {false, true}) {
        DYNAMIC_SECTION((cloud ? "cloud" : "local")) {
            auto provider = Create(cloud);
            auto request = Request("history");
            request.messages.push_back({ModelRole::Assistant, "", "", {{"call-1", "scene_query", R"({"target":"scene"})"}}});
            request.messages.push_back({ModelRole::Tool, R"({"nodes":2})", "call-1"});
            const auto outcome = provider->Stream(request, [](const ModelEvent &) {
                return true;
            }, {});
            REQUIRE(outcome.Succeeded());
        }
    }
}

TEST_CASE("Real adapters preserve cancellation and sanitized failures", "[agent][model][http]") {
    for (const bool cloud : {false, true}) {
        DYNAMIC_SECTION((cloud ? "cloud" : "local")) {
            auto provider = Create(cloud);
            std::vector<ModelEvent> events;
            const auto cancelled = provider->Stream(Request("normal"), [&](const ModelEvent &event) {
                events.push_back(event);
                return false;
            }, {});
            REQUIRE(cancelled.error->code == ModelErrorCode::Cancelled);
            REQUIRE(events.size() == 1);

            std::stop_source source;
            source.request_stop();
            const auto beforeIo = provider->Stream(Request("normal"), [](const ModelEvent &) {
                return true;
            }, source.get_token());
            REQUIRE(beforeIo.error->code == ModelErrorCode::Cancelled);

            const auto malformed = provider->Stream(Request("malformed"), [](const ModelEvent &) {
                return true;
            }, {});
            REQUIRE(malformed.error->code == ModelErrorCode::Protocol);

            const auto limited = provider->Stream(Request("limited"), [](const ModelEvent &) {
                return true;
            }, {});
            REQUIRE(limited.error->code == ModelErrorCode::RateLimited);
            REQUIRE(limited.error->message.find("private") == std::string::npos);
        }
    }
}

TEST_CASE("Cloud credentials stay referenced and are required before dispatch", "[agent][model]") {
    auto provider = CreateOpenAIModelProvider({Endpoint(), "missing-ref", std::chrono::milliseconds{2000}},
                                              [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    });
    const auto outcome = provider->Stream(Request("normal"), [](const ModelEvent &) {
        return true;
    }, {});
    REQUIRE(outcome.error->code == ModelErrorCode::CredentialUnavailable);
    REQUIRE(outcome.error->message.find("missing-ref") == std::string::npos);
}

TEST_CASE("Real adapters reject unconfirmed tool support before network dispatch", "[agent][model]") {
    for (const bool cloud : {false, true}) {
        ModelProviderConfig config{Endpoint(), cloud ? "test-key-ref" : "", std::chrono::milliseconds{2000}};
        auto provider = cloud ? CreateOpenAIModelProvider(std::move(config),
                                                          [](std::string_view) -> std::optional<std::string> {
            return "fixture-token";
        })
                              : CreateOllamaModelProvider(std::move(config));
        auto request = Request("tools");
        request.tools = {{"scene_query", "Read scene", R"({"type":"object"})"}};
        const auto outcome = provider->Stream(request, [](const ModelEvent &) {
            return true;
        }, {});
        REQUIRE(outcome.error->code == ModelErrorCode::UnsupportedCapability);
    }
}
