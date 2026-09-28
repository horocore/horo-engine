#include "Horo/Mcp/McpController.h"
#include "Horo/Mcp/McpErrors.h"
#include "Horo/Mcp/McpInProcessAdapter.h"
#include "Horo/Mcp/McpLocalTransport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

using namespace Horo;
using namespace Horo::Mcp;

namespace {
    class Adapter final : public IMcpToolAdapter {
    public:
        std::atomic<unsigned> calls{};
        std::thread::id invokedOn{};
        std::function<Result<nlohmann::json>(const McpRequestContext &)> action;

        Result<nlohmann::json> Invoke(const nlohmann::json &, const McpRequestContext &context) override {
            invokedOn = std::this_thread::get_id();
            ++calls;
            if (action)
                return action(context);
            return Result<nlohmann::json>::Success({{"ok", true}});
        }
    };

    class DeferredAdapter final : public IMcpToolAdapter {
    public:
        Result<nlohmann::json> Invoke(const nlohmann::json &, const McpRequestContext &) override {
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::ControllerFailed));
        }

        void InvokeAsync(const nlohmann::json &, const McpRequestContext &context,
                         const std::function<void(Result<nlohmann::json>)> &complete) override {
            savedContext = context;
            completion = complete;
        }

        McpRequestContext savedContext;
        std::function<void(Result<nlohmann::json>)> completion;
    };

    class Gate final {
    public:
        void Enter() {
            std::unique_lock lock{mutex_};
            entered_ = true;
            condition_.notify_all();
            condition_.wait(lock, [this] {
                return released_;
            });
        }

        void Wait() {
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

    private:
        std::mutex mutex_;
        std::condition_variable condition_;
        bool entered_{};
        bool released_{};
    };

    class CompleteBeforeReturnAdapter final : public IMcpToolAdapter {
    public:
        explicit CompleteBeforeReturnAdapter(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}

        Result<nlohmann::json> Invoke(const nlohmann::json &, const McpRequestContext &) override {
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::ControllerFailed));
        }

        void InvokeAsync(const nlohmann::json &, const McpRequestContext &,
                         const std::function<void(Result<nlohmann::json>)> &complete) override {
            complete(Result<nlohmann::json>::Success({{"ok", true}}));
            gate_->Enter();
        }

    private:
        std::shared_ptr<Gate> gate_;
    };

    McpToolRegistration Tool(const std::shared_ptr<IMcpToolAdapter> &adapter, McpOwnerContext owner = McpOwnerContext::Editor) {
        return {.descriptor = {.id = {"test.call"},
                               .version = {1, 0, 0},
                               .description = "Owner dispatch test",
                               .inputSchema = {{"type", "object"}, {"additionalProperties", false}},
                               .outputSchema = {{"type", "object"},
                                                {"properties", {{"ok", {{"type", "boolean"}}}}},
                                                {"required", {"ok"}},
                                                {"additionalProperties", false}},
                               .effect = McpToolEffect::Mutation},
                .adapter = adapter,
                .owner = owner};
    }

    std::shared_ptr<McpController> Controller(const std::shared_ptr<IMcpToolAdapter> &adapter, McpControllerLimits limits = {},
                                              McpOwnerContext owner = McpOwnerContext::Editor) {
        auto registry = std::make_shared<McpToolRegistry>();
        REQUIRE(registry->Publish({Tool(adapter, owner)}).HasValue());
        auto created = McpController::Create(registry, limits);
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    McpRequestContext Context(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::minutes{1}) {
        McpRequestContext context;
        context.session = {1, 1};
        context.deadline = deadline;
        return context;
    }

    McpRequest Call(nlohmann::json id = 7) {
        return {.id = std::move(id), .method = "tools/call", .params = {{"name", "test.call"}, {"arguments", nlohmann::json::object()}}};
    }

    nlohmann::json Get(McpController &controller, const McpRequestContext &context, const std::uint64_t id) {
        auto result = controller.Dispatch({.id = 99, .method = "operations/get", .params = {{"operationId", id}}}, context);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }
}  // namespace

