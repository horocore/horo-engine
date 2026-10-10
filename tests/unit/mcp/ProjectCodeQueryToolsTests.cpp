#include "Horo/Mcp/ProjectCodeQueryTools.h"
#include "McpAuthorizationTestSupport.h"
#include "ProjectCodeQueryTestSupport.h"

using namespace Horo;
using namespace Horo::Mcp;
using namespace Horo::Application;
namespace QueryTest = Horo::Test::CodeQuery;
namespace McpTest = Horo::Mcp::Test;

TEST_CASE("Project query pack is inert Query metadata with seven grant-filtered tools", "[unit][mcp][code-query]") {
    unsigned generations{};
    const auto service = QueryTest::Service(QueryTest::Text(std::make_shared<const std::string>("owned source")));
    auto tools = MakeProjectCodeQueryTools(service, "project-one", 3, [&] {
        ++generations;
        return 3;
    });
    REQUIRE(tools.HasValue());
    REQUIRE(tools.Value().size() == 7);
    CHECK(generations == 0);
    for (const auto &tool : tools.Value()) {
        CHECK(tool.owner == McpOwnerContext::Background);
        CHECK(tool.descriptor.effect == McpToolEffect::Query);
    }
    McpToolRegistry registry{McpTest::Authorization()};
    const std::vector<std::string> grants{"horo.project.code.query"};
    REQUIRE(registry.Publish(std::move(tools).Value(), grants).HasValue());
    CHECK(generations == 0);
    CHECK(registry.Read()->Discover({}).empty());
    CHECK(registry.Read()->Discover(grants).size() == 7);
    const auto context = McpTest::Context(
        {.clientIdentity = "query-client", .capabilities = grants, .projectIdentity = "project-one", .authorizationRevision = 3});
    const auto result = registry.Read()->Invoke({"project.text"}, {{"path", "a.cpp"}}, context);
    REQUIRE(result.HasValue());
    CHECK(result.Value().at("text") == "owned source");
    CHECK(result.Value().at("projectGeneration") == 3);
    CHECK(registry.Read()->Invoke({"project.text"}, {{"path", "a.cpp"}, {"root", "/outside"}}, context).HasError());
    CHECK(registry.Read()->Invoke({"project.search"}, {{"path", "a.cpp"}, {"pattern", std::string(129, 'a')}}, context).HasError());
    CHECK(registry.Read()->Invoke({"project.test_status"}, nlohmann::json::object(), context).HasError());
    const auto denied = McpTest::Context({.clientIdentity = "denied", .projectIdentity = "project-one", .authorizationRevision = 3});
    CHECK(registry.Read()->Invoke({"project.text"}, {{"path", "a.cpp"}}, denied).HasError());
}

TEST_CASE("Retained project query registrations reject generation changes before and during reads", "[unit][mcp][code-query]") {
    std::uint64_t generation{3};
    bool changeDuringRead{};
    auto providers = QueryTest::Text(std::make_shared<const std::string>("owned"));
    const auto captureText = providers.existingText;
    providers.existingText = [&](const auto path, const auto &context) {
        if (changeDuringRead)
            ++generation;
        return captureText(path, context);
    };
    auto tools = MakeProjectCodeQueryTools(QueryTest::Service(std::move(providers)), "project-one", 3, [&] {
        return generation;
    });
    REQUIRE(tools.HasValue());
    McpToolRegistry registry{McpTest::Authorization()};
    const std::vector<std::string> grants{"horo.project.code.query"};
    REQUIRE(registry.Publish(std::move(tools).Value(), grants).HasValue());
    const auto context = McpTest::Context(
        {.clientIdentity = "generation-client", .capabilities = grants, .projectIdentity = "project-one", .authorizationRevision = 3});
    generation = 4;
    CHECK(QueryTest::Matches(registry.Read()->Invoke({"project.text"}, {{"path", "a.cpp"}}, context).ErrorValue(), CodeQueryErrors::Stale));
    generation = 3;
    changeDuringRead = true;
    CHECK(QueryTest::Matches(registry.Read()->Invoke({"project.text"}, {{"path", "a.cpp"}}, context).ErrorValue(), CodeQueryErrors::Stale));
}

TEST_CASE("Query pack rejects absent generation authority and invalid owner composition", "[unit][mcp][code-query]") {
    const auto service = QueryTest::Service();
    CHECK(MakeProjectCodeQueryTools(service, "project-one", 3, {}).HasError());
    CHECK(MakeProjectCodeQueryTools(service, "project-one", 3,
                                    [] {
        return 3;
    }, McpOwnerContext::Runtime)
              .HasError());
}
