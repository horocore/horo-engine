#include "Horo/Mcp/McpErrors.h"
#include "Horo/Mcp/McpLocalTransport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace Horo::Mcp {
    namespace {
        class EchoController final : public IMcpRequestController {
        public:
            Result<nlohmann::json> Dispatch(const McpRequest &request, const McpRequestContext &context) override {
                return Result<nlohmann::json>::Success(
                    {{"method", request.method}, {"project", context.projectIdentity.value_or("")}, {"params", request.params}});
            }
        };

        class BlockingController final : public IMcpRequestController {
        public:
            Result<nlohmann::json> Dispatch(const McpRequest &, const McpRequestContext &context) override {
                std::unique_lock lock{mutex_};
                entered_ = true;
                condition_.notify_all();
                condition_.wait(lock, [this] {
                    return released_;
                });
                sawCancellation_ = context.cancellation.IsCancellationRequested();
                return Result<nlohmann::json>::Success(nlohmann::json::object());
            }

            void WaitUntilEntered() {
                std::unique_lock lock{mutex_};
                condition_.wait(lock, [this] {
                    return entered_;
                });
            }

            void Release() {
                std::lock_guard lock{mutex_};
                released_ = true;
                condition_.notify_all();
            }

            [[nodiscard]] bool SawCancellation() const noexcept {
                return sawCancellation_.load();
            }

        private:
            std::mutex mutex_;
            std::condition_variable condition_;
            std::atomic<bool> sawCancellation_{false};
            bool entered_{};
            bool released_{};
        };

        McpSessionAdmission Admission() {
            return {.clientIdentity = "stdio", .capabilities = {"read"}, .projectIdentity = "first"};
        }

        std::shared_ptr<McpLocalTransport> Transport(McpSessionLimits limits = {}) {
            auto manager = McpSessionManager::Create(std::make_shared<EchoController>(), limits);
            REQUIRE(manager.HasValue());
            auto started = McpLocalTransport::Start(manager.Value(), Admission());
            REQUIRE(started.HasValue());
            return std::move(started).Value();
        }

        nlohmann::json OneReply(const std::shared_ptr<McpLocalTransport> &transport, const std::string_view input) {
            auto result = transport->Feed(input);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().size() == 1);
            REQUIRE(result.Value()[0].back() == '\n');
            return nlohmann::json::parse(result.Value()[0]);
        }
    }  // namespace

    TEST_CASE("local MCP framing assembles chunks, CRLF and multiple requests", "[mcp][framing]") {
        auto transport = Transport();
        REQUIRE(transport->Feed(R"({"jsonrpc":"2.0","id":1,"method":)").Value().empty());
        auto first = OneReply(transport, "\"tools/list\"}\r\n");
        REQUIRE(first["result"]["method"] == "tools/list");
        auto batch = transport->Feed("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}\n"
                                     "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}\n");
        REQUIRE(batch.HasValue());
        REQUIRE(batch.Value().size() == 2);
        REQUIRE(nlohmann::json::parse(batch.Value()[0])["id"] == 2);
        REQUIRE(nlohmann::json::parse(batch.Value()[1])["id"] == 3);
    }

    TEST_CASE("local MCP framing rejects malformed, excessive and deep frames without losing sync", "[mcp][framing]") {
        McpSessionLimits limits;
        limits.maximumFrameBytes = 512;
        limits.maximumInputBytes = 256;
        limits.maximumDepth = 5;
        limits.maximumStringBytes = 128;
        auto transport = Transport(limits);

        REQUIRE(OneReply(transport, "{bad}\n")["error"]["code"] == -32700);
        REQUIRE(OneReply(transport, "[]\n")["error"]["code"] == -32600);
        REQUIRE(OneReply(transport, "{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"ping\"}\n")["error"]["code"] == -32600);
        const std::string excessive =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{\"x\":\"" + std::string(400, 'a') + "\"}}\n";
        REQUIRE(OneReply(transport, excessive)["error"]["data"]["code"] == McpErrors::InputCapacityExceeded.code.Value());
        const std::string deeplyNested = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":[[[[[[1]]]]]]}\n";
        REQUIRE(OneReply(transport, deeplyNested)["error"]["code"] == -32600);
        REQUIRE(OneReply(transport, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"ping\"}\n")["result"]["method"] == "ping");
    }

    TEST_CASE("local MCP discard mode bounds an oversized partial frame and resumes at newline", "[mcp][framing]") {
        McpSessionLimits limits;
        limits.maximumFrameBytes = 256;
        limits.maximumInputBytes = 256;
        limits.maximumStringBytes = 64;
        auto transport = Transport(limits);
        REQUIRE(transport->Feed(std::string(256, 'x')).Value().empty());
        REQUIRE(transport->Feed(std::string(256, 'y')).Value().empty());
        REQUIRE(OneReply(transport, "z\n")["error"]["code"] == -32600);
        REQUIRE(OneReply(transport, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n")["id"] == 1);
    }

    TEST_CASE("local MCP notifications have no response and project switches discard stale input", "[mcp][framing]") {
        auto transport = Transport();
        auto notification = transport->Feed("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
        REQUIRE(notification.HasValue());
        REQUIRE(notification.Value().empty());
        REQUIRE(transport->Feed("{\"jsonrpc\":\"2.0\",").Value().empty());
        const auto previous = transport->Session();
        auto switched = transport->SwitchProject("second");
        REQUIRE(switched.HasValue());
        REQUIRE(switched.Value().generation > previous.generation);
        REQUIRE(OneReply(transport, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n")["result"]["project"] == "second");
        transport->Disconnect();
        REQUIRE(transport->Feed("x").HasError());
    }

    TEST_CASE("local MCP cancellation notification and disconnect revoke an active request", "[mcp][framing]") {
        auto controller = std::make_shared<BlockingController>();
        auto manager = McpSessionManager::Create(controller);
        REQUIRE(manager.HasValue());
        auto started = McpLocalTransport::Start(manager.Value(), Admission());
        REQUIRE(started.HasValue());
        auto transport = std::move(started).Value();
        std::atomic<bool> replySuppressed{false};
        std::thread request([&] {
            const auto output = transport->Feed("{\"jsonrpc\":\"2.0\",\"id\":\"active\",\"method\":\"tools/call\"}\n");
            replySuppressed = output.HasValue() && output.Value().empty();
        });
        controller->WaitUntilEntered();
        const auto notification =
            transport->Feed("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":\"active\"}}\n");
        transport->Disconnect();
        controller->Release();
        request.join();
        REQUIRE(notification.HasValue());
        REQUIRE(notification.Value().empty());
        REQUIRE(controller->SawCancellation());
        REQUIRE(replySuppressed.load());
        REQUIRE(manager.Value()->ActiveSessions() == 0);
    }

    TEST_CASE("local MCP reports an oversized controller result without serializing it", "[mcp][framing]") {
        McpSessionLimits limits;
        limits.maximumResultBytes = 64;
        auto transport = Transport(limits);
        const std::string request =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{\"large\":\"" + std::string(100, 'x') + "\"}}\n";
        const auto reply = OneReply(transport, request);
        REQUIRE(reply["error"]["data"]["code"] == McpErrors::ResultCapacityExceeded.code.Value());
    }
}  // namespace Horo::Mcp