TEST_CASE("MCP calls execute only when their bound owner pumps and retain correlation", "[mcp][controller]") {
    auto adapter = std::make_shared<Adapter>();
    auto controller = Controller(adapter);
    const auto context = Context();
    nlohmann::json accepted;
    const std::thread::id transport = std::this_thread::get_id();
    std::thread caller([&] {
        const auto result = controller->Dispatch(Call("corr-7"), context);
        CHECK(result.HasValue());
        accepted = result.Value();
    });
    caller.join();
    REQUIRE(accepted["status"] == "queued");
    REQUIRE(accepted["requestId"] == "corr-7");
    REQUIRE(adapter->calls == 0);
    REQUIRE(controller->Pump(McpOwnerContext::Editor).HasError());
    std::thread owner([&] {
        REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
        const auto pumped = controller->Pump(McpOwnerContext::Editor);
        REQUIRE(pumped.HasValue());
        CHECK(pumped.Value() == 1);
        CHECK(adapter->invokedOn == std::this_thread::get_id());
    });
    owner.join();
    CHECK(adapter->invokedOn != transport);
    const auto final = Get(*controller, context, accepted["operationId"].get<std::uint64_t>());
    CHECK(final["status"] == "succeeded");
    CHECK(final["result"] == nlohmann::json{{"ok", true}});
    CHECK(final["requestId"] == "corr-7");
}

TEST_CASE("MCP pending and active budgets reject excess without invoking adapters", "[mcp][controller]") {
    auto adapter = std::make_shared<Adapter>();
    McpControllerLimits limits;
    limits.maximumPending = 1;
    limits.maximumActive = 1;
    auto controller = Controller(adapter, limits);
    const auto context = Context();
    REQUIRE(controller->Dispatch(Call(1), context).HasValue());
    const auto full = controller->Dispatch(Call(2), context);
    REQUIRE(full.HasError());
    CHECK(full.ErrorValue().code.Value() == McpErrors::OperationCapacityExceeded.code.Value());
    CHECK(adapter->calls == 0);
}

TEST_CASE("MCP rejects a stale registry revision before queuing owner work", "[mcp][controller]") {
    auto adapter = std::make_shared<Adapter>();
    auto controller = Controller(adapter);
    auto context = Context();
    context.registryRevision = 99;
    const auto result = controller->Dispatch(Call(), context);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == McpErrors::RegistryRevisionStale.code.Value());
    CHECK(adapter->calls == 0);
}

TEST_CASE("MCP operation identity is unique and generation scoped", "[mcp][controller]") {
    auto adapter = std::make_shared<Adapter>();
    auto controller = Controller(adapter);
    const auto context = Context();
    const auto accepted = controller->Dispatch(Call("same-id"), context);
    REQUIRE(accepted.HasValue());
    const auto duplicate = controller->Dispatch(Call("same-id"), context);
    REQUIRE(duplicate.HasError());
    CHECK(duplicate.ErrorValue().code.Value() == McpErrors::RequestInvalid.code.Value());
    auto otherGeneration = context;
    otherGeneration.session.generation = 2;
    const auto denied =
        controller->Dispatch({.id = 3, .method = "operations/get", .params = {{"operationId", accepted.Value()["operationId"]}}},
                             otherGeneration);
    REQUIRE(denied.HasError());
    CHECK(denied.ErrorValue().code.Value() == McpErrors::OperationUnavailable.code.Value());
    const auto cancelled =
        controller->Dispatch({.id = 4, .method = "operations/cancel", .params = {{"operationId", accepted.Value()["operationId"]}}},
                             context);
    REQUIRE(cancelled.HasValue());
    CHECK(cancelled.Value()["status"] == "cancelled");
    CHECK(adapter->calls == 0);
}

TEST_CASE("MCP cancellation and deadline win before owner work and finalize once", "[mcp][controller]") {
    auto adapter = std::make_shared<Adapter>();
    auto controller = Controller(adapter);
    REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
    const auto context = Context();
    const auto cancelled = controller->Dispatch(Call(1), context);
    REQUIRE(cancelled.HasValue());
    const auto id = cancelled.Value()["operationId"].get<std::uint64_t>();
    REQUIRE(controller->CancelAccepted(context.session, 1).HasValue());
    CHECK(Get(*controller, context, id)["status"] == "cancelled");
    CHECK(controller->Pump(McpOwnerContext::Editor).Value() == 0);
    CHECK(Get(*controller, context, id)["status"] == "cancelled");

    const auto expired = Context(std::chrono::steady_clock::now() - std::chrono::seconds{1});
    const auto timed = controller->Dispatch(Call(2), expired);
    REQUIRE(timed.HasValue());
    CHECK(timed.Value()["status"] == "timed_out");
    CHECK(controller->Pump(McpOwnerContext::Editor).Value() == 0);
    CHECK(adapter->calls == 0);
}

