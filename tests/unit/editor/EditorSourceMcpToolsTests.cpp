#include "../../support/OwnedTestDirectory.h"
#include "../mcp/McpAuthorizationTestSupport.h"
#include "EditorSourceMcpTools.h"
#include "Horo/Mcp/McpErrors.h"
#include "editor/screens/workspace/source/EditorSourcePresentation.h"

#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Mcp;
    using Json = nlohmann::json;

    struct Workspace final {
        Horo::Tests::OwnedTestDirectory directory{"horo-source-mcp"};
        SourceFileOpenService sources{directory.Path()};
        WorkspacePanelHost tabs;
        std::shared_ptr<const Language::LanguageSourceFocus> focus;
        std::shared_ptr<const EditorSourceCommandEndpoint> endpoint;
        std::shared_ptr<McpAuthorization> policy{Test::Authorization()};
        McpToolRegistry registry{policy};
        std::vector<std::string> grants{std::string{EditorSourcePresentationGrant}};
        std::uint64_t revision{};

        Workspace() {
            std::ofstream(directory.Path() / "example.lua", std::ios::binary) << "\xF0\x9F\x98\x80"
                                                                                 "x\r\n";
            tabs.AttachDocumentIdentityRegistry(sources.DocumentRegistry());
            endpoint = std::make_shared<EditorSourceCommandEndpoint>(
                EditorSourceCommandEndpoint{"project-one", 3, std::this_thread::get_id(),
                                            [this](const SourceDocumentId &path, std::optional<EditorSourceCommandPosition> target,
                                                   const std::function<bool()> &stop) {
                auto result = PresentEditorSource(sources, tabs, path, SourceCommandLocation(path, target), {}, stop);
                if (result.HasError())
                    return Result<void>::Failure(result.ErrorValue());
                focus = std::move(result).Value();
                return Result<void>::Success();
            }});
            auto tools = MakeEditorSourceMcpTools(endpoint);
            REQUIRE(tools.HasValue());
            auto published = registry.Publish(std::move(tools).Value(), grants);
            REQUIRE(published.HasValue());
            revision = published.Value();
        }

        McpRequestContext Context(std::string project = "project-one", std::uint64_t authorizationRevision = 3) const {
            return Test::Context({.clientIdentity = "editor-source-test",
                                  .capabilities = grants,
                                  .projectIdentity = std::move(project),
                                  .authorizationRevision = authorizationRevision,
                                  .registryRevision = revision},
                                 policy);
        }

        Result<Json> Invoke(const McpToolId &tool, const Json &arguments, const McpRequestContext &context, bool approved = true) const {
            if (approved) {
                auto challenge = policy->Challenge(context,
                                                   {.id = context.requestIdentity,
                                                    .method = "tools/call",
                                                    .params = {{"name", tool.value}, {"arguments", arguments}}},
                                                   std::chrono::minutes{1});
                REQUIRE(challenge.HasValue());
                REQUIRE(policy->Decide(challenge.Value(), true).HasValue());
            }
            return registry.Read()->Invoke(tool, arguments, context);
        }
    };

    TEST_CASE("MCP source navigation requires exact approval and preserves dirty editor text", "[unit][editor][mcp][source]") {
        Workspace workspace;
        const McpToolId tool{"editor.source.navigate"};
        const auto context = workspace.Context();
        CHECK(workspace.registry.Read()->Discover({}).empty());
        const auto effect = workspace.registry.Read()->Effect(tool, workspace.grants);
        REQUIRE(effect.HasValue());
        CHECK(effect.Value() == McpToolEffect::PresentationSideEffect);
        const Json valid{{"path", "example.lua"}, {"line", 1}, {"column", 3}};
        const Json splitSurrogate{{"path", "example.lua"}, {"line", 1}, {"column", 2}};
        const auto unapproved = workspace.Invoke(tool, valid, context, false);
        REQUIRE(unapproved.HasError());
        CHECK(unapproved.ErrorValue().code.Value() == McpErrors::ApprovalRequired.code.Value());
        CHECK(workspace.Invoke(tool, splitSurrogate, context).HasError());
        CHECK(workspace.tabs.DocumentTabs().empty());
        REQUIRE(workspace.Invoke(tool, valid, context).HasValue());
        REQUIRE(workspace.focus);
        CHECK(workspace.focus->selection == Language::LanguageByteSpan{4, 4});

        const auto source = workspace.focus->source;
        auto edited = workspace.sources.Documents().Edit(source.Identity().instance, {source.Revision(), 0, 4, "ab"});
        REQUIRE(edited.HasValue());
        REQUIRE(workspace.Invoke(tool, valid, context).HasValue());
        REQUIRE(workspace.focus);
        CHECK(workspace.focus->source.SharesStateWith(edited.Value()));
        CHECK(workspace.focus->selection == Language::LanguageByteSpan{2, 2});
        CHECK(workspace.tabs.DocumentTabs().size() == 1);
    }

    TEST_CASE("MCP embedded open rejects unsafe input and retired or foreign owners", "[unit][editor][mcp][source]") {
        Workspace workspace;
        const McpToolId tool{"editor.source.open"};
        const Json valid{{"path", "example.lua"}};
        const auto context = workspace.Context();
        CHECK(workspace.Invoke(tool, {{"path", "../outside.lua"}}, context).HasError());
        const auto missing = workspace.Invoke(tool, {{"path", "missing.lua"}}, context);
        REQUIRE(missing.HasError());
        CHECK(missing.ErrorValue().code.Value() == SourceOpenErrors::Missing.code.Value());
        CHECK(missing.ErrorValue().message.find("missing.lua") == std::string::npos);
        CHECK(workspace.Invoke(tool, valid, workspace.Context("first", 1)).HasError());
        CHECK(workspace.tabs.DocumentTabs().empty());
        REQUIRE(workspace.Invoke(tool, valid, context).HasValue());
        REQUIRE(workspace.tabs.DocumentTabs().size() == 1);
        CHECK_FALSE(workspace.focus);
        workspace.endpoint.reset();
        const auto retired = workspace.Invoke(tool, valid, context);
        REQUIRE(retired.HasError());
        CHECK(retired.ErrorValue().code.Value() == McpErrors::OwnerUnavailable.code.Value());
    }

    TEST_CASE("Source presentation retires the staged identity if its request stops before publishing", "[unit][editor][mcp][source]") {
        Workspace workspace;
        const auto path = SourceDocumentId::Parse("example.lua");
        REQUIRE(path.HasValue());
        unsigned checkpoints{};
        const auto result = PresentEditorSource(workspace.sources, workspace.tabs, path.Value(), {}, {}, [&] {
            return ++checkpoints == 2;
        });
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == SourceDocumentErrors::Cancelled.code.Value());
        CHECK(workspace.tabs.DocumentTabs().empty());
        CHECK_FALSE(workspace.sources.DocumentRegistry().Find({DocumentKind::Source, path.Value()}));
    }
}  // namespace
