#include "Horo/Packages/PackageLockfile.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <random>

namespace {
    using namespace Horo::Packages;
    using Json = nlohmann::json;

    HoroPackageId Id(const std::string_view text) {
        auto value = HoroPackageId::Parse(text);
        REQUIRE(value.HasValue());
        return std::move(value).Value();
    }

    HoroPackageSourceId Source(const std::string_view text) {
        auto value = HoroPackageSourceId::Parse(text);
        REQUIRE(value.HasValue());
        return std::move(value).Value();
    }

    PackageVersion Version(const std::string_view text) {
        auto value = PackageVersion::Parse(text);
        REQUIRE(value.HasValue());
        return std::move(value).Value();
    }

    Horo::Sha256Digest Digest(const std::string_view text) {
        return Horo::ComputeSha256(std::as_bytes(std::span{text.data(), text.size()}));
    }

    ResolvedPackage Resolved(const std::string_view id, const std::string_view version, const std::string_view source,
                             const std::string_view artifact, std::vector<HoroPackageId> dependencies = {}) {
        return {Id(id), Version(version), Source(source), Digest(artifact), std::move(dependencies)};
    }

    PackageLockArtifact Artifact(const ResolvedPackage &resolved, std::vector<PackagePlatform> platforms = {},
                                 std::vector<std::string> contributions = {}) {
        return {resolved.package,
                resolved.version,
                resolved.source,
                resolved.artifactDigest,
                Digest("manifest:" + resolved.package.Value()),
                Digest("files:" + resolved.package.Value()),
                1U,
                std::move(platforms),
                std::move(contributions)};
    }

    struct Fixture {
        PackageResolutionPlan plan;
        std::vector<HoroPackageId> roots;
        std::vector<PackageLockArtifact> artifacts;
        Horo::Sha256Digest requestHash{Digest("request")};

        Fixture() {
            plan.packages = {Resolved("com.game.root", "1.2.0", "vendor.index", "root", {Id("com.horo.runtime"), Id("com.shared.data")}),
                             Resolved("com.horo.runtime", "2.0.1", "public.registry", "runtime", {Id("com.shared.data")}),
                             Resolved("com.shared.data", "3.4.5", "public.registry", "shared")};
            roots = {Id("com.game.root")};
            artifacts = {Artifact(plan.packages[0], {{"linux", "x64", "horo-sdk-2"}, {"windows", "x64", "horo-sdk-2"}},
                                  {"runtime", "assets"}),
                         Artifact(plan.packages[1], {{"linux", "x64", "horo-sdk-2"}}, {"runtime"}), Artifact(plan.packages[2])};
        }

        Horo::Result<ValidatedPackageLockfileV1> Generate(const PackageLockfileLimits &limits = {}) const {
            return ValidatedPackageLockfileV1::Generate(plan, roots, requestHash, artifacts, limits);
        }
    };

    void CheckFailure(const auto &result, const std::string_view code) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == code);
    }
}  // namespace

TEST_CASE("Package lock generation is deterministic and round trips exact graph evidence", "[packages][lockfile]") {
    Fixture fixture;
    const auto expected = fixture.Generate();
    REQUIRE(expected.HasValue());

    std::mt19937 random{42};
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::ranges::shuffle(fixture.plan.packages, random);
        std::ranges::shuffle(fixture.artifacts, random);
        for (auto &artifact : fixture.artifacts) {
            std::ranges::shuffle(artifact.platforms, random);
            std::ranges::shuffle(artifact.contributions, random);
        }
        const auto generated = fixture.Generate();
        REQUIRE(generated.HasValue());
        CHECK(generated.Value().SerializeCanonical() == expected.Value().SerializeCanonical());
    }

    const std::string encoded = expected.Value().SerializeCanonical();
    const auto parsed = ValidatedPackageLockfileV1::Parse(encoded);
    REQUIRE(parsed.HasValue());
    CHECK(parsed.Value().SerializeCanonical() == encoded);
    CHECK(parsed.Value().RequestHash() == fixture.requestHash);
    REQUIRE(parsed.Value().Roots().size() == 1U);
    CHECK(parsed.Value().Roots().front().package.Value() == "com.game.root");
    REQUIRE(parsed.Value().Packages().size() == 3U);
    CHECK(parsed.Value().Packages().front().package.Value() == "com.game.root");
    CHECK(parsed.Value().Packages().front().dependencies.size() == 2U);
    CHECK(parsed.Value().Packages().front().dependencies.front().package.Value() == "com.horo.runtime");
    CHECK(parsed.Value().Packages().front().contributions == std::vector<std::string>{"assets", "runtime"});
}