TEST_CASE("MCP completion wins later cancellation and preserves application errors", "[mcp][controller]") {
    auto adapter = std::make_shared<Adapter>();
    adapter->action = [](const McpRequestContext &) {
        return Result<nlohmann::json>::Failure(MakeError(McpErrors::ToolInputInvalid));
    };
    auto controller = Controller(adapter);
    REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
    const auto context = Context();
    const auto accepted = controller->Dispatch(Call(), context);
    REQUIRE(accepted.HasValue());
    REQUIRE(controller->Pump(McpOwnerContext::Editor).Value() == 1);
    const auto id = accepted.Value()["operationId"].get<std::uint64_t>();
    const auto final = Get(*controller, context, id);
    CHECK(final["status"] == "failed");
    CHECK(final["error"]["code"] == McpErrors::ToolInputInvalid.code.Value());
    CHECK(controller->CancelAccepted(context.session, 7).HasError());
    CHECK(Get(*controller, context, id) == final);
}

TEST_CASE("MCP progress is bounded and terminal results ignore late callbacks", "[mcp][controller]") {
    auto adapter = std::make_shared<Adapter>();
    McpRequestContext saved;
    adapter->action = [&saved](const McpRequestContext &context) {
        saved = context;
        context.ReportProgress(0.5, "building");
        context.ReportProgress(2.0, "invalid");
        return Result<nlohmann::json>::Success({{"ok", true}});
    };
    auto controller = Controller(adapter);
    REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
    const auto context = Context();
    const auto accepted = controller->Dispatch(Call(), context);
    REQUIRE(accepted.HasValue());
    REQUIRE(controller->Pump(McpOwnerContext::Editor).Value() == 1);
    const auto id = accepted.Value()["operationId"].get<std::uint64_t>();
    const auto final = Get(*controller, context, id);
    CHECK(final["progress"] == 0.5);
    CHECK(final["phase"] == "building");
    saved.ReportProgress(0.9, "late");
    CHECK(Get(*controller, context, id) == final);
}

TEST_CASE("slow MCP application work completes after the owner pump returns", "[mcp][controller]") {
    auto adapter = std::make_shared<DeferredAdapter>();
    auto controller = Controller(adapter);
    REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
    const auto context = Context();
    const auto accepted = controller->Dispatch(Call(), context);
    REQUIRE(accepted.HasValue());
    const auto id = accepted.Value()["operationId"].get<std::uint64_t>();
    REQUIRE(controller->Pump(McpOwnerContext::Editor).Value() == 1);
    CHECK(Get(*controller, context, id)["status"] == "running");
    adapter->savedContext.ReportProgress(0.75, "compiling");
    CHECK(Get(*controller, context, id)["progress"] == 0.75);
    std::thread worker([&] {
        adapter->completion(Result<nlohmann::json>::Success({{"ok", true}}));
    });
    worker.join();
    const auto final = Get(*controller, context, id);
    CHECK(final["status"] == "succeeded");
    CHECK(final["result"] == nlohmann::json{{"ok", true}});
    adapter->completion(Result<nlohmann::json>::Success({{"ok", false}}));
    CHECK(Get(*controller, context, id) == final);
    CHECK(controller->Shutdown().HasValue());
}

TEST_CASE("MCP shutdown drains deferred application completion after bounded timeout", "[mcp][controller]") {
    auto adapter = std::make_shared<DeferredAdapter>();
    McpControllerLimits limits;
    limits.shutdownDrainTimeout = std::chrono::milliseconds{10};
    auto controller = Controller(adapter, limits);
    REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
    const auto context = Context();
    const auto accepted = controller->Dispatch(Call(), context);
    REQUIRE(accepted.HasValue());
    REQUIRE(controller->Pump(McpOwnerContext::Editor).Value() == 1);
    REQUIRE(controller->Shutdown().HasError());
    const auto id = accepted.Value()["operationId"].get<std::uint64_t>();
    CHECK(Get(*controller, context, id)["status"] == "cancelled");
    CHECK(adapter->savedContext.IsStopRequested());
    adapter->completion(Result<nlohmann::json>::Success({{"ok", true}}));
    CHECK(Get(*controller, context, id)["status"] == "cancelled");
    CHECK(controller->Shutdown().HasValue());
}

