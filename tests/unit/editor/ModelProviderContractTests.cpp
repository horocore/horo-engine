#include "AgentLocalSceneTurn.h"
#include "AgentTurnTestTools.h"
#include "AllocationProbe.h"
#include "Horo/Agent/ModelProvider.h"
#include "Horo/Agent/ReferenceModelProviders.h"
#include "Horo/Security/CredentialStore.h"
#include "Horo/Security/SecurityErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace {
    using namespace Horo::Agent;

    std::string Endpoint(const char *variable = "HORO_MODEL_TEST_ENDPOINT") {
        const char *value = std::getenv(variable);
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

    /** @brief Records only non-secret lookup metadata and simulates a vault rejecting a reference. */
    class UnavailableCredentialProvider final : public Horo::Security::CredentialProvider {
    public:
        Horo::Result<Horo::Security::SecureBytes> Resolve(const Horo::CredentialReference &reference, std::uint64_t nowMillis) override {
            references.emplace_back(reference.Value());
            times.push_back(nowMillis);
            return Horo::Result<Horo::Security::SecureBytes>::Failure(
                Horo::MakeError(Horo::SecurityErrors::CredentialRevoked, "private vault detail"));
        }

        std::vector<std::string> references;
        std::vector<std::uint64_t> times;
    };
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

TEST_CASE("Local HTTP inference completes an approved editor MCP tool round without credentials", "[agent][model][http]") {
    auto provider = Create(false);
    const auto discovery = provider->Discover({});
    REQUIRE(discovery.outcome.Succeeded());
    CHECK(discovery.residency == ModelResidency::OnDevice);
    REQUIRE(discovery.models.size() == 1);
    CHECK(discovery.models.front().id == "normal");
    auto request = Request("editor-local-tool");
    request.tools.push_back({"scene.inspect", "Read a bounded scene summary", R"({"type":"object"})"});
    request.requiredFeatures.bits |= static_cast<std::uint32_t>(ModelFeature::Tools);
    Horo::Agent::TestSupport::Tools tools;
    const auto result = tools.Run(*provider, std::move(request));
    REQUIRE(result.status == AgentTurnStatus::Completed);
    REQUIRE(result.messages.size() == 4);
    CHECK(tools.scene->calls == 1);
    CHECK(result.messages.back().text == "Three objects");
}

TEST_CASE("The editor local scene turn probes model tools and returns only the approved count", "[agent][model][http]") {
    const ModelProviderConfig config{.endpoint = Endpoint(), .timeout = std::chrono::seconds{2}};
    auto entropy = std::make_shared<Horo::Mcp::Test::Entropy>();
    std::vector<AgentToolActivity> activity;
    const auto observe = [&](const AgentToolActivity event) {
        activity.push_back(event);
    };
    const auto result = RunApprovedLocalSceneTurn(config, Request("editor-approved-tool"), 7, 3, entropy, {}, observe);
    REQUIRE(result.status == AgentTurnStatus::Completed);
    REQUIRE(result.messages.size() == 4);
    CHECK(result.messages[2].role == ModelRole::Tool);
    CHECK(result.messages[2].text == R"({"count":3})");
    CHECK(result.messages.back().text == "Three objects");
    REQUIRE(activity.size() == 2);
    CHECK(activity.front().ordinal == 1);
    CHECK_FALSE(activity.front().outcome);
    CHECK(activity.back().ordinal == 1);
    CHECK(activity.back().outcome == AgentTurnStatus::Completed);

    activity.clear();
    const auto unsupported = RunApprovedLocalSceneTurn(config, Request("normal"), 8, 4, entropy, {}, observe);
    CHECK(unsupported.status == AgentTurnStatus::ProviderFailed);
    CHECK(unsupported.providerError == ModelErrorCode::UnsupportedCapability);
    CHECK(unsupported.messages.empty());
    CHECK(activity.empty());
}

TEST_CASE("Local model discovery rejects malformed loopback authorities before transport", "[agent][model][http]") {
    for (const auto endpoint :
         {"http://localhost:0", "http://127.0.0.1:65536", "http://localhost:port", "http://[::1]:", "http://127.0.0.1:11434\\alternate"}) {
        CAPTURE(endpoint);
        const auto provider = CreateOllamaModelProvider({.endpoint = endpoint});
        const auto discovery = provider->Discover({});
        REQUIRE(discovery.outcome.error.has_value());
        CHECK(discovery.outcome.error->code == ModelErrorCode::InvalidRequest);
        CHECK_FALSE(discovery.outcome.error->httpStatus.has_value());
    }
}

TEST_CASE("Local editor inference reports unavailable or missing models and the same provider recovers", "[agent][model][http]") {
    auto provider = Create(false);
    AgentTurnRuntime runtime;
    const auto unavailable = runtime.Run(*provider, nullptr, 1, Request("editor-local-restart"), {});
    CHECK(unavailable.status == AgentTurnStatus::ProviderFailed);
    CHECK(unavailable.providerError == ModelErrorCode::Unavailable);
    CHECK(unavailable.messages.empty());

    const auto recovered = runtime.Run(*provider, nullptr, 2, Request("editor-local-restart"), {});
    REQUIRE(recovered.status == AgentTurnStatus::Completed);
    REQUIRE(recovered.messages.size() == 2);
    CHECK(recovered.messages.back().text == "hello world");

    const auto missing = runtime.Run(*provider, nullptr, 3, Request("editor-local-missing"), {});
    CHECK(missing.status == AgentTurnStatus::ProviderFailed);
    CHECK(missing.providerError == ModelErrorCode::InvalidRequest);
    CHECK(missing.messages.empty());
}

TEST_CASE("Local tool results require an exact pending assistant intent", "[agent][model][http]") {
    auto provider = Create(false);
    auto request = Request("history");
    request.messages.push_back({ModelRole::Assistant, "", "", {{"call-1", "scene_query", "{}"}}});
    request.messages.push_back({ModelRole::Tool, R"({"nodes":2})", "unknown-call"});
    const auto outcome = provider->Stream(request, [](const ModelEvent &) {
        return true;
    }, {});
    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->code == ModelErrorCode::InvalidRequest);
    CHECK_FALSE(outcome.error->httpStatus.has_value());
}

