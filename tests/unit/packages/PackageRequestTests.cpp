#include "Horo/Packages/PackageRequest.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Packages;
    using Json = nlohmann::json;

    /** @brief Supplies portable explicit authority and root intent without transport/trust credentials. */
    Json Intent() {
        return {{"sources", {{"horo.public", {{"kind", "public-registry"}, {"registry", "official"}}}}},
                {"dependencies",
                 {{"com.horo.assets",
                   {{"source", "horo.public"}, {"version", "^1.0.0"}, {"features", Json::array({"data", "portable"})}}}}}};
    }

    /** @brief Generates a strict actual lock with independent source/version and the supplied intent commitment. */
    ValidatedPackageLockfileV1 Lock(const Sha256Digest hash, const std::string_view version = "1.2.0",
                                    const std::string_view source = "horo.public", const std::string_view packageName = "com.horo.assets") {
        const auto package = HoroPackageId::Parse(packageName).Value();
        const auto parsedVersion = PackageVersion::Parse(version).Value();
        const auto parsedSource = HoroPackageSourceId::Parse(source).Value();
        const auto artifact = ComputeSha256(std::as_bytes(std::span{"artifact", 8}));
        const PackageResolutionPlan plan{{{package, parsedVersion, parsedSource, artifact, {}}}};
        const PackageLockArtifact evidence{package, parsedVersion, parsedSource, artifact, artifact, artifact, 1, {}, {"assets"}};
        auto lock = ValidatedPackageLockfileV1::Generate(plan, std::span{&package, 1U}, hash, std::span{&evidence, 1U});
        REQUIRE(lock.HasValue());
        return std::move(lock).Value();
    }
}  // namespace

TEST_CASE("Portable package intent canonicalizes order and semantic defaults", "[packages][request]") {
    const auto first = ValidatedPackageRequest::Parse(Intent().dump());
    REQUIRE(first.HasValue());
    auto reordered = Intent();
    reordered["dependencies"]["com.horo.assets"]["features"] = Json::array({"portable", "data"});
    reordered["schemaVersion"] = 1;
    reordered["sources"]["horo.public"]["priority"] = 100;
    const auto second = ValidatedPackageRequest::Parse(reordered.dump(2));
    REQUIRE(second.HasValue());
    CHECK(first.Value().Digest() == second.Value().Digest());
    CHECK(first.Value().SerializeCanonical() == second.Value().SerializeCanonical());
    CHECK(first.Value().ValidateLock(Lock(first.Value().Digest())).HasValue());
    reordered["dependencies"]["com.horo.assets"]["features"] = Json::array({"new-feature"});
    const auto changed = ValidatedPackageRequest::Parse(reordered.dump());
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().ValidateLock(Lock(first.Value().Digest())).HasError());
}

TEST_CASE("Portable request independently checks lock source version and roots", "[packages][request]") {
    const auto intent = ValidatedPackageRequest::Parse(Intent().dump());
    REQUIRE(intent.HasValue());
    SECTION("wrong assigned source") {
        CHECK(intent.Value().ValidateLock(Lock(intent.Value().Digest(), "1.2.0", "other.source")).HasError());
    }
    SECTION("version outside range") {
        CHECK(intent.Value().ValidateLock(Lock(intent.Value().Digest(), "2.0.0")).HasError());
    }
    SECTION("missing required root") {
        REQUIRE(intent.Value().ValidateLock(Lock(intent.Value().Digest())).HasValue());
        // Keep the original intent commitment: rejection must come from root binding, not a stale hash or invalid empty lock.
        const auto differentRoot = Lock(intent.Value().Digest(), "1.2.0", "horo.public", "com.horo.other");
        REQUIRE(differentRoot.RequestHash() == intent.Value().Digest());
        REQUIRE(differentRoot.Roots().size() == 1);
        CHECK(differentRoot.Roots().front().package == HoroPackageId::Parse("com.horo.other").Value());
        CHECK(intent.Value().ValidateLock(differentRoot).HasError());
    }
}

TEST_CASE("Portable intent rejects unknown legacy credential and nonportable metadata", "[packages][request]") {
    auto input = Intent();
    SECTION("unknown root") {
        input["trust"] = true;
    }
    SECTION("unknown dependency") {
        input["dependencies"]["com.horo.assets"]["credential"] = "synthetic-credential";
    }
    SECTION("local source") {
        input["sources"]["horo.public"] = {{"kind", "local-development-override"}, {"root", "/tmp/local"}};
    }
    SECTION("unsupported range") {
        input["dependencies"]["com.horo.assets"]["version"] = ">=1.0.0";
    }
    SECTION("duplicate feature") {
        input["dependencies"]["com.horo.assets"]["features"] = Json::array({"data", "data"});
    }
    SECTION("unassigned source") {
        input["dependencies"]["com.horo.assets"]["source"] = "missing.source";
    }
    SECTION("credential locator") {
        input["sources"]["horo.public"] = {{"kind", "direct-artifact"},
                                           {"url", "https://synthetic-secret@example.test/p"},
                                           {"sha256", FormatSha256(ComputeSha256(std::span<const std::byte>{}))}};
    }
    REQUIRE(ValidatedPackageRequest::Parse(input.dump()).HasError());
}

TEST_CASE("Portable package intent applies duplicate byte source and root bounds", "[packages][request]") {
    CHECK(ValidatedPackageRequest::Parse(R"({"sources":{},"sources":{},"dependencies":{}})").HasError());
    CHECK(ValidatedPackageRequest::Parse(std::string(1024U * 1024U + 1, ' ')).HasError());
    auto input = Intent();
    for (int index = 0; index < 129; ++index)
        input["sources"]["source." + std::to_string(index)] = {{"kind", "public-registry"}, {"registry", "official"}};
    CHECK(ValidatedPackageRequest::Parse(input.dump()).HasError());
    input = Intent();
    for (int index = 0; index < 513; ++index)
        input["dependencies"]["com.horo.p" + std::to_string(index)] = {{"source", "horo.public"}, {"version", "*"}};
    CHECK(ValidatedPackageRequest::Parse(input.dump()).HasError());
}

TEST_CASE("Package intent preserves its distinct key ceiling and sticky nested duplicate rejection", "[packages][request]") {
    const std::string sourceName(128U, 's');
    Json input{{"sources", {{sourceName, {{"kind", "public-registry"}, {"registry", "official"}}}}}, {"dependencies", Json::object()}};
    REQUIRE(ValidatedPackageRequest::Parse(input.dump()).HasValue());
    input["sources"][sourceName + 's'] = input["sources"][sourceName];
    CHECK(ValidatedPackageRequest::Parse(input.dump()).HasError());
    CHECK(ValidatedPackageRequest::Parse(
              R"({"sources":{"horo.public":{"kind":"public-registry","registry":"official","registry":"official"}},"dependencies":{}})")
              .HasError());
    auto deep = Json::object();
    for (int depth = 0; depth < 12; ++depth)
        deep = {{"nested", std::move(deep)}};
    input = Intent();
    input["sources"]["horo.public"]["registry"] = std::move(deep);
    CHECK(ValidatedPackageRequest::Parse(input.dump()).HasError());
}
