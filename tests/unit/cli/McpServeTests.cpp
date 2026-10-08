#include "../mcp/McpAuthorizationTestSupport.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Mcp/McpErrors.h"
#include "McpServe.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <deque>
#include <thread>
using namespace Horo;
using namespace Horo::Application::Internal;

namespace {
    class Adapter final : public Mcp::IMcpToolAdapter {
    public:
        unsigned calls{};
        CancellationToken token;

        Result<nlohmann::json> Invoke(const nlohmann::json &, const Mcp::McpRequestContext &context) override {
            ++calls;
            token = context.cancellation;
            return Result<nlohmann::json>::Success(nlohmann::json::object());
        }
    };

    std::shared_ptr<Mcp::McpToolRegistry> Registry(std::shared_ptr<Adapter> adapter = {}) {
        auto registry = std::make_shared<Mcp::McpToolRegistry>(Mcp::Test::Authorization());
        std::vector<Mcp::McpToolRegistration> tools;
        if (adapter)
            tools.push_back({.descriptor = {.id = {"host.test"},
                                            .description = "Test injected application operation",
                                            .inputSchema = {{"type", "object"}},
                                            .outputSchema = {{"type", "object"}}},
                             .adapter = std::move(adapter),
                             .owner = Mcp::McpOwnerContext::Runtime});
        REQUIRE(registry->Publish(std::move(tools)).HasValue());
        return registry;
    }

    Result<void> ServeApprovedMcp(std::shared_ptr<Mcp::McpToolRegistry> registry, const McpServeChannel &channel,
                                  const std::span<const std::string> capabilities = {}) {
        auto policy = Mcp::Test::Authorization();
        Mcp::McpSessionAdmission admission{.clientIdentity = "horo.cli.test",
                                           .capabilities = {capabilities.begin(), capabilities.end()},
                                           .registryRevision = registry ? registry->Read()->Generation() : 1};
        if (admission.registryRevision != 0)
            admission = Mcp::Test::Authenticate(std::move(admission), policy);
        return ServeMcp(std::move(registry), channel, {std::move(policy), std::move(admission)});
    }

    struct Channel final {
        std::deque<McpChannelRead> reads;
        std::string output;
        bool stop{};

        McpServeChannel View() {
            return {.read = [&] {
                if (reads.empty())
                    return Result<McpChannelRead>::Success({.disconnected = true});
                auto value = std::move(reads.front());
                reads.pop_front();
                return Result<McpChannelRead>::Success(std::move(value));
            }, .write = [&](std::string_view bytes) {
                output += bytes;
                return Result<void>::Success();
            }, .stopped = [&] {
                return stop;
            }};
        }
    };
}  // namespace

TEST_CASE("Headless MCP composes real controller discovery and application owner dispatch", "[cli][mcp]") {
    auto adapter = std::make_shared<Adapter>();
    Channel channel{.reads = {{R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"
                               "\n"},
                              {R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"host.test","arguments":{}}})"
                               "\n"}}};
    const auto result = ServeApprovedMcp(Registry(adapter), channel.View());
    REQUIRE(result.HasValue());
    REQUIRE(adapter->calls == 1);
    const auto split = channel.output.find('\n');
    REQUIRE(nlohmann::json::parse(channel.output.substr(0, split))["result"]["tools"][0]["name"] == "host.test");
    REQUIRE(nlohmann::json::parse(channel.output.substr(split + 1))["result"]["status"] == "queued");
}

TEST_CASE("Headless MCP discards partial frames and stops without presentation records", "[cli][mcp]") {
    Channel channel;
    channel.reads.push_back({.bytes = "{partial"});
    REQUIRE(ServeApprovedMcp(Registry(), channel.View()).HasValue());
    REQUIRE(channel.output.empty());
    channel.stop = true;
    const auto cancelled = ServeApprovedMcp(Registry(), channel.View());
    REQUIRE(cancelled.HasError());
    REQUIRE(cancelled.ErrorValue().code.Value() == Cli::CliErrors::ExecutionCancelled.code.Value());
}

TEST_CASE("Headless MCP reports channel and startup failures without fallback", "[cli][mcp]") {
    Channel channel;
    REQUIRE(ServeApprovedMcp(nullptr, channel.View()).HasError());
    auto view = channel.View();
    view.read = [] {
        return Result<McpChannelRead>::Failure(MakeError(Cli::CliErrors::HostFailure));
    };
    REQUIRE(ServeApprovedMcp(Registry(), view).HasError());
    channel.reads = {{"not-json\n"}};
    view = channel.View();
    view.write = [](std::string_view) {
        return Result<void>::Failure(MakeError(Cli::CliErrors::ExecutionTimedOut));
    };
    const auto failure = ServeApprovedMcp(Registry(), view);
    REQUIRE(failure.HasError());
    REQUIRE(failure.ErrorValue().code.Value() == Cli::CliErrors::ExecutionTimedOut.code.Value());
}

