#include "Horo/Mcp/McpErrors.h"
#include "Horo/Mcp/McpToolRegistry.h"
#include "McpAuthorizationTestSupport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Horo;
using namespace Horo::Mcp;

namespace {
    class CountingAdapter final : public IMcpToolAdapter {
    public:
        explicit CountingAdapter(nlohmann::json result = {{"accepted", true}}) : result_(std::move(result)) {}

        Result<nlohmann::json> Invoke(const nlohmann::json &, const McpRequestContext &) override {
            ++calls;
            return Result<nlohmann::json>::Success(result_);
        }

        std::atomic<unsigned> calls{};

    private:
        nlohmann::json result_;
    };

    class ThrowingAdapter final : public IMcpToolAdapter {
    public:
        Result<nlohmann::json> Invoke(const nlohmann::json &, const McpRequestContext &) override {
            throw std::runtime_error{"private adapter failure"};
        }
    };

    class ThrowingNonStandardAdapter final : public IMcpToolAdapter {
    public:
        Result<nlohmann::json> Invoke(const nlohmann::json &, const McpRequestContext &) override {
            throw 42;
        }
    };

    [[nodiscard]] McpToolRegistration Tool(std::string id, const std::shared_ptr<IMcpToolAdapter> &adapter,
                                           std::vector<std::string> capabilities = {}) {
        return {.descriptor = {.id = {std::move(id)},
                               .version = {1, 0, 0},
                               .description = "A bounded test tool",
                               .inputSchema = {{"type", "object"},
                                               {"properties", {{"count", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4}}}}},
                                               {"required", {"count"}},
                                               {"additionalProperties", false}},
                               .outputSchema = {{"type", "object"},
                                                {"properties", {{"accepted", {{"type", "boolean"}}}}},
                                                {"required", {"accepted"}},
                                                {"additionalProperties", false}},
                               .effect = McpToolEffect::Query,
                               .requiredCapabilities = std::move(capabilities)},
                .adapter = adapter,
                .owner = McpOwnerContext::Editor};
    }

    [[nodiscard]] McpRequestContext Context(std::vector<std::string> capabilities = {}) {
        return Test::Context({.clientIdentity = "registry-client", .capabilities = std::move(capabilities)});
    }

    [[nodiscard]] std::string Code(const Error &error) {
        return error.code.Value();
    }
}  // namespace

TEST_CASE("MCP registry publishes deterministic capability-filtered discovery", "[unit][mcp][registry]") {
    McpToolRegistry registry{Test::Authorization()};
    const auto adapter = std::make_shared<CountingAdapter>();
    REQUIRE(registry.Read()->Generation() == 0);
    const std::vector<std::string> hostCapabilities{"project.read"};
    REQUIRE(registry.Publish({Tool("zeta.query", adapter, {"project.read"}), Tool("alpha.query", adapter)}, hostCapabilities).HasValue());
    const auto snapshot = registry.Read();
    REQUIRE(snapshot->Generation() == 1);
    REQUIRE(snapshot->Discover({}).size() == 1);
    CHECK(snapshot->Discover({})[0].id.value == "alpha.query");
    const std::vector<std::string> grants{"project.read"};
    const auto visible = snapshot->Discover(grants);
    REQUIRE(visible.size() == 2);
    CHECK(visible[0].id.value == "alpha.query");
    CHECK(visible[1].id.value == "zeta.query");
    CHECK(visible[1].version.major == 1);
    CHECK(visible[1].requiredCapabilities == grants);
}

TEST_CASE("MCP registry requires an explicit owner context", "[unit][mcp][registry]") {
    McpToolRegistry registry{Test::Authorization()};
    auto registration = Tool("build.start", std::make_shared<CountingAdapter>());
    registration.owner = McpOwnerContext::Unspecified;
    const auto result = registry.Publish({std::move(registration)});
    REQUIRE(result.HasError());
    CHECK(Code(result.ErrorValue()) == McpErrors::ToolDescriptorInvalid.code.Value());
    CHECK(registry.Read()->Generation() == 0);
}

