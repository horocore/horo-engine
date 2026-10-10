#include "Horo/Application/CodeQueryStoreProviders.h"
#include "ProjectCodeQueryTestSupport.h"

#include <catch2/generators/catch_generators.hpp>

using namespace Horo;
using namespace Horo::Application;
namespace QueryTest = Horo::Test::CodeQuery;

TEST_CASE("Code query UTF-8 paging retains bytes and rejects cross-query continuation", "[unit][code-query]") {
    const auto service = QueryTest::Service(QueryTest::Text(std::make_shared<const std::string>("a\xc3\xa9z")));
    CodeQueryRequest request{.kind = CodeQueryKind::Text, .path = "src/a.cpp", .limit = 2};
    const auto first = service->Query(request, QueryTest::Context());
    REQUIRE(first.HasValue());
    CHECK(first.Value().text == "a");
    REQUIRE(first.Value().nextOffset == 1);
    request.offset = 1;
    REQUIRE(service->Query(request, QueryTest::Context()).HasError());
    request.expectedRevision = first.Value().revision;
    const auto next = service->Query(request, QueryTest::Context());
    REQUIRE(next.HasValue());
    CHECK(next.Value().text == "\xc3\xa9");
    request.path = "src/b.cpp";
    CHECK(service->Query(request, QueryTest::Context()).HasError());
    request.path = "src/a.cpp";
    request.offset = 2;
    CHECK(service->Query(request, QueryTest::Context()).HasError());
    request.offset = 1;
    request.limit = 1;
    CHECK(QueryTest::Matches(service->Query(request, QueryTest::Context()).ErrorValue(), CodeQueryErrors::Capacity));
    auto context = QueryTest::Context();
    context.projectGeneration = 4;
    CHECK(QueryTest::Matches(service->Query(request, context).ErrorValue(), CodeQueryErrors::Stale));
    const auto changed = QueryTest::Service(QueryTest::Text(std::make_shared<const std::string>("a\xc3\xa9z"), "text:2"));
    CHECK(QueryTest::Matches(changed->Query(request, QueryTest::Context()).ErrorValue(), CodeQueryErrors::Stale));
}

TEST_CASE("Code queries reject binary malformed and oversized existing snapshots", "[unit][code-query]") {
    const auto text = GENERATE(std::string{"x\0y", 3}, std::string{"\xff", 1}, std::string((1U << 20U) + 1, 'x'));
    const auto service = QueryTest::Service(QueryTest::Text(std::make_shared<const std::string>(text)));
    CHECK(service->Query({.kind = CodeQueryKind::Text, .path = "a.cpp"}, QueryTest::Context()).HasError());
}

TEST_CASE("Literal search has finite work and no regular expression execution", "[unit][code-query]") {
    const auto service = QueryTest::Service(QueryTest::Text(std::make_shared<const std::string>("(a+)+\n(a+)+")));
    const auto result = service->Query({.kind = CodeQueryKind::Search, .path = "a.cpp", .pattern = "(a+)+"}, QueryTest::Context());
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().records.size() == 2);
    CHECK(result.Value().records[0].byteOffset == 0);
    CHECK(result.Value().records[1].line == 2);
    CodeQueryLimits limits;
    limits.maximumSearchSteps = 10;
    const auto bounded = QueryTest::Service(QueryTest::Text(std::make_shared<const std::string>(100, 'a')), limits);
    const auto exhausted = bounded->Query({.kind = CodeQueryKind::Search, .path = "a.cpp", .pattern = "aaaaab"}, QueryTest::Context());
    REQUIRE(exhausted.HasError());
    CHECK(QueryTest::Matches(exhausted.ErrorValue(), CodeQueryErrors::Capacity));
}

TEST_CASE("Query admission and cancellation precede provider invocation", "[unit][code-query]") {
    unsigned calls{};
    CodeQueryProviders providers;
    providers.symbols = [&](const auto &, const auto &) {
        ++calls;
        return Result<CodeQueryObservation>::Failure(MakeError(CodeQueryErrors::Unavailable));
    };
    const auto service = QueryTest::Service(std::move(providers));
    for (const auto &path : {"../a", "/a", "a//b", "a/../b", "a\\b", "NUL.cpp", "a:b"})
        CHECK(service->Query({.kind = CodeQueryKind::Symbols, .path = path}, QueryTest::Context()).HasError());
    auto context = QueryTest::Context();
    context.authorityStopped = [] {
        return true;
    };
    CHECK(service->Query({.kind = CodeQueryKind::Symbols, .path = "a.cpp"}, context).HasError());
    context = QueryTest::Context();
    context.deadline = std::chrono::steady_clock::now();
    CHECK(service->Query({.kind = CodeQueryKind::Symbols, .path = "a.cpp"}, context).HasError());
    CHECK(calls == 0);
    REQUIRE(service->Query({.kind = CodeQueryKind::Symbols, .path = "a.cpp"}, QueryTest::Context()).HasError());
    CHECK(calls == 1);
    CHECK(QueryTest::Matches(service->Query({.kind = CodeQueryKind::TestStatus}, QueryTest::Context()).ErrorValue(),
                             CodeQueryErrors::Unavailable));
    CodeQueryLimits invalid;
    invalid.maximumFiles = 4097;
    CHECK(ProjectCodeQuery::Create({}, "project-one", 3, {}, invalid).HasError());
    for (const auto duration : {std::chrono::milliseconds{0}, std::chrono::milliseconds{-1}, std::chrono::milliseconds{1001}}) {
        invalid = {};
        invalid.maximumDuration = duration;
        CHECK(ProjectCodeQuery::Create({}, "project-one", 3, {}, invalid).HasError());
    }
}