TEST_CASE("Headless MCP disconnect cancels and drains the application callback exactly once", "[cli][mcp]") {
    class AsyncAdapter final : public Mcp::IMcpToolAdapter {
    public:
        std::atomic<unsigned> completions{};
        std::atomic<bool> observedCancellation{};
        std::jthread worker;

        Result<nlohmann::json> Invoke(const nlohmann::json &, const Mcp::McpRequestContext &) override {
            return Result<nlohmann::json>::Failure(MakeError(Mcp::McpErrors::ControllerFailed));
        }

        void InvokeAsync(const nlohmann::json &, const Mcp::McpRequestContext &context,
                         const std::function<void(Result<nlohmann::json>)> &complete) override {
            worker = std::jthread([this, context, complete] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
                while (!context.IsStopRequested() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                observedCancellation.store(context.IsStopRequested());
                ++completions;
                complete(Result<nlohmann::json>::Failure(MakeError(Mcp::McpErrors::RequestCancelled)));
            });
        }
    };

    auto adapter = std::make_shared<AsyncAdapter>();
    auto registry = std::make_shared<Mcp::McpToolRegistry>(Mcp::Test::Authorization());
    REQUIRE(registry
                ->Publish({{.descriptor = {.id = {"host.test"},
                                           .description = "Owned async application operation",
                                           .inputSchema = {{"type", "object"}},
                                           .outputSchema = {{"type", "object"}}},
                            .adapter = adapter,
                            .owner = Mcp::McpOwnerContext::Runtime}})
                .HasValue());
    Channel channel;
    channel.reads.push_back({.bytes = R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"host.test","arguments":{}}})"
                                      "\n"});
    REQUIRE(ServeApprovedMcp(registry, channel.View()).HasValue());
    REQUIRE(adapter->completions.load() == 1);
    REQUIRE(adapter->observedCancellation.load());
}

TEST_CASE("Headless MCP optional capability denial executes no application adapter", "[cli][mcp]") {
    auto adapter = std::make_shared<Adapter>();
    auto registry = std::make_shared<Mcp::McpToolRegistry>(Mcp::Test::Authorization());
    const std::vector<std::string> available{"horo.renderer.inspect"};
    REQUIRE(registry
                ->Publish({{.descriptor = {.id = {"renderer.inspect"},
                                           .description = "Optional renderer operation",
                                           .inputSchema = {{"type", "object"}},
                                           .outputSchema = {{"type", "object"}},
                                           .requiredCapabilities = available},
                            .adapter = adapter,
                            .owner = Mcp::McpOwnerContext::Runtime}},
                          available)
                .HasValue());
    Channel channel;
    channel.reads.push_back({.bytes =
                                 R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"renderer.inspect","arguments":{}}})"
                                 "\n"});
    REQUIRE(ServeApprovedMcp(registry, channel.View()).HasValue());
    REQUIRE(adapter->calls == 0);
    const auto reply = nlohmann::json::parse(channel.output);
    REQUIRE(reply["error"]["data"]["code"] == Mcp::McpErrors::ToolCapabilityUnavailable.code.Value());
}

TEST_CASE("Headless MCP session admission fails before input and releases partial startup", "[cli][mcp]") {
    Channel channel;
    unsigned reads{};
    auto view = channel.View();
    view.read = [&] {
        ++reads;
        return Result<McpChannelRead>::Success({.disconnected = true});
    };
    const auto failed = ServeApprovedMcp(std::make_shared<Mcp::McpToolRegistry>(Mcp::Test::Authorization()), view);
    REQUIRE(failed.HasError());
    REQUIRE(failed.ErrorValue().code.Value() == Mcp::McpErrors::AdmissionInvalid.code.Value());
    REQUIRE(reads == 0);
    REQUIRE(channel.output.empty());
}

TEST_CASE("Headless MCP rejects missing host policy before polling input", "[cli][mcp]") {
    Channel channel;
    unsigned reads{};
    auto view = channel.View();
    view.read = [&] {
        ++reads;
        return Result<McpChannelRead>::Success({.disconnected = true});
    };
    const auto failed = ServeMcp(Registry(), view, {});
    REQUIRE(failed.HasError());
    REQUIRE(failed.ErrorValue().code.Value() == Mcp::McpErrors::ConfigurationInvalid.code.Value());
    REQUIRE(reads == 0);
    REQUIRE(channel.output.empty());
}
