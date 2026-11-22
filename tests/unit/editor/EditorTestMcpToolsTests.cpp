#include "../../support/OwnedTestDirectory.h"
#include "../mcp/McpAuthorizationTestSupport.h"
#include "EditorCommandMcpSession.h"
#include "EditorInspectionDistance.h"
#include "EditorInspectionMcpTools.h"
#include "EditorMcpWorkspaceHost.h"
#include "EditorSourceMcpTools.h"
#include "EditorTestMcpTools.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Mcp/McpController.h"
#include "Horo/Mcp/McpInProcessAdapter.h"
#include "Horo/Mcp/McpLocalTransport.h"
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

    void Approve(EditorCommandMcpSession &session, const McpRequest &request) {
        const auto challenge = session.RequestApproval(request);
        REQUIRE(challenge.HasValue());
        REQUIRE(session.Decide(challenge.Value(), true).HasValue());
    }

    Json Execute(const EditorCommandMcpSession &session, const McpRequest &request) {
        const auto client = session.Client();
        const auto accepted = client->Call(request);
        REQUIRE(accepted.HasValue());
        REQUIRE(session.Pump().HasValue());
        const auto status = client->Call(
            {.id = "read-result", .method = "operations/get", .params = {{"operationId", accepted.Value().at("operationId")}}});
        REQUIRE(status.HasValue());
        return status.Value();
    }

    McpRequest Command(std::string id, std::string tool, Json arguments = Json::object()) {
        return {.id = std::move(id), .method = "tools/call", .params = {{"name", std::move(tool)}, {"arguments", std::move(arguments)}}};
    }

    TEST_CASE("Editor transform queries carry an exact numeric Scene fence across pages", "[unit][editor][mcp][inspection]") {
        std::vector<ObjectInspectionRequest> reads;
        const InspectionSceneRevision scene{7, 19};
        const auto reader = [&](const ObjectInspectionRequest &request) {
            reads.push_back(request);
            TransformInspectionPage page{.scene = scene,
                                         .records = {{request.objects.at(request.offset), {1, 2, 3}, {0, 0, 0, 1}, {1, 1, 1}}}};
            if (request.offset + 1 < request.objects.size())
                page.nextOffset = request.offset + 1;
            return Result<TransformInspectionPage>::Success(std::move(page));
        };
        const auto endpoint = std::make_shared<const EditorInspectionCommandEndpoint>(
            EditorInspectionCommandEndpoint{"project-one", 3, std::this_thread::get_id(), reader});
        const auto policy = std::make_shared<McpAuthorization>(std::make_shared<Test::Entropy>());
        REQUIRE(policy->SetTrust("project-one", 3, true).HasValue());
        const EditorCommandMcpAdmission admission{"local-client", "project-one", 3, {std::string{EditorSceneInspectionGrant}}};
        auto opened = EditorCommandMcpSession::Create({.inspection = endpoint}, admission, policy);
        REQUIRE(opened.HasValue());
        const auto &session = *opened.Value();
        const auto effect = session.ToolEffect("inspect.transforms");
        REQUIRE(effect.HasValue());
        CHECK(effect.Value() == McpToolEffect::Query);

        const auto first = Execute(session, Command("first", "inspect.transforms", {{"objects", {42, 9}}, {"limit", 1}}));
        REQUIRE(first.at("status") == "succeeded");
        CHECK(first["result"]["records"][0]["object"] == 42);
        CHECK(first["result"]["nextOffset"] == 1);
        CHECK_FALSE(reads.front().expected);
        const auto missingFence = Execute(session, Command("no-fence", "inspect.transforms", {{"objects", {42, 9}}, {"offset", 1}}));
        CHECK(missingFence.at("status") == "failed");
        CHECK(reads.size() == 1);
        const auto second = Execute(session, Command("second", "inspect.transforms",
                                                     {{"objects", {42, 9}}, {"offset", 1}, {"scene", first["result"]["scene"]}}));
        REQUIRE(second.at("status") == "succeeded");
        CHECK(second["result"]["records"][0]["object"] == 9);
        CHECK_FALSE(second["result"].contains("nextOffset"));
        CHECK(reads.back().expected == scene);
        const auto oversize =
            Execute(session, Command("oversize", "inspect.transforms",
                                     {{"objects", std::vector<std::uint64_t>(ObjectInspectionRequest::MaximumObjects + 1, 42)}}));
        CHECK(oversize.at("status") == "failed");
        CHECK(reads.size() == 2);
    }

    TEST_CASE("Bounds pages and explicit unavailable errors match across MCP adapters", "[unit][editor][mcp][inspection]") {
        const auto endpoint = std::make_shared<const EditorInspectionCommandEndpoint>(
            EditorInspectionCommandEndpoint{.project = "project-one",
                                            .authorizationRevision = 3,
                                            .owner = std::this_thread::get_id(),
                                            .readBounds = [](const ObjectInspectionRequest &request) {
            using Output = Result<BoundsInspectionPage>;
            if (request.objects.front() == 9)
                return Output::Failure(MakeError(ErrorCodeDescriptor{.domain = ErrorDomainId{"editor.inspection"},
                                                                     .code = ErrorCode{"bounds_unavailable"},
                                                                     .defaultSeverity = ErrorSeverity::Warning}));
            return Output::Success({.scene = {7, 19}, .records = {{42, {-1, 0, 2}, {1, 4, 6}}}});
        }});
        const auto policy = Test::Authorization();
        auto registry = std::make_shared<McpToolRegistry>(policy);
        const std::vector<std::string> grants{std::string{EditorSceneInspectionGrant}};
        auto registrations = MakeEditorInspectionMcpTools(endpoint);
        REQUIRE(registrations.HasValue());
        const auto revision = registry->Publish(std::move(registrations).Value(), grants);
        REQUIRE(revision.HasValue());
        const auto controller = McpController::Create(registry, {}, policy);
        REQUIRE(controller.HasValue());
        REQUIRE(controller.Value()->BindOwner(McpOwnerContext::Editor).HasValue());
        const auto manager = McpSessionManager::Create(controller.Value(), {}, policy);
        REQUIRE(manager.HasValue());
        const auto admission = Test::Authenticate({.clientIdentity = "inspect-fixture",
                                                   .capabilities = grants,
                                                   .projectIdentity = "project-one",
                                                   .authorizationRevision = 3,
                                                   .registryRevision = revision.Value()},
                                                  policy);
        const auto embedded = McpInProcessAdapter::Start(manager.Value(), admission);
        const auto local = McpLocalTransport::Start(manager.Value(), admission);
        REQUIRE(embedded.HasValue());
        REQUIRE(local.HasValue());

        const auto framed = [&](const McpRequest &request) {
            const auto response = local.Value()->Feed(
                Json{{"jsonrpc", "2.0"}, {"id", request.id}, {"method", request.method}, {"params", request.params}}.dump() + "\n");
            REQUIRE(response.HasValue());
            REQUIRE(response.Value().size() == 1);
            return Json::parse(response.Value().front()).at("result");
        };
        for (const auto object : {42, 9}) {
            const auto request = Command("bounds", "inspect.bounds", {{"objects", {object}}});
            const auto first = embedded.Value()->Call(request);
            REQUIRE(first.HasValue());
            const auto second = framed(request);
            REQUIRE(controller.Value()->Pump(McpOwnerContext::Editor).Value() == 2);
            const auto result = embedded.Value()->Call(
                {.id = "result", .method = "operations/get", .params = {{"operationId", first.Value().at("operationId")}}});
            REQUIRE(result.HasValue());
            const auto peer = framed({.id = "result", .method = "operations/get", .params = {{"operationId", second.at("operationId")}}});
            CHECK(result.Value().at("result") == peer.at("result"));
            CHECK(result.Value().at("error") == peer.at("error"));
            if (object == 42) {
                CHECK(peer["status"] == "succeeded");
                CHECK(peer["result"]["records"][0]["maximum"] == Json::array({1, 4, 6}));
            } else {
                CHECK(peer["status"] == "failed");
                CHECK(peer["error"]["code"] == "bounds_unavailable");
            }
        }
    }

    TEST_CASE("Component counts and committed viewport counts are separately discovered and read-only", "[unit][editor][mcp][inspection]") {
        const auto endpoint = std::make_shared<const EditorInspectionCommandEndpoint>(
            EditorInspectionCommandEndpoint{.project = "project-one",
                                            .authorizationRevision = 3,
                                            .owner = std::this_thread::get_id(),
                                            .readComponents =
                                                [](const ObjectInspectionRequest &request) {
            InspectedComponents record{.object = request.objects.front()};
            record.counts.front() = 1;
            record.counts.back() = 3;
            return Result<ComponentInspectionPage>::Success({.scene = {7, 19}, .records = {record}});
        },
                                            .readGeometry = [](const ObjectInspectionRequest &request) {
            return Result<GeometryInspectionPage>::Success(
                {.scene = {7, 19}, .records = {{.object = request.objects.front(), .instances = 2, .vertices = 30, .triangles = 14}}});
        }});
        const EditorCommandMcpAdmission admission{"numeric-reader", "project-one", 3, {std::string{EditorSceneInspectionGrant}}};
        auto opened = EditorCommandMcpSession::Create({.inspection = endpoint}, admission, Test::Authorization());
        REQUIRE(opened.HasValue());
        const auto &session = *opened.Value();
        REQUIRE(session.ToolEffect("inspect.components").HasValue());
        REQUIRE(session.ToolEffect("inspect.rendering").HasValue());
        CHECK(session.ToolEffect("inspect.components").Value() == McpToolEffect::Query);
        CHECK(session.ToolEffect("inspect.rendering").Value() == McpToolEffect::Query);
        CHECK(session.ToolEffect("inspect.bounds").HasError());
        const auto components = Execute(session, Command("components", "inspect.components", {{"objects", {42}}}));
        REQUIRE(components["status"] == "succeeded");
        const auto &counts = components["result"]["records"][0]["counts"];
        CHECK(counts.size() == InspectedComponents::Names.size());
        CHECK(counts["camera"] == 1);
        CHECK(counts["colliders"] == 0);
        CHECK(counts["navigationSources"] == 3);
        const auto rendering = Execute(session, Command("geometry", "inspect.rendering", {{"objects", {42}}}));
        REQUIRE(rendering["status"] == "succeeded");
        CHECK(rendering["result"]["records"][0] == Json({{"object", 42}, {"instances", 2}, {"vertices", 30}, {"triangles", 14}}));
    }

    TEST_CASE("Bounds measurements distinguish disjoint, intersecting and touching axes", "[unit][editor][mcp][inspection]") {
        const InspectedBounds reference{17, {0, 0, 0}, {2, 2, 2}};
        const auto separated = MeasureInspectionDistance(reference, {42, {5, -6, 1}, {6, -4, 3}}, 0.5);
        CHECK(separated.gap == 5);
        CHECK((separated.nearestReference == std::array<double, 3>{2, 0, 1}));
        CHECK((separated.nearestTarget == std::array<double, 3>{5, -4, 1}));
        CHECK((separated.overlapDepth == std::array<double, 3>{0, 0, 1}));
        CHECK(separated.forwardAngleRadians == 0.5);
        const auto overlap = MeasureInspectionDistance(reference, {42, {1, 1, 1}, {3, 3, 3}}, 0);
        CHECK(overlap.gap == 0);
        CHECK(overlap.nearestReference == overlap.nearestTarget);
        CHECK((overlap.overlapDepth == std::array<double, 3>{1, 1, 1}));
        const auto contact = MeasureInspectionDistance(reference, {42, {2, 2, 2}, {4, 4, 4}}, 0);
        CHECK(contact.gap == 0);
        CHECK((contact.overlapDepth == std::array<double, 3>{0, 0, 0}));
    }

    TEST_CASE("Distance inspection requires an exact reference ID before dispatch", "[unit][editor][mcp][inspection]") {
        unsigned reads{};
        const auto endpoint = std::make_shared<const EditorInspectionCommandEndpoint>(
            EditorInspectionCommandEndpoint{.project = "project-one",
                                            .authorizationRevision = 3,
                                            .owner = std::this_thread::get_id(),
                                            .readDistances = [&](const ObjectInspectionRequest &request) {
            ++reads;
            REQUIRE(request.referenceObject == 17);
            const InspectedBounds reference{*request.referenceObject, {0, 0, 0}, {2, 2, 2}};
            const InspectedBounds target{request.objects.front(), {5, -6, 1}, {6, -4, 3}};
            return Result<DistanceInspectionPage>::Success(
                {.scene = {7, 19}, .records = {MeasureInspectionDistance(reference, target, 0.5)}});
        }});
        const EditorCommandMcpAdmission admission{"distance-reader", "project-one", 3, {std::string{EditorSceneInspectionGrant}}};
        const auto opened = EditorCommandMcpSession::Create({.inspection = endpoint}, admission, Test::Authorization());
        REQUIRE(opened.HasValue());
        const auto &session = *opened.Value();
        const auto denied = Execute(session, Command("missing-reference", "inspect.distances", {{"objects", {42}}}));
        CHECK(denied["status"] == "failed");
        CHECK(reads == 0);
        const auto result = Execute(session, Command("measure", "inspect.distances", {{"objects", {42}}, {"referenceObject", 17}}));
        REQUIRE(result["status"] == "succeeded");
        const auto &record = result["result"]["records"][0];
        CHECK(record["referenceObject"] == 17);
        CHECK(record["object"] == 42);
        CHECK(record["gap"] == 5);
        CHECK(record["forwardAngleRadians"] == 0.5);
        CHECK(reads == 1);
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
        auto session = EditorCommandMcpSession::Create({.test = endpoint},
                                                       {"configured-tests",
                                                        "project-one",
                                                        3,
                                                        {std::string{EditorTestExecutionGrant}, std::string{EditorTestObservationGrant}}},
                                                       Test::Authorization());
        REQUIRE(session.HasValue());
        auto &host = *session.Value();
        const auto client = host.Client();

        const auto start = Command("start", "editor.test.start");
        const auto denied = client->Call(start);
        REQUIRE(denied.HasError());
        CHECK(runner.calls.load(std::memory_order_acquire) == 0);
        Approve(host, start);
        const auto launched = Execute(host, start);
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
        const auto status = Execute(host, Command("observe", "editor.test.status", {{"testId", id}}));
        CHECK(status.at("status") == "succeeded");
        CHECK(status.at("result").at("testId") == id);
        CHECK(status.at("result").at("state") == "running");

        const auto cancel = Command("stop", "editor.test.cancel", {{"testId", id}});
        CHECK(client->Call(cancel).HasError());
        Approve(host, cancel);
        CHECK(Execute(host, cancel).at("result").at("cancellationRequested") == true);
        service.Close();
        const auto terminal = Execute(host, Command("terminal", "editor.test.status", {{"testId", id}}));
        CHECK(terminal.at("result").at("state") == "cancelled");
    }

    TEST_CASE("Editor command sessions bind grants and approvals to one active workspace", "[unit][editor][mcp]") {
        int presentations{};
        const auto source = std::make_shared<EditorSourceCommandEndpoint>(
            EditorSourceCommandEndpoint{"project-one", 3, std::this_thread::get_id(),
                                        [&](const SourceDocumentId &, std::optional<EditorSourceCommandPosition>, const auto &) {
            ++presentations;
            return Result<void>::Success();
        }});
        const EditorCommandMcpAdmission admission{"local-client", "project-one", 3, {std::string{EditorSourcePresentationGrant}}};
        const auto policy = std::make_shared<McpAuthorization>(std::make_shared<Test::Entropy>());
        CHECK(EditorCommandMcpSession::Create({.source = source}, admission, policy).HasError());
        REQUIRE(policy->SetTrust("project-one", 3, true).HasValue());
        CHECK(EditorCommandMcpSession::Create({}, admission, policy).HasError());
        auto foreign = admission;
        foreign.project = "other-project";
        CHECK(EditorCommandMcpSession::Create({.source = source}, foreign, policy).HasError());

        auto opened = EditorCommandMcpSession::Create({.source = source}, admission, policy);
        REQUIRE(opened.HasValue());
        auto &session = *opened.Value();
        const auto client = session.Client();
        const auto open = Command("request", "editor.source.open", {{"path", "main.cpp"}});
        CHECK(client->Call(open).HasError());
        const auto denied = session.RequestApproval(open);
        REQUIRE(denied.HasValue());
        CHECK(session.Decide(denied.Value(), false).HasValue());
        CHECK(session.Decide(denied.Value(), true).HasError());
        CHECK(client->Call(open).HasError());

        Approve(session, open);
        auto changed = open;
        changed.params["arguments"]["path"] = "other.cpp";
        CHECK(client->Call(changed).HasError());
        CHECK(Execute(session, open).at("status") == "succeeded");
        CHECK(client->Call(open).HasError());

        const auto queued = Command("queued", "editor.source.open", {{"path", "main.cpp"}});
        Approve(session, queued);
        REQUIRE(client->Call(queued).HasValue());
        REQUIRE(policy->SetTrust("project-one", 4, false).HasValue());
        REQUIRE(session.Pump().HasValue());
        CHECK(presentations == 1);
        CHECK(session.RequestApproval(queued).HasError());
        CHECK(session.Close().HasValue());
        CHECK(client->Call(queued).HasError());
    }

    /** @brief Owns the source leases published and revoked by the test workspace host. */
    struct SourceWorkspace {
        int presentations{};
        std::vector<std::uint64_t> revisions;
        std::shared_ptr<const EditorSourceCommandEndpoint> endpoint;
        const std::vector<std::string> grants{std::string{EditorSourcePresentationGrant}};
        EditorMcpWorkspaceHost host{std::make_shared<Test::Entropy>()};

        EditorWorkspaceCommandAccess Workspace(const std::string &project) {
            return EditorWorkspaceCommandAccess{
                .projectIdentity = project,
                .bind =
                    [this, project](const std::uint64_t revision) {
                revisions.push_back(revision);
                endpoint = std::make_shared<EditorSourceCommandEndpoint>(
                    EditorSourceCommandEndpoint{project, revision, std::this_thread::get_id(),
                                                [this](const SourceDocumentId &, std::optional<EditorSourceCommandPosition>, const auto &) {
                    ++presentations;
                    return Result<void>::Success();
                }});
                return EditorWorkspaceCommandEndpoints{.source = endpoint};
            },
                .revoke =
                    [this] {
                endpoint.reset();
            },
            };
        }
    };

    TEST_CASE_METHOD(SourceWorkspace, "The editor MCP host exposes and cancels the live request", "[unit][editor][mcp]") {
        CHECK_FALSE(host.ReadMcpStatus({}));
        CHECK(host.Open("operator", grants).HasError());
        REQUIRE(host.Attach(Workspace("first")).HasValue());
        CHECK(revisions.empty());
        auto old = host.Open("operator", grants);
        REQUIRE(old.HasValue());
        const auto initial = host.ReadMcpStatus({});
        REQUIRE(initial);
        CHECK(initial->capabilities == grants);
        CHECK(initial->requests.empty());
        CHECK_FALSE(host.ReadMcpStatus(initial->revision));
        CHECK(host.Open("second-client", grants).HasError());
        const auto command = Command("first-request", "editor.source.open", {{"path", "main.cpp"}});
        Approve(*host.ActiveSession(), command);
        const auto accepted = old.Value()->Call(command);
        REQUIRE(accepted.HasValue());
        const auto id = accepted.Value().at("operationId").get<std::uint64_t>();
        const auto active = host.ReadMcpStatus(initial->revision);
        REQUIRE(active);
        REQUIRE(active->requests.size() == 1);
        CHECK(active->requests.front().id == id);
        CHECK(active->requests.front().tool == "editor.source.open");
        CHECK_FALSE(active->requests.front().running);
        REQUIRE(host.CancelMcpRequest(active->revision.session, id).HasValue());
        const auto cancelled = old.Value()->Call({.id = "cancelled-status", .method = "operations/get", .params = {{"operationId", id}}});
        REQUIRE(cancelled.HasValue());
        CHECK(cancelled.Value().at("status") == "cancelled");
        const auto afterCancel = host.ReadMcpStatus(active->revision);
        REQUIRE(afterCancel);
        CHECK(afterCancel->requests.empty());
        REQUIRE(host.PumpWorkspaceCommands().HasValue());
        CHECK(presentations == 0);
    }

    TEST_CASE_METHOD(SourceWorkspace, "The editor MCP host retires command authority on a workspace switch", "[unit][editor][mcp]") {
        REQUIRE(host.Attach(Workspace("first")).HasValue());
        auto old = host.Open("operator", grants);
        REQUIRE(old.HasValue());
        const auto initial = host.ReadMcpStatus({});
        REQUIRE(initial);
        const auto command = Command("first-request", "editor.source.open", {{"path", "main.cpp"}});
        Approve(*host.ActiveSession(), command);
        REQUIRE(old.Value()->Call(command).HasValue());
        REQUIRE(host.Detach().HasValue());
        const auto retired = host.ReadMcpStatus(initial->revision);
        REQUIRE(retired);
        CHECK(retired->revision.session.principal == 0);
        CHECK_FALSE(endpoint);
        CHECK(old.Value()->Call(command).HasError());
        CHECK(presentations == 0);

        REQUIRE(host.Attach(Workspace("replacement")).HasValue());
        auto current = host.Open("operator", grants);
        REQUIRE(current.HasValue());
        REQUIRE(revisions.size() == 2);
        CHECK(revisions[1] > revisions[0]);
        CHECK(current.Value()->Call(command).HasError());
        Approve(*host.ActiveSession(), command);
        const auto replacement = current.Value()->Call(command);
        REQUIRE(replacement.HasValue());
        const auto replacementId = replacement.Value().at("operationId").get<std::uint64_t>();
        CHECK(host.CancelMcpRequest(initial->revision.session, replacementId).HasError());
        CHECK(host.RevokeMcpSession(initial->revision.session).HasError());
        REQUIRE(host.PumpWorkspaceCommands().HasValue());
        CHECK(presentations == 1);
        const auto live = host.ReadMcpStatus({});
        REQUIRE(live);
        REQUIRE(host.RevokeMcpSession(live->revision.session).HasValue());
        CHECK_FALSE(endpoint);
        CHECK(current.Value()->Call(command).HasError());
    }
}  // namespace
