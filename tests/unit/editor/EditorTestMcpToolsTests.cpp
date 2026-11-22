#include "../../support/OwnedTestDirectory.h"
#include "../mcp/McpAuthorizationTestSupport.h"
#include "EditorTestMcpTools.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Mcp/McpController.h"
#include "Horo/Mcp/McpInProcessAdapter.h"
#include "Horo/Platform/ExternalProcess.h"

#include <atomic>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Mcp;
    using Json = nlohmann::json;
    using namespace std::chrono_literals;

    class WaitingRunner final : public IExternalProcessRunner {
    public:
        Result<ExternalProcessResult> Run(const ExternalProcessRequest &, const CancellationToken &stop) override {
            calls.fetch_add(1, std::memory_order_release);
            while (!stop.IsCancellationRequested())
                std::this_thread::sleep_for(2ms);
            return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Cancelled, 1, ProcessStopCause::Cancellation});
        }

        std::atomic_uint calls{};
    };

    struct TestSession final {
        std::shared_ptr<McpAuthorization> policy{Test::Authorization()};
        std::shared_ptr<McpController> controller;
        std::shared_ptr<McpSessionManager> manager;
        McpSessionAdmission admission;
        std::shared_ptr<McpInProcessAdapter> embedded;

        explicit TestSession(const std::shared_ptr<EditorTestCommandEndpoint> &endpoint) {
            auto tools = MakeEditorTestMcpTools(endpoint);
            REQUIRE(tools.HasValue());
            auto registry = std::make_shared<McpToolRegistry>(policy);
            std::vector<std::string> grants{std::string{EditorTestExecutionGrant}, std::string{EditorTestObservationGrant}};
            auto revision = registry->Publish(std::move(tools).Value(), grants);
            REQUIRE(revision.HasValue());
            auto created = McpController::Create(registry, {}, policy);
            REQUIRE(created.HasValue());
            controller = std::move(created).Value();
            REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
            auto sessions = McpSessionManager::Create(controller, {}, policy);
            REQUIRE(sessions.HasValue());
            manager = std::move(sessions).Value();
            admission = Test::Authenticate({.clientIdentity = "configured-tests",
                                            .capabilities = std::move(grants),
                                            .projectIdentity = "project-one",
                                            .authorizationRevision = 3,
                                            .registryRevision = revision.Value()},
                                           policy);
            auto connected = McpInProcessAdapter::Start(manager, admission);
            REQUIRE(connected.HasValue());
            embedded = std::move(connected).Value();
        }

        void Approve(const McpRequest &request) const {
            const McpRequestContext context{.session = embedded->Session(),
                                            .clientIdentity = admission.clientIdentity,
                                            .capabilities = admission.capabilities,
                                            .projectIdentity = admission.projectIdentity,
                                            .authorizationRevision = admission.authorizationRevision,
                                            .registryRevision = admission.registryRevision,
                                            .deadline = std::chrono::steady_clock::now() + 1min,
                                            .authority = admission.authority,
                                            .authorization = policy,
                                            .requestIdentity = request.id};
            const auto challenge = policy->Challenge(context, request, 1min);
            REQUIRE(challenge.HasValue());
            REQUIRE(policy->Decide(challenge.Value(), true).HasValue());
        }

        Json Execute(const McpRequest &request) const {
            const auto accepted = embedded->Call(request);
            REQUIRE(accepted.HasValue());
            REQUIRE(controller->Pump(McpOwnerContext::Editor).HasValue());
            const auto status = embedded->Call(
                {.id = "read-result", .method = "operations/get", .params = {{"operationId", accepted.Value().at("operationId")}}});
            REQUIRE(status.HasValue());
            return status.Value();
        }
    };

    McpRequest Command(std::string id, std::string tool, Json arguments = Json::object()) {
        return {.id = std::move(id), .method = "tools/call", .params = {{"name", std::move(tool)}, {"arguments", std::move(arguments)}}};
    }

    TEST_CASE("Configured test commands run through an approved embedded editor session", "[unit][editor][mcp][gameplay-test]") {
        Horo::Tests::OwnedTestDirectory project{"horo-test-mcp"};
        const auto configured = Application::GameplayLocalBuildDirectory(project.Path());
        std::filesystem::create_directories(configured);
        std::filesystem::create_directories(project.Path() / ".horo/local/locks");
        std::ofstream(configured / "CTestTestfile.cmake") << "";
        WaitingRunner runner;
        NativeDurableFileSystem files;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4, .ioWorkerCount = 1}};
        OperationStore nativeOperations{4, 4};
        BuildOutputStore output{32};
        Application::GameplayTestService service(runner, jobs, files, nativeOperations, output);
        const Application::GameplayTestEnvironment environment{HORO_EDITOR_TEST_CTEST};
        const auto endpoint = std::make_shared<EditorTestCommandEndpoint>(
            EditorTestCommandEndpoint{"project-one", 3, std::this_thread::get_id(), [&](const std::function<bool()> &stop) {
            return service.Start(project.Path(), environment, stop);
        }, [&](const auto id) {
            return service.Query(id);
        }, [&](const auto id) {
            return service.RequestCancel(id);
        }});
        const TestSession session{endpoint};

        const auto start = Command("start", "editor.test.start");
        const auto denied = session.embedded->Call(start);
        REQUIRE(denied.HasError());
        CHECK(runner.calls.load(std::memory_order_acquire) == 0);
        session.Approve(start);
        const auto launched = session.Execute(start);
        REQUIRE(launched.at("status") == "succeeded");
        const auto &result = launched.at("result");
        const auto id = result.at("testId");
        CHECK(result.at("operationId") == id);
        CHECK(id.is_string());
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (runner.calls.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(2ms);
        REQUIRE(runner.calls.load(std::memory_order_acquire) == 1);
        service.Poll();
        const auto status = session.Execute(Command("observe", "editor.test.status", {{"testId", id}}));
        CHECK(status.at("status") == "succeeded");
        CHECK(status.at("result").at("testId") == id);
        CHECK(status.at("result").at("state") == "running");

        const auto cancel = Command("stop", "editor.test.cancel", {{"testId", id}});
        CHECK(session.embedded->Call(cancel).HasError());
        session.Approve(cancel);
        CHECK(session.Execute(cancel).at("result").at("cancellationRequested") == true);
        service.Close();
        const auto terminal = session.Execute(Command("terminal", "editor.test.status", {{"testId", id}}));
        CHECK(terminal.at("result").at("state") == "cancelled");
    }
}  // namespace
