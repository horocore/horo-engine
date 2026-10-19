#include "GameplaySourceLibraryTestSupport.h"

#include <type_traits>

using namespace Horo::Tests::GameplayLibraries;

static_assert(!std::is_aggregate_v<GameplaySourceLibrary>);
static_assert(!std::is_default_constructible_v<GameplaySourceLibrary>);

TEST_CASE("Gameplay source selection requires installed content and exact independent compilation approval",
          "[packages][gameplay][source]") {
    Fixture fixture;
    const std::vector sources{Parse<PackagePath>("libraries/matematik_ü/Answer.cpp")};
    CHECK(GameplaySourceLibrary::Select({}, fixture.id, fixture.contribution, sources, fixture.archive->Digest()).HasError());
    CHECK(GameplaySourceLibrary::Select(fixture.installed, fixture.id, fixture.contribution, sources).HasError());
    CHECK(GameplaySourceLibrary::Select(fixture.installed, fixture.id, fixture.contribution, sources, Horo::Sha256Digest{}).HasError());
    CHECK(GameplaySourceLibrary::Select(fixture.installed, Parse<HoroPackageId>("com.example.missing"), fixture.contribution, sources,
                                        fixture.archive->Digest())
              .HasError());
    CHECK(fixture.Select().Package().lock.version.ToString() == "1.0.0");
}

TEST_CASE("Gameplay source selection preserves exact inventory and canonical source identity", "[packages][gameplay][source]") {
    Fixture fixture;
    const auto first = Parse<PackagePath>("libraries/matematik_ü/Answer.cpp");
    const auto second = Parse<PackagePath>("libraries/matematik_ü/Other.cpp");
    auto forward =
        GameplaySourceLibrary::Select(fixture.installed, fixture.id, fixture.contribution, {first, second}, fixture.archive->Digest());
    auto reverse =
        GameplaySourceLibrary::Select(fixture.installed, fixture.id, fixture.contribution, {second, first}, fixture.archive->Digest());
    REQUIRE(forward.HasValue());
    REQUIRE(reverse.HasValue());
    CHECK(forward.Value().Fingerprint() == reverse.Value().Fingerprint());
    CHECK(forward.Value().Sources().front().Value() == first.Value());
    CHECK(forward.Value().Files().size() == 4);
    CHECK(forward.Value().Fingerprint() != fixture.Select().Fingerprint());
    fixture.graph->packages.front().lock.version = Parse<PackageVersion>("9.0.0");
    CHECK(forward.Value().Package().lock.version.ToString() == "1.0.0");
}

TEST_CASE("Gameplay source selection rejects missing duplicate foreign and non-source inputs", "[packages][gameplay][source]") {
    Fixture fixture;
    auto source = Parse<PackagePath>("libraries/matematik_ü/Answer.cpp");
    std::vector sources{source};
    SECTION("empty") {
        sources.clear();
    }
    SECTION("duplicate") {
        sources.push_back(source);
    }
    SECTION("missing") {
        sources = {Parse<PackagePath>("libraries/matematik_ü/Missing.cpp")};
    }
    SECTION("metadata") {
        sources = {Parse<PackagePath>("horo-package.toml")};
    }
    SECTION("build script") {
        sources = {Parse<PackagePath>("libraries/matematik_ü/CMakeLists.txt")};
    }
    SECTION("foreign contribution") {
        fixture.contribution = Parse<PackagePath>("libraries/other");
    }
    SECTION("source bound") {
        sources.assign(257, source);
    }
    CHECK(
        GameplaySourceLibrary::Select(fixture.installed, fixture.id, fixture.contribution, sources, fixture.archive->Digest()).HasError());
}

TEST_CASE("Gameplay source selection enforces the complete contribution file bound", "[packages][gameplay][source]") {
    SECTION("exactly 256 files") {
        Fixture fixture{252};
        CHECK(fixture.Select().Files().size() == 256);
    }
    SECTION("257 files") {
        Fixture fixture{253};
        CHECK(GameplaySourceLibrary::Select(fixture.installed, fixture.id, fixture.contribution,
                                            {Parse<PackagePath>("libraries/matematik_ü/Answer.cpp")}, fixture.archive->Digest())
                  .HasError());
    }
}