TEST_CASE("MCP schema and capability admission precedes every application adapter call", "[unit][mcp][registry]") {
    McpToolRegistry registry{Test::Authorization()};
    const auto adapter = std::make_shared<CountingAdapter>();
    const std::vector<std::string> hostCapabilities{"project.read"};
    REQUIRE(registry.Publish({Tool("project.query", adapter, {"project.read"})}, hostCapabilities).HasValue());
    const auto snapshot = registry.Read();
    const McpRequestContext denied = Context();
    auto outcome = snapshot->Invoke({"project.query"}, {{"count", 1}}, denied);
    REQUIRE(outcome.HasError());
    CHECK(Code(outcome.ErrorValue()) == McpErrors::ToolCapabilityUnavailable.code.Value());
    CHECK(adapter->calls == 0);

    const McpRequestContext admitted = Context({"project.read"});
    for (const nlohmann::json invalid : {nlohmann::json::object(), nlohmann::json{{"count", -1}}, nlohmann::json{{"count", 5}},
                                         nlohmann::json{{"count", "1"}}, nlohmann::json{{"count", 1}, {"extra", true}}}) {
        outcome = snapshot->Invoke({"project.query"}, invalid, admitted);
        REQUIRE(outcome.HasError());
        CHECK(Code(outcome.ErrorValue()) == McpErrors::ToolInputInvalid.code.Value());
        CHECK(adapter->calls == 0);
    }
    outcome = snapshot->Invoke({"project.query"}, {{"count", 4}}, admitted);
    REQUIRE(outcome.HasValue());
    CHECK(outcome.Value() == nlohmann::json{{"accepted", true}});
    CHECK(adapter->calls == 1);
}

TEST_CASE("MCP registry rejects malformed, duplicate, unavailable and incompatible candidates atomically", "[unit][mcp][registry]") {
    McpToolRegistry registry{Test::Authorization()};
    const auto adapter = std::make_shared<CountingAdapter>();
    REQUIRE(registry.Publish({Tool("alpha.query", adapter)}).HasValue());
    const auto original = registry.Read();

    auto malformed = Tool("beta.query", adapter);
    malformed.descriptor.inputSchema["$ref"] = "https://untrusted.example/schema";
    auto result = registry.Publish({Tool("alpha.query", adapter), malformed});
    REQUIRE(result.HasError());
    CHECK(Code(result.ErrorValue()) == McpErrors::ToolDescriptorInvalid.code.Value());
    CHECK(registry.Read() == original);

    result = registry.Publish({Tool("alpha.query", adapter), Tool("restricted.query", adapter, {"project.write"})});
    REQUIRE(result.HasError());
    CHECK(Code(result.ErrorValue()) == McpErrors::ToolCapabilityUnavailable.code.Value());
    CHECK(registry.Read() == original);

    result = registry.Publish({Tool("same.query", adapter), Tool("same.query", adapter)});
    REQUIRE(result.HasError());
    CHECK(Code(result.ErrorValue()) == McpErrors::ToolDuplicate.code.Value());
    CHECK(registry.Read() == original);

    auto missing = Tool("missing.query", adapter);
    missing.adapter.reset();
    result = registry.Publish({std::move(missing)});
    REQUIRE(result.HasError());
    CHECK(Code(result.ErrorValue()) == McpErrors::ToolDescriptorInvalid.code.Value());
    CHECK(registry.Read() == original);

    auto incompatible = Tool("alpha.query", adapter);
    incompatible.descriptor.version.major = 2;
    result = registry.Publish({std::move(incompatible)});
    REQUIRE(result.HasError());
    CHECK(Code(result.ErrorValue()) == McpErrors::ToolIncompatible.code.Value());
    CHECK(registry.Read() == original);

    auto changedSchema = Tool("alpha.query", adapter);
    changedSchema.descriptor.inputSchema["required"] = nlohmann::json::array();
    result = registry.Publish({std::move(changedSchema)});
    REQUIRE(result.HasError());
    CHECK(Code(result.ErrorValue()) == McpErrors::ToolIncompatible.code.Value());
    CHECK(registry.Read() == original);
}

