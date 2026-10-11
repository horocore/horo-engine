#include "Horo/Editor/SourceDocumentCodeQuery.h"
#include "ProjectCodeQueryTestSupport.h"

#include <future>

using namespace Horo;
using namespace Horo::Editor;
using namespace Horo::Application;
namespace QueryTest = Horo::Test::CodeQuery;

TEST_CASE("Code queries reuse dirty source snapshots without opening documents or inspecting disk", "[unit][editor][code-query]") {
    QueryTest::Directory directory;
    directory.Write("a.cpp", "disk");
    const auto identities = std::make_shared<DocumentIdentityRegistry>();
    const auto documents = std::make_shared<SourceDocumentService>(directory.root);
    auto source = SourceDocumentId::Parse("a.cpp");
    REQUIRE(source.HasValue());
    const auto identity = identities->Open({DocumentKind::Source, std::move(source).Value()});
    REQUIRE(identity.HasValue());
    const auto opened = documents->Open(identity.Value().identity, directory.root / "a.cpp");
    REQUIRE(opened.HasValue());
    const auto edited = documents->Edit(identity.Value().identity.instance, {opened.Value().Revision(), 0, 4, "dirty"});
    REQUIRE(edited.HasValue());
    const auto service = QueryTest::Service(MakeSourceDocumentCodeQueryProviders(documents, identities, "project-one", 3));
    auto wrongThread = std::async(std::launch::async, [&] {
        return service->Query({.kind = CodeQueryKind::Text, .path = "a.cpp"}, QueryTest::Context());
    });
    const auto rejected = wrongThread.get();
    REQUIRE(rejected.HasError());
    CHECK(QueryTest::Matches(rejected.ErrorValue(), SourceDocumentErrors::WrongThread));
    // Removing the disk file proves the existing-session query performs no disk read.
    std::filesystem::remove(directory.root / "a.cpp");
    const auto query = service->Query({.kind = CodeQueryKind::Text, .path = "a.cpp"}, QueryTest::Context());
    REQUIRE(query.HasValue());
    CHECK(query.Value().text == "dirty");
    CHECK(identities->Size() == 1);
    CHECK(service->Query({.kind = CodeQueryKind::Text, .path = "unopened.cpp"}, QueryTest::Context()).HasError());
    CHECK(identities->Size() == 1);
    REQUIRE(documents->Close(identity.Value().identity.instance).HasValue());
    CHECK(service->Query({.kind = CodeQueryKind::Text, .path = "a.cpp"}, QueryTest::Context()).HasError());
    CHECK(query.Value().text == "dirty");
    REQUIRE(documents->Shutdown().HasValue());
}
