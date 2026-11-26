#include "../../../apps/HoroEditor/app/ImmersiveAuthoringContext.h"
#include "AgentContext.h"
#include "AgentTurnMcp.h"
#include "Horo/Mcp/McpController.h"
#include "Horo/Mcp/ProjectCodeQueryTools.h"
#include "McpAuthorizationTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <condition_variable>
#include <future>
#include <mutex>

namespace Horo::Agent {
    namespace {
        using namespace Application;
        using namespace Security;

        Platform::ProjectReadContext Project() {
            return {"project", 4, {}, std::chrono::steady_clock::now() + std::chrono::seconds{10}, {}};
        }

        AgentContextReaders Readers() {
            AgentContextReaders readers;
            readers.isCurrent = [](const auto &) {
                return true;
            };
            readers.redact = [](const auto &, std::string_view text) {
                std::string result{text};
                if (const auto start = result.find("secret"); start != std::string::npos)
                    result.replace(start, 6, "<redacted>");
                return Result<std::string>::Success(std::move(result));
            };
            return readers;
        }

        struct RecordingModel final : IModelProvider {
            std::vector<ModelMessage> sent;
            std::function<void(std::stop_token)> beforeResponse;

            ProviderDiscovery Discover(std::stop_token) override {
                return {};
            }

            ModelOutcome Stream(const ModelRequest &request, const ModelEventSink &sink, std::stop_token stop) override {
                sent = request.messages;
                if (beforeResponse)
                    beforeResponse(stop);
                if (!sink({.kind = ModelEventKind::TextDelta, .text = "Result"}) || !sink({.kind = ModelEventKind::Completed}))
                    return {ModelError{ModelErrorCode::Cancelled, "Stopped by host", {}}};
                return {};
            }
        };
    }  // namespace

