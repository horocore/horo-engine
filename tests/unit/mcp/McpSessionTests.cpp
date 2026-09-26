#include "Horo/Mcp/McpErrors.h"
#include "Horo/Mcp/McpInProcessAdapter.h"
#include "Horo/Mcp/McpLocalTransport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>

namespace Horo::Mcp {
    namespace {
        class Controller final : public IMcpRequestController {
        public:
            std::function<Result<nlohmann::json>(const McpRequest &, const McpRequestContext &)> action;

            Result<nlohmann::json> Dispatch(const McpRequest &request, const McpRequestContext &context) override {
                if (action)
                    return action(request, context);
                return Result<nlohmann::json>::Success({{"client", context.clientIdentity},
                                                        {"project", context.projectIdentity.value_or("")},
                                                        {"method", request.method},
                                                        {"params", request.params},
                                                        {"capability", context.capabilities.front()}});
            }
        };

        McpSessionAdmission Admission() {
            return {.clientIdentity = "local-client",
                    .capabilities = {"project.read"},
                    .projectIdentity = "project-one",
                    .authorizationRevision = 3,
                    .registryRevision = 7};
        }

        std::shared_ptr<McpSessionManager> Manager(const std::shared_ptr<Controller> &controller, McpSessionLimits limits = {}) {
            auto created = McpSessionManager::Create(controller, limits);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        void RequireCode(const Error &error, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(error.domain.Value() == descriptor.domain.Value());
            REQUIRE(error.code.Value() == descriptor.code.Value());
        }

        class Gate final {
        public:
            void Enter() {
                std::unique_lock lock{mutex_};
                ++entered_;
                enteredCondition_.notify_all();
                releaseCondition_.wait(lock, [this] {
                    return released_;
                });
            }

            void WaitFor(const int count) {
                std::unique_lock lock{mutex_};
                enteredCondition_.wait(lock, [this, count] {
                    return entered_ >= count;
                });
            }

            void Release() {
                std::lock_guard lock{mutex_};
                released_ = true;
                releaseCondition_.notify_all();
            }

        private:
            std::mutex mutex_;
            std::condition_variable enteredCondition_;
            std::condition_variable releaseCondition_;
            int entered_{};
            bool released_{};
        };
    }  // namespace

    TEST_CASE("local and embedded MCP requests use the same authority and controller", "[mcp][session]") {
        auto controller = std::make_shared<Controller>();
        auto manager = Manager(controller);
        auto embedded = McpInProcessAdapter::Start(manager, Admission());
        auto local = McpLocalTransport::Start(manager, Admission());
        REQUIRE(embedded.HasValue());
        REQUIRE(local.HasValue());

        const McpRequest request{.id = 4, .method = "tools/list", .params = {{"filter", "all"}}};
        const auto direct = embedded.Value()->Call(request);
        REQUIRE(direct.HasValue());
        auto framed = local.Value()->Feed(R"({"jsonrpc":"2.0","id":4,"method":"tools/list","params":{"filter":"all"}})"
                                          "\n");
        REQUIRE(framed.HasValue());
        REQUIRE(framed.Value().size() == 1);
        REQUIRE(nlohmann::json::parse(framed.Value()[0])["result"] == direct.Value());
        REQUIRE(direct.Value()["client"] == "local-client");
        REQUIRE(direct.Value()["project"] == "project-one");
        REQUIRE(manager->ActiveSessions() == 2);
    }

    TEST_CASE("MCP admission and request budgets are finite and typed", "[mcp][session]") {
        McpSessionLimits limits;
        limits.maximumSessions = 1;
        limits.maximumInFlightPerSession = 1;
        limits.maximumInputBytes = 128;
        limits.maximumStringBytes = 64;
        auto controller = std::make_shared<Controller>();
        auto manager = Manager(controller, limits);
        auto bad = Admission();
        bad.capabilities.push_back("project.read");
        const auto invalid = manager->Open(bad);
        REQUIRE(invalid.HasError());
        RequireCode(invalid.ErrorValue(), McpErrors::AdmissionInvalid);

        auto opened = manager->Open(Admission());
        REQUIRE(opened.HasValue());
        const auto full = manager->Open(Admission());
        REQUIRE(full.HasError());
        RequireCode(full.ErrorValue(), McpErrors::SessionCapacityExceeded);

        const auto oversized =
            manager->Dispatch(opened.Value(), {.id = 1, .method = "tools/call", .params = {{"data", std::string(100, 'x')}}});
        REQUIRE(oversized.HasError());
        RequireCode(oversized.ErrorValue(), McpErrors::InputCapacityExceeded);
        manager->Close(opened.Value());
        REQUIRE(manager->ActiveSessions() == 0);
    }