TEST_CASE("Package removal preview prunes only entries no remaining root can reach", "[packages][lockfile][removal]") {
    Fixture fixture;
    SECTION("removing the last root prunes the entire graph") {
        const auto lock = fixture.Generate();
        REQUIRE(lock.HasValue());
        const auto preview = lock.Value().PreviewRemoval(Id("com.game.root"));
        REQUIRE(preview.HasValue());
        CHECK(preview.Value().directRoot);
        CHECK(preview.Value().removed == std::vector<LockedPackageReference>{{Id("com.game.root"), Version("1.2.0")},
                                                                             {Id("com.horo.runtime"), Version("2.0.1")},
                                                                             {Id("com.shared.data"), Version("3.4.5")}});
        CHECK(preview.Value().requiredBy.empty());
    }
    SECTION("another root preserves shared dependencies and explains retained roots") {
        fixture.roots.push_back(Id("com.horo.runtime"));
        const auto lock = fixture.Generate();
        REQUIRE(lock.HasValue());
        const auto preview = lock.Value().PreviewRemoval(Id("com.game.root"));
        REQUIRE(preview.HasValue());
        CHECK(preview.Value().removed == std::vector<LockedPackageReference>{{Id("com.game.root"), Version("1.2.0")}});

        const auto retainedRoot = lock.Value().PreviewRemoval(Id("com.horo.runtime"));
        REQUIRE(retainedRoot.HasValue());
        CHECK(retainedRoot.Value().directRoot);
        CHECK(retainedRoot.Value().removed.empty());
        CHECK(retainedRoot.Value().requiredBy == std::vector<LockedPackageReference>{{Id("com.game.root"), Version("1.2.0")}});

        const auto transitive = lock.Value().PreviewRemoval(Id("com.shared.data"));
        REQUIRE(transitive.HasValue());
        CHECK_FALSE(transitive.Value().directRoot);
        CHECK(transitive.Value().removed.empty());
        CHECK(transitive.Value().requiredBy.size() == 2);
        CheckFailure(lock.Value().PreviewRemoval(Id("com.missing")), "packages.lockfile.invalid");
    }
}

TEST_CASE("Package lock generation binds every resolver selection to exact verified evidence", "[packages][lockfile]") {
    Fixture fixture;

    SECTION("missing artifact evidence") {
        fixture.artifacts.pop_back();
        CheckFailure(fixture.Generate(), "packages.lockfile.invalid");
    }
    SECTION("duplicate artifact evidence") {
        fixture.artifacts.back() = fixture.artifacts.front();
        CheckFailure(fixture.Generate(), "packages.lockfile.invalid");
    }
    SECTION("artifact digest mismatch") {
        fixture.artifacts.front().artifactDigest = Digest("changed");
        CheckFailure(fixture.Generate(), "packages.lockfile.invalid");
    }
    SECTION("unknown requested root") {
        fixture.roots = {Id("com.missing")};
        CheckFailure(fixture.Generate(), "packages.lockfile.invalid");
    }
    SECTION("stale dependency edge") {
        fixture.plan.packages.front().dependencies.push_back(Id("com.missing"));
        CheckFailure(fixture.Generate(), "packages.lockfile.invalid");
    }
    SECTION("duplicate contribution identity") {
        fixture.artifacts.front().contributions = {"runtime", "runtime"};
        CheckFailure(fixture.Generate(), "packages.lockfile.invalid");
    }
}

TEST_CASE("Package lock parser rejects noncanonical schema and portable identity input", "[packages][lockfile]") {
    Fixture fixture;
    const auto generated = fixture.Generate();
    REQUIRE(generated.HasValue());
    const Json valid = Json::parse(generated.Value().SerializeCanonical());

    SECTION("a nonempty graph requires roots") {
        Json changed = valid;
        changed["roots"] = Json::array();
        CheckFailure(ValidatedPackageLockfileV1::Parse(changed.dump()), "packages.lockfile.invalid");
    }

    SECTION("unknown root field") {
        Json changed = valid;
        changed["workspace"] = "local";
        CheckFailure(ValidatedPackageLockfileV1::Parse(changed.dump()), "packages.lockfile.invalid");
    }
    SECTION("unknown entry field") {
        Json changed = valid;
        changed["packages"][0]["cachePath"] = "/home/user/cache";
        CheckFailure(ValidatedPackageLockfileV1::Parse(changed.dump()), "packages.lockfile.invalid");
    }
    SECTION("duplicate JSON key") {
        std::string changed = generated.Value().SerializeCanonical();
        changed.insert(changed.find('{') + 1U, "\n  \"schemaVersion\": 1,");
        CheckFailure(ValidatedPackageLockfileV1::Parse(changed), "packages.lockfile.invalid");
    }
    SECTION("transport locator is not a source authority identity") {
        Json changed = valid;
        changed["packages"][0]["source"] = "https://registry.example/private?token=secret";
        CheckFailure(ValidatedPackageLockfileV1::Parse(changed.dump()), "packages.lockfile.invalid");
    }
    SECTION("digest is noncanonical") {
        Json changed = valid;
        changed["packages"][0]["manifestSha256"] = "SHA256:1234";
        CheckFailure(ValidatedPackageLockfileV1::Parse(changed.dump()), "packages.lockfile.invalid");
    }
    SECTION("package version is not canonical") {
        Json changed = valid;
        changed["packages"][0]["version"] = "01.2.0";
        CheckFailure(ValidatedPackageLockfileV1::Parse(changed.dump()), "packages.lockfile.invalid");
    }
    SECTION("arrays must already use canonical order") {
        Json changed = valid;
        std::swap(changed["packages"][0], changed["packages"][1]);
        CheckFailure(ValidatedPackageLockfileV1::Parse(changed.dump()), "packages.lockfile.invalid");
    }
}