TEST_CASE("MCP registry retains old reader and adapter lifetimes across replacement", "[unit][mcp][registry]") {
    McpToolRegistry registry{Test::Authorization()};
    auto first = std::make_shared<CountingAdapter>();
    REQUIRE(registry.Publish({Tool("alpha.query", first)}).HasValue());
    const auto old = registry.Read();
    std::weak_ptr<CountingAdapter> firstLease = first;
    first.reset();
    const auto second = std::make_shared<CountingAdapter>();
    auto replacement = Tool("alpha.query", second);
    replacement.descriptor.version.minor = 1;
    REQUIRE(registry.Publish({std::move(replacement)}).HasValue());
    REQUIRE(registry.Read()->Generation() == 2);
    REQUIRE(old->Generation() == 1);
    REQUIRE_FALSE(firstLease.expired());
    REQUIRE(old->Invoke({"alpha.query"}, {{"count", 0}}, Context()).HasValue());
    CHECK(firstLease.lock()->calls == 1);
    REQUIRE(registry.Read()->Invoke({"alpha.query"}, {{"count", 0}}, Context()).HasValue());
    CHECK(second->calls == 1);
}

TEST_CASE("MCP registry treats required capabilities as a canonical set across replacement", "[unit][mcp][registry]") {
    McpToolRegistry registry{Test::Authorization()};
    const auto adapter = std::make_shared<CountingAdapter>();
    const std::vector<std::string> hostCapabilities{"project.write", "project.read"};
    REQUIRE(registry.Publish({Tool("project.query", adapter, {"project.write", "project.read"})}, hostCapabilities).HasValue());
    const auto old = registry.Read();
    CHECK(old->Discover(hostCapabilities)[0].requiredCapabilities == std::vector<std::string>{"project.read", "project.write"});

    auto replacement = Tool("project.query", adapter, {"project.read", "project.write"});
    replacement.descriptor.version.minor = 1;
    REQUIRE(registry.Publish({std::move(replacement)}, hostCapabilities).HasValue());
    CHECK(registry.Read()->Generation() == 2);
    CHECK(old->Generation() == 1);
    CHECK(registry.Read()->Discover(hostCapabilities)[0].requiredCapabilities == std::vector<std::string>{"project.read", "project.write"});
}

TEST_CASE("MCP registry enforces input and output budgets without leaking malformed results", "[unit][mcp][registry]") {
    McpToolRegistry registry{Test::Authorization()};
    const auto adapter = std::make_shared<CountingAdapter>(nlohmann::json{{"accepted", "wrong"}});
    auto entry = Tool("bounded.query", adapter);
    entry.descriptor.bounds.maximumInputBytes = 32;
    entry.descriptor.bounds.maximumResultBytes = 32;
    REQUIRE(registry.Publish({std::move(entry)}).HasValue());
    const auto snapshot = registry.Read();
    auto outcome = snapshot->Invoke({"bounded.query"}, {{"count", 1}, {"padding", std::string(64, 'x')}}, Context());
    REQUIRE(outcome.HasError());
    CHECK(adapter->calls == 0);
    outcome = snapshot->Invoke({"bounded.query"}, {{"count", 1}}, Context());
    REQUIRE(outcome.HasError());
    CHECK(Code(outcome.ErrorValue()) == McpErrors::ToolOutputInvalid.code.Value());
    CHECK(adapter->calls == 1);
}