    TEST_CASE("MCP cancellation, project switch and disconnect revoke old work", "[mcp][session]") {
        auto gate = std::make_shared<Gate>();
        auto controller = std::make_shared<Controller>();
        std::atomic<bool> cancelled{false};
        controller->action = [gate, &cancelled](const McpRequest &, const McpRequestContext &context) {
            gate->Enter();
            cancelled = context.cancellation.IsCancellationRequested();
            return Result<nlohmann::json>::Success({{"done", true}});
        };
        auto manager = Manager(controller);
        auto embedded = McpInProcessAdapter::Start(manager, Admission());
        REQUIRE(embedded.HasValue());
        const auto oldHandle = embedded.Value()->Session();
        std::atomic<bool> completed{false};
        std::thread worker([&] {
            const auto result = embedded.Value()->Call({.id = "old", .method = "tools/call"});
            completed = result.HasValue();
        });
        gate->WaitFor(1);
        const auto cancelledRequest = embedded.Value()->Cancel("old");
        const auto switched = embedded.Value()->SwitchProject("project-two");
        gate->Release();
        worker.join();
        REQUIRE(cancelledRequest.HasValue());
        REQUIRE(switched.HasValue());
        REQUIRE(switched.Value().generation == oldHandle.generation + 1);
        const auto stale = manager->Dispatch(oldHandle, {.id = 2, .method = "tools/list"});
        REQUIRE(stale.HasError());
        RequireCode(stale.ErrorValue(), McpErrors::SessionUnavailable);
        REQUIRE(completed.load());
        REQUIRE(cancelled.load());
        embedded.Value()->Disconnect();
        REQUIRE(manager->ActiveSessions() == 0);
        REQUIRE(embedded.Value()->Call({.id = 3, .method = "tools/list"}).HasError());
    }

    TEST_CASE("MCP shutdown closes admission and waits only its declared drain budget", "[mcp][session]") {
        auto gate = std::make_shared<Gate>();
        auto controller = std::make_shared<Controller>();
        controller->action = [gate](const McpRequest &, const McpRequestContext &) {
            gate->Enter();
            return Result<nlohmann::json>::Success(nlohmann::json::object());
        };
        McpSessionLimits limits;
        limits.shutdownDrainTimeout = std::chrono::milliseconds{10};
        auto manager = Manager(controller, limits);
        auto embedded = McpInProcessAdapter::Start(manager, Admission());
        REQUIRE(embedded.HasValue());
        std::thread worker([&] {
            static_cast<void>(embedded.Value()->Call({.id = 1, .method = "tools/call"}));
        });
        gate->WaitFor(1);
        const auto stopped = manager->Shutdown();
        const auto rejected = manager->Open(Admission());
        gate->Release();
        worker.join();
        REQUIRE(stopped.HasError());
        RequireCode(stopped.ErrorValue(), McpErrors::DrainTimedOut);
        REQUIRE(rejected.HasError());
        RequireCode(rejected.ErrorValue(), McpErrors::ShuttingDown);
        REQUIRE(manager->Shutdown().HasValue());
    }

    TEST_CASE("concurrent MCP requests respect in-flight and duplicate-ID limits", "[mcp][session]") {
        auto gate = std::make_shared<Gate>();
        auto controller = std::make_shared<Controller>();
        std::atomic<int> badContexts{0};
        controller->action = [gate, &badContexts](const McpRequest &, const McpRequestContext &context) {
            if (context.authorizationRevision != 3 || context.registryRevision != 7 || context.deadline <= std::chrono::steady_clock::now())
                ++badContexts;
            gate->Enter();
            return Result<nlohmann::json>::Success(nlohmann::json::object());
        };
        McpSessionLimits limits;
        limits.maximumInFlightPerSession = 2;
        auto manager = Manager(controller, limits);
        auto embedded = McpInProcessAdapter::Start(manager, Admission());
        REQUIRE(embedded.HasValue());
        std::atomic<int> finished{0};
        std::thread first([&] {
            if (embedded.Value()->Call({.id = 1, .method = "tools/call"}).HasValue())
                ++finished;
        });
        gate->WaitFor(1);
        const auto duplicate = embedded.Value()->Call({.id = 1, .method = "tools/call"});
        std::thread second([&] {
            if (embedded.Value()->Call({.id = 2, .method = "tools/call"}).HasValue())
                ++finished;
        });
        gate->WaitFor(2);
        const auto full = embedded.Value()->Call({.id = 3, .method = "tools/call"});
        gate->Release();
        first.join();
        second.join();
        REQUIRE(duplicate.HasError());
        RequireCode(duplicate.ErrorValue(), McpErrors::RequestInvalid);
        REQUIRE(full.HasError());
        RequireCode(full.ErrorValue(), McpErrors::RequestCapacityExceeded);
        REQUIRE(finished.load() == 2);
        REQUIRE(badContexts.load() == 0);
    }

    TEST_CASE("embedded MCP rejects invalid UTF-8 and unsupported value types before dispatch", "[mcp][session]") {
        auto controller = std::make_shared<Controller>();
        auto manager = Manager(controller);
        auto embedded = McpInProcessAdapter::Start(manager, Admission());
        REQUIRE(embedded.HasValue());
        const auto invalid = embedded.Value()->Call({.id = 1, .method = "ping", .params = {{"bad", std::string(1, '\xff')}}});
        REQUIRE(invalid.HasError());
        RequireCode(invalid.ErrorValue(), McpErrors::InputCapacityExceeded);
        const auto binary = embedded.Value()->Call({.id = 2, .method = "ping", .params = nlohmann::json::binary({1, 2})});
        REQUIRE(binary.HasError());
        RequireCode(binary.ErrorValue(), McpErrors::InputCapacityExceeded);
        const auto nonFinite =
            embedded.Value()->Call({.id = 3, .method = "ping", .params = {{"value", std::numeric_limits<double>::infinity()}}});
        REQUIRE(nonFinite.HasError());
        RequireCode(nonFinite.ErrorValue(), McpErrors::InputCapacityExceeded);
        const auto scalar = embedded.Value()->Call({.id = 4, .method = "ping", .params = 7});
        REQUIRE(scalar.HasError());
        RequireCode(scalar.ErrorValue(), McpErrors::RequestInvalid);
    }
}  // namespace Horo::Mcp
