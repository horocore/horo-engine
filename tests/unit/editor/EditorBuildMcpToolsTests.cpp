#include "../mcp/McpAuthorizationTestSupport.h"
#include "EditorBuildMcpTools.h"
#include "Horo/Mcp/McpErrors.h"

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Mcp;
    using Json = nlohmann::json;

    struct BuildFixture final {
        Application::GameplayBuildSnapshot build;
        unsigned starts{};
        unsigned cancels{};
        std::shared_ptr<EditorBuildCommandEndpoint> endpoint;
        std::shared_ptr<McpAuthorization> policy{Test::Authorization()};
        McpToolRegistry registry{policy};
        std::vector<std::string> grants{std::string{EditorBuildExecutionGrant}, std::string{EditorBuildObservationGrant}};
        std::uint64_t revision{};

        BuildFixture() {
            endpoint = std::make_shared<EditorBuildCommandEndpoint>(
                EditorBuildCommandEndpoint{"project-one", 3, std::this_thread::get_id(), [this](const std::function<bool()> &stop) {
                CHECK_FALSE(stop());
                ++starts;
                build = {.id = 42, .state = Application::GameplayBuildState::Building, .operationId = 7};
                return Result<EditorBuildCommandEndpoint::Id>::Success(build.id);
            }, [this](auto id) {
                return id == build.id ? std::optional{build} : std::nullopt;
            }, [this](auto id) {
                CHECK(id == build.id);
                ++cancels;
                build.cancellationRequested = true;
                return true;
            }});
            auto tools = MakeEditorBuildMcpTools(endpoint);
            REQUIRE(tools.HasValue());
            REQUIRE(starts == 0);
            auto published = registry.Publish(std::move(tools).Value(), grants);
            REQUIRE(published.HasValue());
            revision = published.Value();
        }

        McpRequestContext Context(const std::vector<std::string> &capabilities) const {
            return Test::Context({.clientIdentity = "editor-build-test",
                                  .capabilities = capabilities,
                                  .projectIdentity = "project-one",
                                  .authorizationRevision = 3,
                                  .registryRevision = revision},
                                 policy);
        }

        Result<Json> Invoke(const McpToolId &id, const Json &args, const McpRequestContext &context, const bool approve = false) const {
            if (approve) {
                const auto request =
                    McpRequest{.id = context.requestIdentity, .method = "tools/call", .params = {{"name", id.value}, {"arguments", args}}};
                const auto challenge = policy->Challenge(context, request, std::chrono::minutes{1});
                REQUIRE(challenge.HasValue());
                REQUIRE(policy->Decide(challenge.Value(), true).HasValue());
            }
            return registry.Read()->Invoke(id, args, context);
        }
    };

    TEST_CASE("Editor build tools separate observations from approved execution", "[unit][mcp][editor][build]") {
        BuildFixture fixture;
        const McpToolId start{"editor.build.start"}, status{"editor.build.status"}, cancel{"editor.build.cancel"};
        const Json empty = Json::object();
        const Json active{{"buildId", "42"}};
        const auto context = fixture.Context(fixture.grants);
        const auto observation = fixture.Context({std::string{EditorBuildObservationGrant}});
        REQUIRE(fixture.registry.Read()->Effect(status, fixture.grants).Value() == McpToolEffect::Query);
        REQUIRE(fixture.registry.Read()->Effect(start, fixture.grants).Value() == McpToolEffect::Mutation);
        CHECK(fixture.Invoke(start, empty, observation).HasError());
        CHECK(fixture.Invoke(start, {{"command", "cmake"}}, context, true).HasError());
        CHECK(fixture.Invoke(start, empty, context).HasError());
        CHECK(fixture.starts == 0);

        const auto started = fixture.Invoke(start, empty, context, true);
        REQUIRE(started.HasValue());
        CHECK(started.Value().at("buildId") == "42");
        CHECK(started.Value().at("operationId") == "7");
        CHECK(started.Value().at("state") == "building");
        CHECK(fixture.Invoke(status, active, observation).HasError());
        CHECK(fixture.Invoke(status, active, context).HasValue());
        CHECK(fixture.Invoke(cancel, active, context).HasError());
        CHECK(fixture.cancels == 0);
        const auto cancelled = fixture.Invoke(cancel, active, context, true);
        REQUIRE(cancelled.HasValue());
        CHECK(cancelled.Value().at("cancellationRequested") == true);
        CHECK(fixture.starts == 1);
        CHECK(fixture.cancels == 1);
    }

    TEST_CASE("An editor build status has no native path and cannot outlive the workspace", "[unit][mcp][editor][build]") {
        BuildFixture fixture;
        const auto context = fixture.Context(fixture.grants);
        REQUIRE(fixture.Invoke({"editor.build.start"}, Json::object(), context, true).HasValue());
        fixture.build.state = Application::GameplayBuildState::Failed;
        fixture.build.error = MakeError(McpErrors::ControllerFailed, "C:/private/secret.cpp");
        const Json input{{"buildId", "42"}};
        const auto failure = fixture.Invoke({"editor.build.status"}, input, context);
        REQUIRE(failure.HasValue());
        CHECK(failure.Value().at("state") == "failed");
        CHECK(failure.Value().at("error").is_object());
        CHECK(failure.Value().dump().find("secret.cpp") == std::string::npos);
        fixture.endpoint.reset();
        CHECK(fixture.Invoke({"editor.build.status"}, input, context).HasError());
    }
}  // namespace
