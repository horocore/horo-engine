#include "../../support/OwnedTestDirectory.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Logging/StructuredLogStore.h"
#include "Horo/Mcp/McpController.h"
#include "Horo/Mcp/McpErrors.h"
#include "Horo/Mcp/McpInProcessAdapter.h"
#include "Horo/Mcp/McpLocalTransport.h"
#include "McpAuthorizationTestSupport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>

using namespace Horo;
using namespace Horo::Mcp;

namespace {
#if HORO_ENABLE_TELEMETRY
    /** @brief Holds the consumer outside the queue lock while the fixture submits audit events. */
    class AuditDispatchGate final : public Telemetry::ISink {
    public:
        void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *) override {
            if (record.subsystem == "fixture.mcp_audit") {
                entered.release();
                resume.acquire();
            }
        }

        void Flush() override {}

        std::binary_semaphore entered{0};
        std::binary_semaphore resume{0};
    };

    struct AuditLog final {
        Horo::Tests::OwnedTestDirectory directory{"horo-mcp-audit"};
        std::shared_ptr<AuditDispatchGate> gate{std::make_shared<AuditDispatchGate>()};
        std::shared_ptr<Log::StructuredLogStore> presentation{std::make_shared<Log::StructuredLogStore>(32)};
        bool held{};

        AuditLog() {
            REQUIRE(Log::Logger::Init(Log::LoggerConfiguration{.logDirectory = directory.Path(),
                                                               .baseName = "mcp-test",
                                                               .queueCapacity = 64,
                                                               .additionalSinks = {gate},
                                                               .writeSessionMarkers = false,
                                                               .echoToStderr = false}));
            Log::Logger::SetStructuredLogStore(presentation);
        }

        ~AuditLog() {
            Release();
            Log::Logger::Shutdown();
            Log::Logger::SetStructuredLogStore(nullptr);
        }

        void Hold() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            do {
                held = Telemetry::Runtime::EmitEvent("fixture.mcp_audit", "hold", Log::Level::Info, "Hold dispatcher for audit fixture");
                if (!held)
                    std::this_thread::yield();
            } while (!held && std::chrono::steady_clock::now() < deadline);
            REQUIRE(held);
            REQUIRE(gate->entered.try_acquire_until(deadline));
        }

        void Release() {
            if (std::exchange(held, false))
                gate->resume.release();
        }

        std::vector<nlohmann::json> Read() {
            Release();
            REQUIRE(Log::Logger::Flush());
            std::ifstream input(directory.Path() / "mcp-test.jsonl");
            REQUIRE(input.is_open());
            std::vector<nlohmann::json> rows;
            for (std::string line; std::getline(input, line);) {
                auto row = nlohmann::json::parse(line);
                if (row.at("category") == "mcp.audit")
                    rows.push_back(std::move(row));
            }
            return rows;
        }
    };