    TEST_CASE("Agent context dispatch sends exactly the consented redacted preview", "[agent][context]") {
        using enum AgentContextAttachmentKind;
        using enum AgentProviderResidence;
        const auto project = Project();
        auto readers = Readers();
        std::size_t sourceReads{};
        std::size_t queryReads{};
        readers.existingText = [&](auto path, const auto &context) {
            ++sourceReads;
            using Snapshot = std::optional<Platform::ProjectReadTextSnapshot>;
            return Result<Snapshot>::Success(
                Platform::ProjectReadTextSnapshot{context.projectIdentity, context.projectGeneration, std::string{path}, "r1",
                                                  std::make_shared<const std::string>("prefix secret suffix")});
        };
        readers.readOnlyQuery = [&](const auto &query, const auto &context) {
            ++queryReads;
            CHECK(query.kind == CodeQueryKind::Diagnostics);
            CHECK(query.expectedRevision == "r2");
            return Result<CodeQueryPage>::Success({.revision = "r2",
                                                   .projectGeneration = context.projectGeneration,
                                                   .records = {{CodeQueryRecordKind::Diagnostic, "src/test.cpp", "warning", 1, 2}},
                                                   .nextOffset = 1});
        };
        const std::array<AgentContextSelection, 3> selected{AgentContextTextRequest{AgentContextKind::SourceRange, "src/test.cpp", "r1", 7,
                                                                                    6},
                                                            AgentContextQueryRequest{"r2",
                                                                                     {.kind = CodeQueryKind::Diagnostics, .limit = 1}},
                                                            AgentContextAttachment{Selection, "scene selection", "r3", "mesh, light"}};
        auto capture = AgentContextPreview::Capture(selected, readers, project, Cloud, 21, 8);
        REQUIRE(capture.HasValue());
        const auto &preview = capture.Value();
        REQUIRE(preview.Items().size() == 3);
        CHECK(preview.Provider() == Cloud);
        CHECK(preview.Items()[0].provenance.offset == 7);
        CHECK(preview.Items()[0].provenance.capturedBytes == 6);
        CHECK(preview.Items()[0].candidate.text.find("<redacted>") != std::string::npos);
        CHECK(preview.Items()[0].candidate.text.find("prefix") == std::string::npos);
        CHECK(preview.Items()[1].provenance.truncated);
        CHECK(preview.Items()[2].provenance.kind == AgentContextKind::Selection);

        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(true, 8));
        const ModelMessage original{.role = ModelRole::User, .text = "Summarize the selection"};
        ModelRequest request{.model = "fake", .messages = {original}};
        CHECK_FALSE(preview.InsertBeforeDispatch(policy, project, readers.isCurrent, Cloud, 21, request).Allowed());
        REQUIRE(request.messages.size() == 1);
        REQUIRE(preview.Grant(policy).Allowed());
        CHECK_FALSE(preview.InsertBeforeDispatch(policy, project, readers.isCurrent, Local, 21, request).Allowed());
        CHECK_FALSE(preview.InsertBeforeDispatch(policy, project, readers.isCurrent, Cloud, 22, request).Allowed());
        REQUIRE(preview.InsertBeforeDispatch(policy, project, readers.isCurrent, Cloud, 21, request).Allowed());
        REQUIRE(request.messages.size() == 4);
        for (std::size_t i = 0; i < preview.Items().size(); ++i) {
            CHECK(request.messages[i].role == ModelRole::User);
            CHECK(request.messages[i].text == preview.Items()[i].candidate.text);
        }
        CHECK(request.messages.back().text == original.text);
        CHECK_FALSE(preview.InsertBeforeDispatch(policy, project, readers.isCurrent, Cloud, 21, request).Allowed());
        CHECK(sourceReads == 1);
        CHECK(queryReads == 1);
    }

    TEST_CASE("Agent context captures only safe current textual selections", "[agent][context]") {
        using enum AgentProviderResidence;
        using enum AgentContextKind;
        const auto project = Project();
        auto readers = Readers();
        std::size_t reads{};
        std::optional<std::string> bytes{std::string{"a\xC3\xA7z"}};
        readers.existingText = [&](auto path, const auto &context) {
            ++reads;
            using Snapshot = std::optional<Platform::ProjectReadTextSnapshot>;
            if (!bytes)
                return Result<Snapshot>::Success(std::nullopt);
            return Result<Snapshot>::Success(Platform::ProjectReadTextSnapshot{context.projectIdentity, context.projectGeneration,
                                                                               std::string{path}, "fresh",
                                                                               std::make_shared<const std::string>(*bytes)});
        };
        const auto capture = [&](AgentContextSelection chosen) {
            return AgentContextPreview::Capture(std::span{&chosen, 1}, readers, project, Local, 1, 9);
        };
        CHECK(capture(AgentContextTextRequest{File, "../secret", "fresh"}).HasError());
        CHECK(reads == 0);
        CHECK(capture(AgentContextTextRequest{File, "src/app.cpp", "stale"}).HasError());
        CHECK(capture(AgentContextTextRequest{SourceRange, "src/app.cpp", "fresh", 2, 1}).HasError());
        CHECK(capture(AgentContextTextRequest{SourceRange, "src/app.cpp", "fresh", 1, 2}).HasValue());
        bytes = std::string{"a\0b", 3};
        CHECK(capture(AgentContextTextRequest{File, "src/app.cpp", "fresh"}).HasError());
        bytes = std::string(4097, 'x');
        CHECK(capture(AgentContextTextRequest{File, "src/app.cpp", "fresh"}).HasError());
        bytes.reset();
        CHECK(capture(AgentContextTextRequest{SourceRange, "src/app.cpp", "fresh", 1, 2}).HasError());
        CHECK(capture(AgentContextQueryRequest{"fresh", {.kind = CodeQueryKind::Files, .limit = 1}}).HasError());
    }

    TEST_CASE("Agent context revokes consent when the source changes after preview", "[agent][context]") {
        using enum AgentProviderResidence;
        const auto project = Project();
        auto readers = Readers();
        const std::array<AgentContextSelection, 1> selected{
            AgentContextAttachment{AgentContextAttachmentKind::Report, "report", "r1", "evidence"}};
        const auto capture = AgentContextPreview::Capture(selected, readers, project, Local, 42, 2);
        REQUIRE(capture.HasValue());
        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(false, 2));
        REQUIRE(capture.Value().Grant(policy).Allowed());
        ModelRequest request{.model = "fake", .messages = {{.role = ModelRole::User, .text = "Explain"}}};
        const auto stale = [](const auto &) {
            return false;
        };
        CHECK(capture.Value().InsertBeforeDispatch(policy, project, stale, Local, 42, request).reason == AgentPolicyReason::StaleAuthority);
        REQUIRE(request.messages.size() == 1);
        CHECK_FALSE(capture.Value().InsertBeforeDispatch(policy, project, readers.isCurrent, Local, 42, request).Allowed());
    }

    TEST_CASE("Agent context dynamic diagnostics use the authenticated read-only MCP query", "[agent][context][mcp]") {
        using namespace std::chrono_literals;
        constexpr std::uint64_t generation = 3;
        constexpr std::string_view project = "project-one";
        unsigned reads{};
        CodeQueryProviders providers;
        providers.diagnostics = [&](const CodeQueryRequest &, const CodeQueryContext &) {
            ++reads;
            return Result<CodeQueryObservation>::Success({std::string{project},
                                                          generation,
                                                          "current diagnostics",
                                                          {{CodeQueryRecordKind::Diagnostic, "src/main.cpp", "warning", 5, 2}}});
        };
        auto capability = ProjectCodeQuery::Create({}, std::string{project}, generation, std::move(providers));
        REQUIRE(capability.HasValue());
        const auto auth = Mcp::Test::Authorization();
        auto registry = std::make_shared<Mcp::McpToolRegistry>(auth);
        auto tools = Mcp::MakeProjectCodeQueryTools(std::move(capability).Value(), std::string{project}, generation, [] {
            return generation;
        }, Mcp::McpOwnerContext::Editor);
        REQUIRE(tools.HasValue());
        const std::vector<std::string> grants{"horo.project.code.query"};
        const auto revision = registry->Publish(std::move(tools).Value(), grants);
        REQUIRE(revision.HasValue());
        const auto controller = Mcp::McpController::Create(registry, {}, auth);
        REQUIRE(controller.HasValue());
        REQUIRE(controller.Value()->BindOwner(Mcp::McpOwnerContext::Editor).HasValue());
        auto manager = Mcp::McpSessionManager::Create(controller.Value(), {}, auth);
        REQUIRE(manager.HasValue());
        auto session =
            Mcp::McpInProcessAdapter::Start(std::move(manager).Value(), Mcp::Test::Authenticate({.clientIdentity = "agent-context-test",
                                                                                                 .capabilities = grants,
                                                                                                 .projectIdentity = std::string{project},
                                                                                                 .authorizationRevision = generation,
                                                                                                 .registryRevision = revision.Value()},
                                                                                                auth));
        REQUIRE(session.HasValue());
        const AgentTurnMcp bridge{session.Value(), {std::string{project}, generation}};
        CodeQueryContext context{std::string{project}, generation, {}, std::chrono::steady_clock::now() + 2s};
        const CodeQueryRequest query{.kind = CodeQueryKind::Diagnostics, .path = "src/main.cpp", .limit = 2};
        auto foreign = context;
        foreign.projectIdentity = "different-project";
        CHECK(bridge.ReadContextQuery(31, 0, query, foreign).HasError());
        CHECK(bridge.ReadContextQuery(31, 0, {.kind = CodeQueryKind::Text, .limit = 2}, context).HasError());
        CHECK(reads == 0);
        auto pending = std::async(std::launch::async, [&] {
            return bridge.ReadContextQuery(31, 1, query, context);
        });
        while (pending.wait_for(1ms) != std::future_status::ready)
            REQUIRE(controller.Value()->Pump(Mcp::McpOwnerContext::Editor).HasValue());
        const auto result = pending.get();
        REQUIRE(result.HasValue());
        REQUIRE(reads == 1);
        REQUIRE(result.Value().records.size() == 1);
        CHECK(result.Value().records.front().label == "warning");
        CHECK(result.Value().projectGeneration == generation);

        auto readers = Readers();
        readers.readOnlyQuery = [&](const auto &request, const auto &) {
            REQUIRE(request.expectedRevision == result.Value().revision);
            return result;
        };
        const std::array<AgentContextSelection, 1> chosen{AgentContextQueryRequest{result.Value().revision, query}};
        const auto preview = AgentContextPreview::Capture(chosen, readers, context, AgentProviderResidence::Local, 31, generation);
        REQUIRE(preview.HasValue());
        CHECK(preview.Value().Items().front().candidate.text.find("src/main.cpp 5:2 warning") != std::string::npos);
        CHECK(reads == 1);
    }

    TEST_CASE("Agent runtime dispatches admitted context without publishing it into history", "[agent][context]") {
        RecordingModel model;

        const auto project = Project();
        const auto readers = Readers();
        const std::array<AgentContextSelection, 1> selection{
            AgentContextAttachment{AgentContextAttachmentKind::BuildOutput, "compiler", "r3", "selected build log"}};
        const auto preview = AgentContextPreview::Capture(selection, readers, project, AgentProviderResidence::Local, 25, 4);
        REQUIRE(preview.HasValue());
        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(true, 4));
        const AgentTurnContext context{preview.Value(), policy, project, readers.isCurrent, AgentProviderResidence::Local};
        const ModelRequest request{.model = "fake", .messages = {{.role = ModelRole::User, .text = "Explain this log"}}};
        AgentTurnRuntime runtime;
        const auto run = [&] {
            return runtime.Run(model, nullptr, 25, request, {}, {}, &context);
        };
        CHECK(run().status == AgentTurnStatus::ContextDenied);
        CHECK(model.sent.empty());
        REQUIRE(preview.Value().Grant(policy).Allowed());
        const auto result = run();
        REQUIRE(result.status == AgentTurnStatus::Completed);
        REQUIRE(model.sent.size() == 2);
        CHECK(model.sent.front().text == preview.Value().Items().front().candidate.text);
        CHECK(model.sent.front().role == ModelRole::User);
        REQUIRE(result.messages.size() == 2);
        CHECK(result.messages.front().text == request.messages.front().text);
        CHECK(result.messages.back().text == "Result");
    }

    TEST_CASE("A provider cannot publish a response after its approved source revision expires", "[agent][context]") {
        struct LateModel final : IModelProvider {
            bool sourceCurrent{true};
            bool revokeBeforeText{};
            bool acceptedLateText{};

            ProviderDiscovery Discover(std::stop_token) override {
                return {};
            }

            ModelOutcome Stream(const ModelRequest &, const ModelEventSink &sink, std::stop_token) override {
                sourceCurrent = !revokeBeforeText;
                acceptedLateText = sink({.kind = ModelEventKind::TextDelta, .text = "Response with approved context"});
                static_cast<void>(sink({.kind = ModelEventKind::Completed}));
                sourceCurrent = false;
                return {};
            }
        };

        LateModel model;
        model.revokeBeforeText = GENERATE(false, true);
        const auto project = Project();
        auto readers = Readers();
        readers.isCurrent = [&model](const auto &) {
            return model.sourceCurrent;
        };
        const std::array<AgentContextSelection, 1> selection{
            AgentContextAttachment{AgentContextAttachmentKind::BuildOutput, "compiler", "r3", "selected build log"}};
        const auto preview = AgentContextPreview::Capture(selection, readers, project, AgentProviderResidence::Local, 25, 4);
        REQUIRE(preview.HasValue());
        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(true, 4));
        REQUIRE(preview.Value().Grant(policy).Allowed());
        const AgentTurnContext context{preview.Value(), policy, project, readers.isCurrent, AgentProviderResidence::Local};
        const ModelRequest request{.model = "fake", .messages = {{.role = ModelRole::User, .text = "Explain this log"}}};
        AgentTurnRuntime runtime;
        unsigned presented{};
        const auto result = runtime.Run(model, nullptr, 25, request, {}, {}, &context, [&presented](std::string_view) {
            ++presented;
        });
        CHECK(result.status == AgentTurnStatus::ContextDenied);
        CHECK(result.contextError == AgentPolicyReason::StaleAuthority);
        CHECK(result.messages.empty());
        CHECK_FALSE(result.providerError.has_value());
        CHECK(model.acceptedLateText == !model.revokeBeforeText);
        CHECK(presented == (model.revokeBeforeText ? 0 : 1));
    }

    TEST_CASE("Immersive planning dispatches only project-matched consented context", "[agent][context][plan]") {
        using enum AgentTurnStatus;
        CancellationSource projectLifetime;
        auto project = Project();
        project.cancellation = projectLifetime.Token();
        const auto readers = Readers();
        const std::array<AgentContextSelection, 1> selection{
            AgentContextAttachment{AgentContextAttachmentKind::BuildOutput, "compiler", "r3", "selected secret build log"}};
        const auto preview = AgentContextPreview::Capture(selection, readers, project, AgentProviderResidence::Local, 25, 4);
        REQUIRE(preview.HasValue());
        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(true, 4));
        const AgentTurnContext approved{preview.Value(), policy, project, readers.isCurrent, AgentProviderResidence::Local};
        const XR::XRSessionId session{{XR::XRRuntimeGeneration::Create(1).Value(), {0, 1}}, {0, 1}};
        Editor::ImmersiveAuthoringContext owner;
        const auto context = owner.Publish({project.projectGeneration,
                                            1,
                                            1,
                                            session,
                                            {session, {0, 1}},
                                            1,
                                            1,
                                            1,
                                            XR::XRCapabilityRevision::Create(1).Value(),
                                            Editor::ImmersiveAuthoringMode::Edit,
                                            0,
                                            true},
                                           {});
        REQUIRE(context);
        RecordingModel model;
        const auto inspect = [](const ModelToolIntent &) {
            return AgentPlanToolInspection{ToolDenied};
        };
        const auto capture = [&](std::string_view identity) {
            return owner.CapturePlan(*context, {.model = "fake", .messages = {{.role = ModelRole::User, .text = "Plan the change"}}},
                                     identity);
        };
        auto pending = capture(project.projectIdentity);
        REQUIRE(pending);
        CHECK(pending->Plan(model, 25, {}, inspect, {}, &approved).Status() == ContextDenied);
        REQUIRE(preview.Value().Grant(policy).Allowed());
        auto foreign = capture("other-project");
        REQUIRE(foreign);
        CHECK(foreign->Plan(model, 25, {}, inspect, {}, &approved).Status() == ContextDenied);
        CHECK(model.sent.empty());

        auto correct = capture(project.projectIdentity);
        REQUIRE(correct);
        SECTION("changing trust stops a quiet provider within the same project opening") {
            bool observedStop{};
            model.beforeResponse = [&](std::stop_token stop) {
                REQUIRE(policy.SetProjectTrust(false, 5));
                std::mutex mutex;
                std::condition_variable_any wake;
                std::unique_lock lock{mutex};
                wake.wait_for(lock, stop, std::chrono::seconds{2}, [] { return false; });
                observedStop = stop.stop_requested();
            };
            const auto review = correct->Plan(model, 25, AgentTurnLimits{.timeout = std::chrono::seconds{5}}, inspect, {}, &approved);
            CHECK(observedStop);
            CHECK_FALSE(projectLifetime.Token().IsCancellationRequested());
            CHECK(context->IsCurrent());
            CHECK(review.Status() == ContextDenied);
            CHECK_FALSE(review.Read(*context));
            return;
        }
        const auto review = correct->Plan(model, 25, {}, inspect, {}, &approved);
        REQUIRE(review.Status() == Completed);
        REQUIRE(model.sent.size() == 2);
        CHECK(model.sent.front().text == preview.Value().Items().front().candidate.text);
        REQUIRE(review.Read(*context));
        CHECK(review.Read(*context)->draft->text == "Result");

        SECTION("closing the project retires the review") {
            projectLifetime.RequestCancellation();
            CHECK_FALSE(review.Read(*context));
        }
        SECTION("replacing trust retires the review within the same project opening") {
            CHECK_FALSE(policy.SetProjectTrust(false, 4));
            REQUIRE(review.Read(*context));
            REQUIRE(policy.SetProjectTrust(false, 5));
            CHECK_FALSE(projectLifetime.Token().IsCancellationRequested());
            CHECK(context->IsCurrent());
            CHECK(review.Status() == ContextDenied);
            CHECK_FALSE(review.Read(*context));
            REQUIRE(policy.SetProjectTrust(true, 6));
            CHECK_FALSE(review.Read(*context));
        }
    }
}  // namespace Horo::Agent