TEST_CASE("Local inference distinguishes a stopped service and a loading model", "[agent][model][http]") {
    auto offline =
        CreateOllamaModelProvider({.endpoint = Endpoint("HORO_MODEL_TEST_OFFLINE_ENDPOINT"), .timeout = std::chrono::milliseconds{250}});
    const auto discovery = offline->Discover({});
    REQUIRE(discovery.outcome.error);
    CHECK(discovery.outcome.error->code == ModelErrorCode::Unavailable);
    CHECK(discovery.outcome.error->message.find("configured port") != std::string::npos);
    CHECK(discovery.models.empty());

    auto loading = CreateOllamaModelProvider({.endpoint = Endpoint(), .timeout = std::chrono::milliseconds{200}});
    std::vector<ModelEvent> events;
    const auto status = loading->Stream(Request("editor-local-loading"), [&events](const ModelEvent &event) {
        events.push_back(event);
        return true;
    }, {});
    REQUIRE(status.error);
    CHECK(status.error->code == ModelErrorCode::Unavailable);
    CHECK(status.error->message.find("model to load") != std::string::npos);
    CHECK(events.empty());
}

TEST_CASE("Local cancellation interrupts a quiet stream before it can publish completion", "[agent][model][http]") {
    auto provider = CreateOllamaModelProvider({.endpoint = Endpoint(), .timeout = std::chrono::seconds{8}});
    std::stop_source source;
    std::vector<ModelEventKind> events;
    const auto started = std::chrono::steady_clock::now();
    const auto status = provider->Stream(Request("editor-local-idle"), [&events, &source](const ModelEvent &event) {
        events.push_back(event.kind);
        source.request_stop();
        return true;
    }, source.get_token());
    REQUIRE(status.error);
    CHECK(status.error->code == ModelErrorCode::Cancelled);
    CHECK(events == std::vector{ModelEventKind::TextDelta});
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{5});
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

TEST_CASE("Cloud adapter preserves HTTP failure status after a truncated error body", "[agent][model][http]") {
    auto provider = Create(true);
    const auto [model, status, code] = GENERATE(Catch::Generators::table<const char *, long, ModelErrorCode>({
        {"truncated-authentication", 401, ModelErrorCode::Authentication},
        {"truncated-rate-limit", 429, ModelErrorCode::RateLimited},
    }));
    const auto outcome = provider->Stream(Request(model), [](const ModelEvent &) {
        FAIL("An HTTP error cannot publish a model event");
        return true;
    }, {});
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == code);
    CHECK(outcome.error->httpStatus == status);
    CHECK(outcome.error->message.find("private") == std::string::npos);

    const auto recovered = provider->Stream(Request("normal"), [](const ModelEvent &) {
        return true;
    }, {});
    CHECK(recovered.Succeeded());
}

TEST_CASE("HTTP adapters bound response records even without a final newline", "[agent][model][http]") {
    const auto cloud = GENERATE(false, true);
    const auto model = GENERATE("oversized-record", "oversized-final-fragment");
    auto provider = Create(cloud);
    const auto outcome = provider->Stream(Request(model), [](const ModelEvent &) {
        FAIL("An oversized response cannot publish an event");
        return true;
    }, {});
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ModelErrorCode::Protocol);
    CHECK(outcome.error->message == "Model response exceeds limit");

    const auto recovered = provider->Stream(Request("normal"), [](const ModelEvent &) {
        return true;
    }, {});
    CHECK(recovered.Succeeded());
}