TEST_CASE("MCP deferred callback remains safe after a timed-out controller destructor", "[mcp][controller]") {
    auto adapter = std::make_shared<DeferredAdapter>();
    const std::weak_ptr<DeferredAdapter> weakAdapter = adapter;
    McpControllerLimits limits;
    limits.shutdownDrainTimeout = std::chrono::milliseconds{10};
    auto controller = Controller(adapter, limits);
    REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
    const auto context = Context();
    REQUIRE(controller->Dispatch(Call(), context).HasValue());
    REQUIRE(controller->Pump(McpOwnerContext::Editor).Value() == 1);
    const auto savedContext = adapter->savedContext;
    auto complete = std::move(adapter->completion);
    adapter.reset();
    controller.reset();
    CHECK(savedContext.IsStopRequested());
    CHECK(weakAdapter.expired());
    CHECK_NOTHROW(complete(Result<nlohmann::json>::Success({{"ok", true}})));
}

TEST_CASE("MCP shutdown waits for an owner pump after its callback completes", "[mcp][controller]") {
    auto gate = std::make_shared<Gate>();
    auto adapter = std::make_shared<CompleteBeforeReturnAdapter>(gate);
    McpControllerLimits limits;
    limits.shutdownDrainTimeout = std::chrono::milliseconds{10};
    auto controller = Controller(adapter, limits, McpOwnerContext::Background);
    const auto context = Context();
    REQUIRE(controller->Dispatch(Call(), context).HasValue());
    std::thread owner([&] {
        REQUIRE(controller->BindOwner(McpOwnerContext::Background).HasValue());
        static_cast<void>(controller->Pump(McpOwnerContext::Background));
    });
    gate->Wait();
    const auto draining = controller->Shutdown();
    REQUIRE(draining.HasError());
    CHECK(draining.ErrorValue().code.Value() == McpErrors::DrainTimedOut.code.Value());
    gate->Release();
    owner.join();
    CHECK(controller->Shutdown().HasValue());
}

TEST_CASE("MCP validates deferred results before publishing terminal success", "[mcp][controller]") {
    auto adapter = std::make_shared<DeferredAdapter>();
    auto controller = Controller(adapter);
    REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
    const auto context = Context();
    const auto accepted = controller->Dispatch(Call(), context);
    REQUIRE(accepted.HasValue());
    REQUIRE(controller->Pump(McpOwnerContext::Editor).Value() == 1);
    adapter->completion(Result<nlohmann::json>::Success({{"ok", "wrong-type"}}));
    const auto final = Get(*controller, context, accepted.Value()["operationId"].get<std::uint64_t>());
    CHECK(final["status"] == "failed");
    CHECK(final["error"]["code"] == McpErrors::ToolOutputInvalid.code.Value());
    CHECK(controller->Shutdown().HasValue());
}

TEST_CASE("MCP deadline wins a deferred completion after the owner pump", "[mcp][controller]") {
    auto adapter = std::make_shared<DeferredAdapter>();
    auto controller = Controller(adapter);
    REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
    const auto context = Context(deadline);
    const auto accepted = controller->Dispatch(Call(), context);
    REQUIRE(accepted.HasValue());
    REQUIRE(controller->Pump(McpOwnerContext::Editor).Value() == 1);
    const auto id = accepted.Value()["operationId"].get<std::uint64_t>();
    CHECK(Get(*controller, context, id)["status"] == "running");
    std::this_thread::sleep_until(deadline + std::chrono::milliseconds{1});
    adapter->completion(Result<nlohmann::json>::Success({{"ok", true}}));
    const auto final = Get(*controller, context, id);
    CHECK(final["status"] == "timed_out");
    CHECK(final["error"]["code"] == McpErrors::RequestTimedOut.code.Value());
    CHECK(adapter->savedContext.IsStopRequested());
    CHECK(controller->Shutdown().HasValue());
}

TEST_CASE("MCP error data retains typed causes while redacting private details", "[mcp][controller]") {
    Error error = MakeError(McpErrors::ControllerFailed, "/private/path");
    error.diagnostics.push_back({.code = DiagnosticCode{"input.invalid"},
                                 .severity = DiagnosticSeverity::Error,
                                 .message = "private token",
                                 .path = "/private/path"});
    error = WithCause(std::move(error), MakeError(McpErrors::ToolInputInvalid, "private cause"));
    const auto data = SafeErrorData(error);
    CHECK(data["domain"] == "horo.mcp");
    CHECK(data["diagnostics"][0]["code"] == "input.invalid");
    CHECK(data["causes"][0]["code"] == McpErrors::ToolInputInvalid.code.Value());
    CHECK(data.dump().find("private") == std::string::npos);
}