TEST_CASE("Package lock parser rejects stale, unreachable and cyclic graph entries", "[packages][lockfile]") {
    Fixture fixture;
    const auto generated = fixture.Generate();
    REQUIRE(generated.HasValue());
    const Json valid = Json::parse(generated.Value().SerializeCanonical());

    SECTION("dependency pins a different version") {
        Json changed = valid;
        changed["packages"][0]["dependencies"][0]["version"] = "9.0.0";
        CheckFailure(ValidatedPackageLockfileV1::Parse(changed.dump()), "packages.lockfile.invalid");
    }
    SECTION("unreachable package remains in the lock") {
        Json changed = valid;
        changed["packages"][0]["dependencies"].erase(changed["packages"][0]["dependencies"].begin() + 1);
        changed["packages"][1]["dependencies"].clear();
        CheckFailure(ValidatedPackageLockfileV1::Parse(changed.dump()), "packages.lockfile.invalid");
    }
    SECTION("exact dependency graph contains a cycle") {
        Json changed = valid;
        changed["packages"][2]["dependencies"].push_back({{"id", "com.game.root"}, {"version", "1.2.0"}});
        CheckFailure(ValidatedPackageLockfileV1::Parse(changed.dump()), "packages.lockfile.invalid");
    }
    SECTION("duplicate root is rejected") {
        Json changed = valid;
        changed["roots"].push_back(changed["roots"].front());
        CheckFailure(ValidatedPackageLockfileV1::Parse(changed.dump()), "packages.lockfile.invalid");
    }
}

TEST_CASE("Package lock restore validation is pure and fail closed", "[packages][lockfile][restore]") {
    Fixture fixture;
    const auto generated = fixture.Generate();
    REQUIRE(generated.HasValue());
    const auto lock = ValidatedPackageLockfileV1::Parse(generated.Value().SerializeCanonical());
    REQUIRE(lock.HasValue());
    const std::string before = lock.Value().SerializeCanonical();

    CHECK(lock.Value().ValidateForRestore(fixture.requestHash, {"linux", "x64", "horo-sdk-2"}).HasValue());
    CheckFailure(lock.Value().ValidateForRestore(Digest("new request"), {"linux", "x64", "horo-sdk-2"}), "packages.lockfile.stale");
    CheckFailure(lock.Value().ValidateForRestore(fixture.requestHash, {"macos", "arm64", "horo-sdk-2"}),
                 "packages.lockfile.unsupported_platform");
    CheckFailure(lock.Value().ValidateForRestore(fixture.requestHash, {"linux", "x64", "horo-sdk-2"}, 2U),
                 "packages.lockfile.unsupported_format");
    CheckFailure(lock.Value().ValidateForRestore(fixture.requestHash, {"Linux", "x64", "horo-sdk-2"}), "packages.lockfile.invalid");
    CHECK(lock.Value().SerializeCanonical() == before);
}

TEST_CASE("Package lock admission enforces document and collection limits", "[packages][lockfile][limits]") {
    Fixture fixture;
    const auto generated = fixture.Generate();
    REQUIRE(generated.HasValue());
    const std::string encoded = generated.Value().SerializeCanonical();

    PackageLockfileLimits limits;
    limits.documentBytes = encoded.size() - 1U;
    CheckFailure(ValidatedPackageLockfileV1::Parse(encoded, limits), "packages.lockfile.limit");

    limits = {};
    limits.packages = 2U;
    CheckFailure(ValidatedPackageLockfileV1::Parse(encoded, limits), "packages.lockfile.limit");

    limits = {};
    limits.dependenciesPerPackage = 1U;
    CheckFailure(ValidatedPackageLockfileV1::Parse(encoded, limits), "packages.lockfile.limit");

    limits = {};
    limits.platformsPerPackage = 1U;
    CheckFailure(fixture.Generate(limits), "packages.lockfile.limit");
}