TEST_CASE("HTTP response buffer allocation failure stays inside the C callback boundary", "[agent][model][http]") {
    struct FailurePoint final {
        std::optional<Horo::Tests::AllocationProbe::ScopedFailure> failure;
        std::size_t bytes{};
    };

    for (const bool cloud : {false, true}) {
        auto provider = Create(cloud);
        auto request = Request("buffer-allocation");
        FailurePoint point;
        ModelOutcome outcome;
        std::size_t completed{};
        {
            Horo::Tests::AllocationProbe::ScopedObserver observe{[](const std::size_t bytes, void *raw) noexcept {
                auto &state = *static_cast<FailurePoint *>(raw);
                if (state.bytes == 0 && bytes >= 8192) {
                    state.bytes = bytes;
                    state.failure.emplace(0);
                }
            }, &point};
            outcome = provider->Stream(request, [&](const ModelEvent &event) {
                if (event.kind == ModelEventKind::Completed)
                    ++completed;
                return true;
            }, {});
        }
        REQUIRE(point.bytes >= 8192);
        REQUIRE_FALSE(outcome.Succeeded());
        REQUIRE(outcome.error);
        CHECK(outcome.error->code == ModelErrorCode::Protocol);
        CHECK(completed == 0);
        auto recovered = provider->Stream(Request("normal"), [](const ModelEvent &) {
            return true;
        }, {});
        CHECK(recovered.Succeeded());
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

TEST_CASE("Cloud credential lookup respects cancellation and sanitizes backend errors", "[agent][model][credentials]") {
    constexpr auto sensitive = "private lookup detail";
    unsigned lookups{};
    auto provider = CreateOpenAIModelProvider({Endpoint(), "opaque-ref", std::chrono::milliseconds{2000}},
                                              [&lookups](std::string_view) -> std::optional<std::string> {
        ++lookups;
        throw std::runtime_error("private lookup detail");
    });
    std::stop_source operation;
    bool delivered{};
    const auto sink = [&delivered](const ModelEvent &) {
        delivered = true;
        return true;
    };
    auto code = ModelErrorCode::CredentialUnavailable;
    SECTION("cancelled operations do not resolve a credential") {
        operation.request_stop();
        code = ModelErrorCode::Cancelled;
    }
    SECTION("lookup exceptions are represented by the same redacted error as revoked credentials") {}

    const auto discovery = provider->Discover(operation.get_token());
    const auto generation = provider->Stream(Request("normal"), sink, operation.get_token());
    REQUIRE(discovery.outcome.error);
    REQUIRE(generation.error);
    CHECK(discovery.outcome.error->code == code);
    CHECK(generation.error->code == code);
    CHECK(discovery.outcome.error->message.find(sensitive) == std::string::npos);
    CHECK(generation.error->message.find(sensitive) == std::string::npos);
    CHECK_FALSE(delivered);
    CHECK(lookups == (operation.stop_requested() ? 0U : 2U));
}

TEST_CASE("Typed cloud credential lookup requires HTTPS and an opaque reference", "[agent][model][credentials]") {
    const std::string reference = "cred:v1:" + std::string(48, 'a');
    auto credentials = std::make_shared<UnavailableCredentialProvider>();
    const auto sink = [](const ModelEvent &) {
        FAIL("A rejected credential cannot deliver model events");
        return false;
    };
    SECTION("plain HTTP is rejected before accessing the secure provider") {
        auto provider = CreateOpenAIModelProviderWithCredentials({Endpoint(), reference}, credentials);
        const auto discovery = provider->Discover({});
        const auto stream = provider->Stream(Request("normal"), sink, {});
        REQUIRE(discovery.outcome.error);
        REQUIRE(stream.error);
        CHECK(discovery.outcome.error->code == ModelErrorCode::InvalidRequest);
        CHECK(stream.error->code == ModelErrorCode::InvalidRequest);
        CHECK(credentials->references.empty());
    }
    SECTION("malformed references never reach the secure provider") {
        auto provider = CreateOpenAIModelProviderWithCredentials({"https://provider.invalid", "malformed"}, credentials);
        const auto discovery = provider->Discover({});
        REQUIRE(discovery.outcome.error);
        CHECK(discovery.outcome.error->code == ModelErrorCode::CredentialUnavailable);
        CHECK(credentials->references.empty());
    }
    SECTION("a typed rejection is redacted and resolved once for each operation") {
        auto provider = CreateOpenAIModelProviderWithCredentials({"https://provider.invalid", reference}, credentials);
        const auto discovery = provider->Discover({});
        const auto generation = provider->Stream(Request("normal"), sink, {});
        REQUIRE(discovery.outcome.error);
        REQUIRE(generation.error);
        CHECK(discovery.outcome.error->code == ModelErrorCode::CredentialUnavailable);
        CHECK(generation.error->code == ModelErrorCode::CredentialUnavailable);
        CHECK(discovery.outcome.error->message.find("private vault detail") == std::string::npos);
        CHECK(generation.error->message.find("private vault detail") == std::string::npos);
        CHECK(credentials->references == std::vector{reference, reference});
        REQUIRE(credentials->times.size() == 2);
        CHECK(credentials->times[1] >= credentials->times[0]);
    }
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