TEST_CASE("local and embedded MCP adapters share queued execution and request cancellation", "[mcp][controller]") {
    auto adapter = std::make_shared<Adapter>();
    auto controller = Controller(adapter);
    auto manager = McpSessionManager::Create(controller);
    REQUIRE(manager.HasValue());
    const McpSessionAdmission admission{.clientIdentity = "test-client", .registryRevision = 1};
    auto embedded = McpInProcessAdapter::Start(manager.Value(), admission);
    auto local = McpLocalTransport::Start(manager.Value(), admission);
    REQUIRE(embedded.HasValue());
    REQUIRE(local.HasValue());
    const auto first = embedded.Value()->Call(Call("embedded"));
    REQUIRE(first.HasValue());
    auto framed = local.Value()->Feed(R"({"jsonrpc":"2.0","id":"local","method":"tools/call","params":{"name":"test.call","arguments":{}}})"
                                      "\n");
    REQUIRE(framed.HasValue());
    REQUIRE(framed.Value().size() == 1);
    const auto second = nlohmann::json::parse(framed.Value()[0])["result"];
    CHECK(first.Value()["status"] == "queued");
    CHECK(second["status"] == "queued");
    CHECK(adapter->calls == 0);
    REQUIRE(embedded.Value()->Cancel("embedded").HasValue());
    REQUIRE(local.Value()
                ->Feed(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"local"}})"
                       "\n")
                .HasValue());
    REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
    CHECK(controller->Pump(McpOwnerContext::Editor).Value() == 0);
    CHECK(adapter->calls == 0);
    const auto firstStatus =
        embedded.Value()->Call({.id = "status", .method = "operations/get", .params = {{"operationId", first.Value()["operationId"]}}});
    REQUIRE(firstStatus.HasValue());
    CHECK(firstStatus.Value()["status"] == "cancelled");
    framed = local.Value()->Feed("{\"jsonrpc\":\"2.0\",\"id\":\"status\",\"method\":\"operations/get\",\"params\":{\"operationId\":" +
                                 second["operationId"].dump() + "}}\n");
    REQUIRE(framed.HasValue());
    CHECK(nlohmann::json::parse(framed.Value()[0])["result"]["status"] == "cancelled");
}

TEST_CASE("MCP cancellation racing a running completion publishes one terminal result", "[mcp][controller]") {
    auto adapter = std::make_shared<Adapter>();
    auto gate = std::make_shared<Gate>();
    adapter->action = [gate](const McpRequestContext &context) {
        gate->Enter();
        CHECK(context.IsStopRequested());
        return Result<nlohmann::json>::Success({{"ok", true}});
    };
    auto controller = Controller(adapter, {}, McpOwnerContext::Background);
    const auto context = Context();
    const auto accepted = controller->Dispatch(Call(), context);
    REQUIRE(accepted.HasValue());
    std::thread owner([&] {
        REQUIRE(controller->BindOwner(McpOwnerContext::Background).HasValue());
        static_cast<void>(controller->Pump(McpOwnerContext::Background));
    });
    gate->Wait();
    REQUIRE(controller->CancelAccepted(context.session, 7).HasValue());
    const auto id = accepted.Value()["operationId"].get<std::uint64_t>();
    CHECK(Get(*controller, context, id)["status"] == "cancelled");
    gate->Release();
    owner.join();
    CHECK(Get(*controller, context, id)["status"] == "cancelled");
}

TEST_CASE("MCP shutdown cancels running work and drains its callback lease", "[mcp][controller]") {
    auto adapter = std::make_shared<Adapter>();
    auto gate = std::make_shared<Gate>();
    adapter->action = [gate](const McpRequestContext &) {
        gate->Enter();
        return Result<nlohmann::json>::Success({{"ok", true}});
    };
    McpControllerLimits limits;
    limits.shutdownDrainTimeout = std::chrono::milliseconds{10};
    auto controller = Controller(adapter, limits, McpOwnerContext::Background);
    const auto context = Context();
    const auto accepted = controller->Dispatch(Call(), context);
    REQUIRE(accepted.HasValue());
    std::thread owner([&] {
        REQUIRE(controller->BindOwner(McpOwnerContext::Background).HasValue());
        static_cast<void>(controller->Pump(McpOwnerContext::Background));
    });
    gate->Wait();
    const auto draining = controller->Shutdown();
    REQUIRE(draining.HasError());
    CHECK(draining.ErrorValue().code.Value() == McpErrors::DrainTimedOut.code.Value());
    CHECK(Get(*controller, context, accepted.Value()["operationId"].get<std::uint64_t>())["status"] == "cancelled");
    CHECK(controller->Dispatch(Call(8), context).HasError());
    gate->Release();
    owner.join();
    CHECK(controller->Shutdown().HasValue());
}