#endif

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
                               .effect = McpToolEffect::Query},
                .adapter = adapter,
                .owner = owner};
    }

    std::shared_ptr<McpController> Controller(const std::shared_ptr<IMcpToolAdapter> &adapter, McpControllerLimits limits = {},
                                              McpOwnerContext owner = McpOwnerContext::Editor) {
        auto registry = std::make_shared<McpToolRegistry>(Test::Authorization());
        REQUIRE(registry->Publish({Tool(adapter, owner)}).HasValue());
        auto created = McpController::Create(registry, limits, Test::Authorization());
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    McpRequestContext Context(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::minutes{1}) {
        McpRequestContext context = Test::Context();
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

    /** @brief Sends a JSON-RPC request through the local transport and returns its successful result. */
    nlohmann::json LocalResult(McpLocalTransport &transport, const McpRequest &request) {
        const nlohmann::json wire{{"jsonrpc", "2.0"}, {"id", request.id}, {"method", request.method}, {"params", request.params}};
        const auto framed = transport.Feed(wire.dump() + '\n');
        REQUIRE(framed.HasValue());
        REQUIRE(framed.Value().size() == 1);
        const auto response = nlohmann::json::parse(framed.Value().front());
        REQUIRE(response.at("id") == request.id);
        return response.at("result");
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

#if HORO_ENABLE_TELEMETRY
TEST_CASE("MCP audit persists only generated identities and registered operation metadata", "[mcp][controller][audit]") {
    AuditLog log;
    log.Hold();
    const auto policy = Test::Authorization();
    auto adapter = std::make_shared<Adapter>();
    adapter->action = [](const McpRequestContext &) {
        return Result<nlohmann::json>::Success({{"detail", "private-result-marker"}});
    };
    auto registration = Tool(adapter);
    registration.descriptor.effect = McpToolEffect::Mutation;
    registration.descriptor.inputSchema["properties"] = {{"detail", {{"type", "string"}, {"maxLength", 64}}}};
    registration.descriptor.outputSchema = {{"type", "object"},
                                            {"properties", {{"detail", {{"type", "string"}, {"maxLength", 64}}}}},
                                            {"additionalProperties", false}};
    const auto registry = std::make_shared<McpToolRegistry>(policy);
    auto revision = registry->Publish({std::move(registration)});
    REQUIRE(revision.HasValue());
    auto controller = McpController::Create(registry, {}, policy);
    REQUIRE(controller.HasValue());
    REQUIRE(controller.Value()->BindOwner(McpOwnerContext::Editor).HasValue());
    auto manager = McpSessionManager::Create(controller.Value(), {}, policy);
    REQUIRE(manager.HasValue());
    const auto admission = Test::Authenticate({.clientIdentity = "private-client-marker", .registryRevision = revision.Value()}, policy);
    {
        Log::LogContext ambient{"ambient.note", "private-context-marker"};
        auto client = McpInProcessAdapter::Start(manager.Value(), admission);
        REQUIRE(client.HasValue());
        const McpRequestContext context{.session = client.Value()->Session(),
                                        .clientIdentity = admission.clientIdentity,
                                        .authorizationRevision = admission.authorizationRevision,
                                        .registryRevision = admission.registryRevision,
                                        .deadline = std::chrono::steady_clock::now() + std::chrono::minutes{1},
                                        .authority = admission.authority,
                                        .authorization = policy};
        auto request = Call("private-request-marker");
        request.params["arguments"]["detail"] = "private-argument-marker";
        CHECK(client.Value()->Call(request).HasError());
        const auto challenge = policy->Challenge(context, request, std::chrono::minutes{1});
        REQUIRE(challenge.HasValue());
        REQUIRE(policy->Decide(challenge.Value(), true).HasValue());
        const auto accepted = client.Value()->Call(request);
        REQUIRE(accepted.HasValue());
        REQUIRE(controller.Value()->Pump(McpOwnerContext::Editor).HasValue());
        CHECK(Get(*controller.Value(), context, accepted.Value().at("operationId").get<std::uint64_t>())["result"]["detail"] ==
              "private-result-marker");
        client.Value()->Disconnect();
    }

    const auto rows = log.Read();
    const auto find = [&](const std::string_view name) -> const nlohmann::json & {
        const auto row = std::ranges::find_if(rows, [name](const nlohmann::json &entry) {
            return entry.at("message").get_ref<const std::string &>() == name;
        });
        REQUIRE(row != rows.end());
        return row->at("fields");
    };
    CHECK(find("session.opened").at("principal_id") == admission.authority->AuditIdentity());
    CHECK(find("operation.rejected").at("status") == "authorization_denied");
    CHECK(find("approval.requested").at("approval_id") == find("approval.decided").at("approval_id"));
    CHECK(find("approval.decided").at("status") == "approved");
    CHECK(find("operation.queued").at("operation_id") == find("operation.finished").at("operation_id"));
    CHECK(find("operation.finished").at("tool") == "test.call");
    CHECK(find("operation.finished").at("status") == "succeeded");
    CHECK(find("operation.finished").at("elapsed_ms").is_number_integer());
    CHECK(find("session.retired").at("status") == "closed");
    const nlohmann::json persisted = rows;
    CHECK(persisted.dump().find("private-") == std::string::npos);
    const auto live = log.presentation->SnapshotIfChanged(0);
    REQUIRE(live);
    const auto finished = std::ranges::find_if(live->records, [](const auto &row) {
        return row->category == "mcp.audit" && row->message == "operation.finished";
    });
    REQUIRE(finished != live->records.end());
    CHECK((*finished)->context.empty());
    const auto &fields = (*finished)->fields;
    const auto tool = std::ranges::find(fields, "tool", &Telemetry::Field::key);
    REQUIRE(tool != fields.end());
    CHECK(std::get<std::string>(tool->value) == "test.call");
}

TEST_CASE("An overflowing MCP audit queue cannot stop command execution or shutdown", "[mcp][controller][audit]") {
    AuditLog log;
    log.Hold();
    for (int index = 0; index < 128; ++index)
        static_cast<void>(Telemetry::Runtime::EmitEvent("fixture.queue", "fill", Log::Level::Info, "Fill the audit test queue"));
    CHECK_FALSE(Telemetry::Runtime::EmitEvent("fixture.queue", "overflow", Log::Level::Info, "Verify queue saturation"));
    auto adapter = std::make_shared<Adapter>();
    auto controller = Controller(adapter);
    const auto context = Context();
    REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
    const auto accepted = controller->Dispatch(Call(), context);
    REQUIRE(accepted.HasValue());
    REQUIRE(controller->Pump(McpOwnerContext::Editor).HasValue());
    CHECK(Get(*controller, context, accepted.Value().at("operationId").get<std::uint64_t>())["status"] == "succeeded");
    CHECK(adapter->calls == 1);
    CHECK(controller->Shutdown().HasValue());
}
#endif

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
    auto context = Test::Context({.clientIdentity = "test-client", .registryRevision = 99});
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
    CHECK(final["phase"] == "");  // Free-text progress is not a credential-safe presentation contract.
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

TEST_CASE("Local and in-process MCP replies redact nested Save failure details", "[mcp][controller][save-privacy]") {
    auto adapter = std::make_shared<Adapter>();
    adapter->action = [](const McpRequestContext &) {
        Error save{.code = ErrorCode{"save.archive.chunk_hash_mismatch"},
                   .domain = ErrorDomainId{"horo.save"},
                   .message = "private-fixture-slot-title"};
        Error provider{.code = ErrorCode{"storage.read_failed"},
                       .domain = ErrorDomainId{"horo.platform"},
                       .message = "private-fixture-provider-detail"};
        auto failure = WrapError(McpErrors::ControllerFailed, WithCause(std::move(save), std::move(provider)), "private-fixture-wrapper");
        failure.diagnostics.push_back(
            {.code = DiagnosticCode{"storage.integrity"}, .message = "private-fixture-payload", .path = "/private-fixture/recovery"});
        return Result<nlohmann::json>::Failure(std::move(failure));
    };
    auto controller = Controller(adapter);
    auto manager = McpSessionManager::Create(controller, {}, Test::Authorization());
    REQUIRE(manager.HasValue());
    const auto admission = Test::Authenticate({.clientIdentity = "test-client", .registryRevision = 1});
    const auto embedded = McpInProcessAdapter::Start(manager.Value(), admission);
    const auto local = McpLocalTransport::Start(manager.Value(), admission);
    REQUIRE(embedded.HasValue());
    REQUIRE(local.HasValue());
    const auto embeddedCall = embedded.Value()->Call(Call("embedded-save"));
    REQUIRE(embeddedCall.HasValue());
    const auto localCall = LocalResult(*local.Value(), Call("local-save"));
    REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
    const auto pumped = controller->Pump(McpOwnerContext::Editor);
    REQUIRE(pumped.HasValue());
    REQUIRE(pumped.Value() == 2);

    const auto embeddedStatus = embedded.Value()->Call(
        {.id = "embedded-status", .method = "operations/get", .params = {{"operationId", embeddedCall.Value().at("operationId")}}});
    REQUIRE(embeddedStatus.HasValue());
    const auto localStatus =
        LocalResult(*local.Value(),
                    {.id = "local-status", .method = "operations/get", .params = {{"operationId", localCall.at("operationId")}}});
    CHECK(localStatus.at("status") == "failed");
    CHECK(embeddedStatus.Value().at("status") == "failed");
    const auto &failure = localStatus.at("error");
    CHECK(failure == embeddedStatus.Value().at("error"));
    const auto expectedCauses = nlohmann::json::array({{{"domain", "horo.save"}, {"code", "save.archive.chunk_hash_mismatch"}},
                                                       {{"domain", "horo.platform"}, {"code", "storage.read_failed"}}});
    CHECK(failure.at("causes") == expectedCauses);
    CHECK(failure.at("diagnostics").at(0).at("code") == "storage.integrity");
    CHECK(localStatus.dump().find("private-fixture") == std::string::npos);
    CHECK(embeddedStatus.Value().dump().find("private-fixture") == std::string::npos);
}

TEST_CASE("local and embedded MCP adapters share queued execution and request cancellation", "[mcp][controller]") {
    auto adapter = std::make_shared<Adapter>();
    auto controller = Controller(adapter);
    auto manager = McpSessionManager::Create(controller, {}, Test::Authorization());
    REQUIRE(manager.HasValue());
    const McpSessionAdmission admission = Test::Authenticate({.clientIdentity = "test-client", .registryRevision = 1});
    auto embedded = McpInProcessAdapter::Start(manager.Value(), admission);
    auto local = McpLocalTransport::Start(manager.Value(), admission);
    REQUIRE(embedded.HasValue());
    REQUIRE(local.HasValue());
    const auto first = embedded.Value()->Call(Call("embedded"));
    REQUIRE(first.HasValue());
    const auto second = LocalResult(*local.Value(), Call("local"));
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
    const auto secondStatus =
        LocalResult(*local.Value(), {.id = "status", .method = "operations/get", .params = {{"operationId", second.at("operationId")}}});
    CHECK(secondStatus.at("status") == "cancelled");
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