TEST_CASE("Producer pages fence revisions validate provenance and own sorted results", "[unit][code-query]") {
    CodeQueryObservation observed{"project-one",
                                  3,
                                  "symbols:1",
                                  {{CodeQueryRecordKind::Symbol, "a.cpp", "Z", 2, 1}, {CodeQueryRecordKind::Symbol, "a.cpp", "A", 1, 1}}};
    CodeQueryProviders providers;
    providers.symbols = [&](const auto &, const auto &) {
        return Result<CodeQueryObservation>::Success(observed);
    };
    const auto service = QueryTest::Service(std::move(providers));
    CodeQueryRequest request{.kind = CodeQueryKind::Symbols, .path = "a.cpp", .limit = 1};
    const auto first = service->Query(request, QueryTest::Context());
    REQUIRE(first.HasValue());
    CHECK(first.Value().records[0].label == "A");
    request.offset = 1;
    request.expectedRevision = first.Value().revision;
    REQUIRE(service->Query(request, QueryTest::Context()).HasValue());
    observed.revision = "symbols:2";
    CHECK(QueryTest::Matches(service->Query(request, QueryTest::Context()).ErrorValue(), CodeQueryErrors::Stale));
    CHECK(first.Value().records[0].label == "A");
    request.offset = 0;
    request.expectedRevision.reset();
    observed.projectIdentity = "foreign";
    CHECK(QueryTest::Matches(service->Query(request, QueryTest::Context()).ErrorValue(), CodeQueryErrors::Stale));
    observed.projectIdentity = "project-one";
    observed.records[0].path = "../secret";
    CHECK(service->Query(request, QueryTest::Context()).HasError());
    observed.records.assign(4097, {});
    CHECK(QueryTest::Matches(service->Query(request, QueryTest::Context()).ErrorValue(), CodeQueryErrors::Capacity));
}

TEST_CASE("Existing build authorities provide bounded diagnostics and typed build status only", "[unit][code-query]") {
    QueryTest::Directory directory;
    const auto diagnostics = std::make_shared<BuildOutputStore>(2);
    BuildOutputRecord record;
    record.message = "compiler warning";
    record.code = DiagnosticCode{"compiler.warning"};
    record.source = DiagnosticSourceLocation{(directory.root / "a.cpp").string(), 2, 3};
    diagnostics->Append(record);
    record.source = DiagnosticSourceLocation{(directory.root.parent_path() / "secret.cpp").string(), 2, 3};
    diagnostics->Append(record);
    const auto operations = std::make_shared<OperationStore>(2, 2);
    REQUIRE(operations->Begin({.kind = OperationKind::Build, .title = "compile"}));
    REQUIRE(operations->Begin({.kind = OperationKind::Other, .title = "test"}));
    const auto service = QueryTest::Service(MakeCodeQueryStoreProviders(directory.root, "project-one", 3, diagnostics, operations));
    const auto result = service->Query({.kind = CodeQueryKind::Diagnostics}, QueryTest::Context());
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().records.size() == 2);
    CHECK(result.Value().records[0].path.empty());
    CHECK(result.Value().records[0].line == 0);
    CHECK(result.Value().records[1].path == "a.cpp");
    const auto builds = service->Query({.kind = CodeQueryKind::BuildStatus}, QueryTest::Context());
    REQUIRE(builds.HasValue());
    CHECK(builds.Value().records.size() == 1);
    CHECK(service->Query({.kind = CodeQueryKind::TestStatus}, QueryTest::Context()).HasError());
}

TEST_CASE("File pages and injected test status are bounded revisioned observations", "[unit][code-query]") {
    QueryTest::Directory directory;
    directory.Write("a.cpp", "a");
    directory.Write("b.cpp", "b");
    CodeQueryProviders providers;
    providers.testStatus = [](const auto &, const CodeQueryContext &context) {
        return Result<CodeQueryObservation>::Success(
            {context.projectIdentity, context.projectGeneration, "test:1", {{CodeQueryRecordKind::Test, {}, "succeeded", 0, 0, 7}}, 2});
    };
    auto service = ProjectCodeQuery::Create(directory.Files(), "project-one", 3, std::move(providers));
    REQUIRE(service.HasValue());
    CodeQueryRequest request{.kind = CodeQueryKind::Files, .limit = 1};
    const auto first = service.Value()->Query(request, QueryTest::Context());
    REQUIRE(first.HasValue());
    CHECK(first.Value().records[0].path == "a.cpp");
    request.offset = 1;
    request.expectedRevision = first.Value().revision;
    REQUIRE(service.Value()->Query(request, QueryTest::Context()).HasValue());
    directory.Write("c.cpp", "c");
    CHECK(QueryTest::Matches(service.Value()->Query(request, QueryTest::Context()).ErrorValue(), CodeQueryErrors::Stale));
    const auto status = service.Value()->Query({.kind = CodeQueryKind::TestStatus}, QueryTest::Context());
    REQUIRE(status.HasValue());
    CHECK(status.Value().records[0].label == "succeeded");
    CHECK(status.Value().droppedRecords == 2);
}