TEST_CASE("MCP registry rejects malformed metadata and unsupported schema constructs", "[unit][mcp][registry]") {
    McpToolRegistry registry{Test::Authorization()};
    const auto adapter = std::make_shared<CountingAdapter>();
    auto entry = Tool("valid.query", adapter);
    const auto reject = [&registry](McpToolRegistration invalid) {
        auto result = registry.Publish({std::move(invalid)});
        REQUIRE(result.HasError());
        CHECK(Code(result.ErrorValue()) == McpErrors::ToolDescriptorInvalid.code.Value());
        CHECK(registry.Read()->Generation() == 0);
    };

    auto invalid = entry;
    invalid.descriptor.id.value = "Provider/Native";
    reject(std::move(invalid));
    invalid = entry;
    invalid.descriptor.version.major = 0;
    reject(std::move(invalid));
    invalid = entry;
    invalid.descriptor.effect = static_cast<McpToolEffect>(255);
    reject(std::move(invalid));
    invalid = entry;
    invalid.descriptor.requiredCapabilities = {"project.read", "project.read"};
    reject(std::move(invalid));
    invalid = entry;
    invalid.descriptor.inputSchema["required"] = {"count", "count"};
    reject(std::move(invalid));
    invalid = entry;
    invalid.descriptor.inputSchema["properties"]["count"]["minimum"] = 10;
    reject(std::move(invalid));
    invalid = entry;
    invalid.descriptor.inputSchema["$ref"] = "https://example.invalid/schema";
    reject(std::move(invalid));
    invalid = entry;
    invalid.descriptor.bounds.maximumDepth = 0;
    reject(std::move(invalid));
    invalid = entry;
    invalid.descriptor.inputSchema["properties"]["count"]["minimum"] = std::numeric_limits<double>::quiet_NaN();
    reject(std::move(invalid));
    invalid = entry;
    invalid.descriptor.inputSchema["properties"]["count"]["enum"] = {"one"};
    reject(std::move(invalid));
    invalid = entry;
    invalid.descriptor.inputSchema["properties"]["count"]["enum"] = {5};
    reject(std::move(invalid));
    invalid = entry;
    invalid.descriptor.inputSchema["properties"]["count"]["enum"] = {1, 1};
    reject(std::move(invalid));
    invalid = entry;
    invalid.descriptor.inputSchema["properties"][std::string(1, static_cast<char>(0xff))] = {{"type", "string"}};
    reject(std::move(invalid));
}

TEST_CASE("MCP nested schema validates arrays enums string limits and finite numeric inputs", "[unit][mcp][registry]") {
    McpToolRegistry registry{Test::Authorization()};
    const auto adapter = std::make_shared<CountingAdapter>();
    auto entry = Tool("nested.query", adapter);
    entry.descriptor.inputSchema = {{"type", "object"},
                                    {"properties",
                                     {{"items",
                                       {{"type", "array"},
                                        {"minItems", 1},
                                        {"maxItems", 2},
                                        {"items", {{"type", "string"}, {"minLength", 2}, {"maxLength", 4}, {"enum", {"ok", "yes"}}}}}}}},
                                    {"required", {"items"}},
                                    {"additionalProperties", false}};
    REQUIRE(registry.Publish({std::move(entry)}).HasValue());
    const auto snapshot = registry.Read();
    for (const nlohmann::json invalid :
         {nlohmann::json{{"items", nlohmann::json::array()}}, nlohmann::json{{"items", {"ok", "yes", "ok"}}},
          nlohmann::json{{"items", {"x"}}}, nlohmann::json{{"items", {"oops"}}}, nlohmann::json{{"items", {1}}}}) {
        const auto result = snapshot->Invoke({"nested.query"}, invalid, Context());
        REQUIRE(result.HasError());
        CHECK(adapter->calls == 0);
    }
    REQUIRE(snapshot->Invoke({"nested.query"}, {{"items", {"ok", "yes"}}}, Context()).HasValue());
    CHECK(adapter->calls == 1);
}

TEST_CASE("MCP schema matching preserves primitive types bounds and open object members", "[unit][mcp][registry]") {
    McpToolRegistry registry{Test::Authorization()};
    const auto adapter = std::make_shared<CountingAdapter>();
    auto entry = Tool("primitive.query", adapter);
    entry.descriptor.inputSchema = {{"type", "object"},
                                    {"properties",
                                     {{"flag", {{"type", "boolean"}}},
                                      {"ratio", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                                      {"nothing", {{"type", "null"}}},
                                      {"items", {{"type", "array"}, {"minItems", 1}, {"maxItems", 2}, {"items", {{"type", "integer"}}}}}}},
                                    {"required", {"flag", "ratio", "nothing", "items"}},
                                    {"additionalProperties", true}};
    REQUIRE(registry.Publish({std::move(entry)}).HasValue());
    const auto snapshot = registry.Read();
    const nlohmann::json valid{{"flag", true}, {"ratio", 0.5}, {"nothing", nullptr}, {"items", {1, 2}}, {"extra", "allowed"}};
    REQUIRE(snapshot->Invoke({"primitive.query"}, valid, Context()).HasValue());
    CHECK(adapter->calls == 1);

    for (const auto &[name, replacement] : std::vector<std::pair<std::string, nlohmann::json>>{{"flag", 1},
                                                                                               {"ratio", 2},
                                                                                               {"nothing", false},
                                                                                               {"items", nlohmann::json::array()}}) {
        auto invalid = valid;
        invalid[name] = replacement;
        const auto result = snapshot->Invoke({"primitive.query"}, invalid, Context());
        REQUIRE(result.HasError());
        CHECK(Code(result.ErrorValue()) == McpErrors::ToolInputInvalid.code.Value());
        CHECK(adapter->calls == 1);
    }
}

TEST_CASE("MCP registry rejects malformed UTF-8 arguments before application invocation", "[unit][mcp][registry]") {
    McpToolRegistry registry{Test::Authorization()};
    const auto adapter = std::make_shared<CountingAdapter>();
    auto entry = Tool("text.query", adapter);
    entry.descriptor.inputSchema = {{"type", "string"}};
    REQUIRE(registry.Publish({std::move(entry)}).HasValue());
    const auto result = registry.Read()->Invoke({"text.query"}, std::string(1, static_cast<char>(0xff)), Context());
    REQUIRE(result.HasError());
    CHECK(Code(result.ErrorValue()) == McpErrors::ToolInputInvalid.code.Value());
    CHECK(adapter->calls == 0);
}

TEST_CASE("MCP snapshots retain adapters after registry teardown and translate adapter exceptions", "[unit][mcp][registry]") {
    std::shared_ptr<const McpToolSnapshot> retained;
    const auto adapter = std::make_shared<CountingAdapter>();
    {
        McpToolRegistry registry{Test::Authorization()};
        REQUIRE(registry.Publish({Tool("retained.query", adapter)}).HasValue());
        retained = registry.Read();
    }
    REQUIRE(retained->Invoke({"retained.query"}, {{"count", 1}}, Context()).HasValue());
    CHECK(adapter->calls == 1);

    McpToolRegistry registry{Test::Authorization()};
    REQUIRE(registry.Publish({Tool("throws.query", std::make_shared<ThrowingAdapter>())}).HasValue());
    const auto result = registry.Read()->Invoke({"throws.query"}, {{"count", 1}}, Context());
    REQUIRE(result.HasError());
    CHECK(Code(result.ErrorValue()) == McpErrors::ControllerFailed.code.Value());

    REQUIRE(registry.Publish({Tool("throws.nonstandard", std::make_shared<ThrowingNonStandardAdapter>())}).HasValue());
    const auto nonStandard = registry.Read()->Invoke({"throws.nonstandard"}, {{"count", 1}}, Context());
    REQUIRE(nonStandard.HasError());
    CHECK(Code(nonStandard.ErrorValue()) == McpErrors::ControllerFailed.code.Value());
}

TEST_CASE("MCP candidate publication is all-or-nothing across concurrent snapshot readers", "[unit][mcp][registry]") {
    McpToolRegistry registry{Test::Authorization()};
    const auto adapter = std::make_shared<CountingAdapter>();
    REQUIRE(registry.Publish({Tool("first.query", adapter)}).HasValue());
    std::atomic<bool> invalidObservation{};
    std::thread reader([&] {
        for (int index = 0; index < 1000; ++index) {
            const auto snapshot = registry.Read();
            const auto descriptors = snapshot->Discover({});
            if (!((snapshot->Generation() == 1 && descriptors.size() == 1 && descriptors[0].id.value == "first.query") ||
                  (snapshot->Generation() == 2 && descriptors.size() == 2 && descriptors[0].id.value == "first.query" &&
                   descriptors[1].id.value == "second.query")))
                invalidObservation = true;
        }
    });
    const auto result = registry.Publish({Tool("second.query", adapter), Tool("first.query", adapter)});
    reader.join();
    REQUIRE(result.HasValue());
    CHECK_FALSE(invalidObservation);
}
