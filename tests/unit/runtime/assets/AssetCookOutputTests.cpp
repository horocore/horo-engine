#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Foundation/Sha256.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;

    AssetId Id(const std::string_view value) {
        auto parsed = AssetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetTypeId Type(const std::string_view value) {
        auto parsed = AssetTypeId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetCookTargetId Target(const std::string_view value) {
        auto parsed = AssetCookTargetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    Sha256Digest DigestOf(std::span<const std::uint8_t> bytes) {
        return ComputeSha256(std::as_bytes(bytes));
    }

    std::vector<std::uint8_t> MakePayload(std::size_t size, std::uint8_t fill = 0x42) {
        return std::vector<std::uint8_t>(size, fill);
    }

    struct TempDir {
        std::filesystem::path path;

        TempDir() {
            auto tmp = std::filesystem::temp_directory_path() / "horo_output_test";
            std::filesystem::create_directories(tmp);
            auto unique = tmp / ("test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(unique);
            path = unique;
        }

        ~TempDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

}  // namespace

TEST_CASE("PublishCookGeneration creates current.json and manifest.json", "[native]") {
    TempDir tmp;
    auto nullTarget = Target("headless-null");

    std::vector<AssetCookManifestEntry> entries;
    std::vector<std::vector<std::uint8_t>> payloads;

    auto id1 = Id("00000000-0000-0000-0000-000000000001");
    auto payload1 = MakePayload(64, 0xAB);
    entries.push_back(
        {.assetId = id1, .assetType = Type("core.mesh"), .artifactFile = id1.ToString() + ".cooked", .artifactHash = DigestOf(payload1)});
    payloads.push_back(payload1);

    auto id2 = Id("00000000-0000-0000-0000-000000000002");
    auto payload2 = MakePayload(128, 0xCD);
    entries.push_back(
        {.assetId = id2, .assetType = Type("core.mesh"), .artifactFile = id2.ToString() + ".cooked", .artifactHash = DigestOf(payload2)});
    payloads.push_back(payload2);

    auto result = PublishCookGeneration(tmp.path, nullTarget, entries, payloads);
    REQUIRE((result.HasValue()));
    auto gen = result.Value();

    REQUIRE((gen.target == nullTarget));
    REQUIRE((gen.artifactCount == 2));
    REQUIRE((std::filesystem::exists(gen.generationRoot)));
    REQUIRE((std::filesystem::exists(gen.generationRoot / "manifest.json")));
    REQUIRE((std::filesystem::exists(gen.generationRoot / (id1.ToString() + ".cooked"))));
    REQUIRE((std::filesystem::exists(gen.generationRoot / (id2.ToString() + ".cooked"))));
    REQUIRE((std::filesystem::exists(tmp.path / "current.json")));
}

// Filenames that are not portable across supported platforms (Windows/NTFS
// reserves " < > | : ? * and control characters) must be rejected explicitly
// with MalformedArtifact on every platform, instead of failing late with an
// opaque filesystem error on Windows only.
//
// Note: this replaces the former "escapes manifest strings" test. That test's
// intent — exercising AppendJsonString escaping — is unreachable through
// PublishCookGeneration because all manifest string inputs (target ID, type ID,
// artifact filename) are canonicalized/validated before they reach the JSON
// writer. AppendJsonString remains as a defense-in-depth layer.
TEST_CASE("PublishCookGeneration rejects non-portable artifact filenames", "[native]") {
    TempDir tmp;
    const auto target = Target("headless-null");
    const auto id = Id("00000000-0000-0000-0000-000000000003");
    const auto payload = MakePayload(16, 0x7F);
    const std::vector<AssetCookManifestEntry> entries = {
        {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = "quoted\"artifact.cooked", .artifactHash = DigestOf(payload)}};
    const std::vector<std::vector<std::uint8_t>> payloads = {payload};

    const auto published = PublishCookGeneration(tmp.path, target, entries, payloads);
    REQUIRE((published.HasError()));
    // current.json is written last: a rejected generation must leave no authority behind.
    REQUIRE((!std::filesystem::exists(tmp.path / "current.json")));
}

TEST_CASE("PublishCookGeneration rejects reserved and trailing-punctuation artifact filenames", "[native]") {
    TempDir tmp;
    const auto target = Target("headless-null");
    const auto payload = MakePayload(16, 0x7F);
    // Reserved DOS device names (case-insensitive, with or without extension),
    // trailing dots/spaces that Windows silently strips, and ".." segments.
    const std::string_view rejectedNames[] = {"CON.cooked",       "con.cooked",       "Con",
                                              "NUL.cooked",       "com0.cooked",      "com1.cooked",
                                              "LPT0.cooked",      "LPT4.cooked",      "aux",
                                              "clock$.cooked",    "conin$.txt",       "artifact.cooked.",
                                              "artifact.cooked ", "ar..tifact.cooked"};
    for (const auto name : rejectedNames) {
        const auto id = Id("00000000-0000-0000-0000-000000000005");
        const std::vector<AssetCookManifestEntry> entries = {
            {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = std::string(name), .artifactHash = DigestOf(payload)}};
        const std::vector<std::vector<std::uint8_t>> payloads = {payload};

        INFO("rejected name: " << name);
        const auto published = PublishCookGeneration(tmp.path, target, entries, payloads);
        REQUIRE((published.HasError()));
        REQUIRE((!std::filesystem::exists(tmp.path / "current.json")));
    }

    // Sanity: a normal portable name is still accepted.
    const auto id = Id("00000000-0000-0000-0000-000000000006");
    const std::vector<AssetCookManifestEntry> entries = {
        {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = "plain-artifact.cooked", .artifactHash = DigestOf(payload)}};
    const std::vector<std::vector<std::uint8_t>> payloads = {payload};
    REQUIRE((PublishCookGeneration(tmp.path, target, entries, payloads).HasValue()));
}

TEST_CASE("PublishCookGeneration rejects duplicate asset IDs", "[native]") {
    TempDir tmp;
    auto nullTarget = Target("headless-null");

    auto id = Id("00000000-0000-0000-0000-000000000001");
    auto payload = MakePayload(32);

    // Unsorted: same ID twice
    std::vector<AssetCookManifestEntry> entries =
        {{.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(payload)},
         {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(payload)}};
    std::vector<std::vector<std::uint8_t>> payloads = {payload, payload};

    auto result = PublishCookGeneration(tmp.path, nullTarget, entries, payloads);
    REQUIRE((result.HasError()));
}

TEST_CASE("PublishCookGeneration rejects mismatch between entries and payloads", "[native]") {
    TempDir tmp;
    auto nullTarget = Target("headless-null");

    auto id = Id("00000000-0000-0000-0000-000000000001");
    auto payload = MakePayload(32);

    std::vector<AssetCookManifestEntry> entries = {
        {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(payload)}};
    std::vector<std::vector<std::uint8_t>> payloads = {payload, payload};  // 2 payloads, 1 entry

    auto result = PublishCookGeneration(tmp.path, nullTarget, entries, payloads);
    REQUIRE((result.HasError()));
}

TEST_CASE("PublishCookGeneration rejects empty entries", "[native]") {
    TempDir tmp;
    auto nullTarget = Target("headless-null");

    std::vector<AssetCookManifestEntry> entries;
    std::vector<std::vector<std::uint8_t>> payloads;

    auto result = PublishCookGeneration(tmp.path, nullTarget, entries, payloads);
    REQUIRE((result.HasError()));
}

TEST_CASE("PublishCookGeneration is deterministic", "[native]") {
    TempDir tmp;
    auto nullTarget = Target("headless-null");

    auto id = Id("00000000-0000-0000-0000-000000000001");
    auto payload = MakePayload(64, 0xEE);

    std::vector<AssetCookManifestEntry> entries = {
        {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(payload)}};
    std::vector<std::vector<std::uint8_t>> payloads = {payload};

    auto result1 = PublishCookGeneration(tmp.path, nullTarget, entries, payloads);
    REQUIRE((result1.HasValue()));

    auto result2 = PublishCookGeneration(tmp.path, nullTarget, entries, payloads);
    REQUIRE((result2.HasValue()));

    // Same manifest digest = same generation path
    REQUIRE((result1.Value().manifestDigest.bytes == result2.Value().manifestDigest.bytes));
}

TEST_CASE("ResolveCurrentCookGeneration reads published generation", "[native]") {
    TempDir tmp;
    auto nullTarget = Target("headless-null");

    auto id = Id("00000000-0000-0000-0000-000000000005");
    auto payload = MakePayload(48, 0x11);

    std::vector<AssetCookManifestEntry> entries = {
        {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(payload)}};
    std::vector<std::vector<std::uint8_t>> payloads = {payload};

    auto pubResult = PublishCookGeneration(tmp.path, nullTarget, entries, payloads);
    REQUIRE((pubResult.HasValue()));

    auto resolveResult = ResolveCurrentCookGeneration(tmp.path);
    REQUIRE((resolveResult.HasValue()));
    auto gen = resolveResult.Value();

    REQUIRE((gen.target == nullTarget));
    REQUIRE((gen.artifactCount == 1));
    REQUIRE((gen.manifestDigest == pubResult.Value().manifestDigest));
    REQUIRE((std::filesystem::exists(gen.generationRoot)));
}

TEST_CASE("ResolveCurrentCookGeneration rejects changed manifest and malformed count", "[native]") {
    TempDir tmp;
    const auto target = Target("headless-null");
    const auto id = Id("00000000-0000-0000-0000-000000000007");
    const auto payload = MakePayload(16, 0x22);
    const std::vector<AssetCookManifestEntry> entries = {
        {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(payload)}};
    const std::vector<std::vector<std::uint8_t>> payloads = {payload};
    auto published = PublishCookGeneration(tmp.path, target, entries, payloads);
    REQUIRE(published.HasValue());

    const auto currentPath = tmp.path / "current.json";
    std::ifstream currentInput(currentPath, std::ios::binary);
    const std::string current{std::istreambuf_iterator<char>{currentInput}, std::istreambuf_iterator<char>{}};
    auto malformedCount = current;
    const auto countPosition = malformedCount.find(R"("artifactCount":"1")");
    REQUIRE(countPosition != std::string::npos);
    malformedCount.replace(countPosition, std::string_view{R"("artifactCount":"1")"}.size(), R"("artifactCount":"not-a-number")");
    {
        std::ofstream output(currentPath, std::ios::binary | std::ios::trunc);
        output << malformedCount;
    }
    CHECK(ResolveCurrentCookGeneration(tmp.path).HasError());
    {
        std::ofstream output(currentPath, std::ios::binary | std::ios::trunc);
        output << current;
    }

    const auto manifestPath = published.Value().generationRoot / "manifest.json";
    {
        std::ofstream output(manifestPath, std::ios::binary | std::ios::app);
        output << 'x';
    }
    CHECK(ResolveCurrentCookGeneration(tmp.path).HasError());
}

TEST_CASE("ResolveCurrentCookGeneration rejects a generation path outside the target root", "[native]") {
    TempDir tmp;
    std::ofstream current{tmp.path / "current.json", std::ios::binary | std::ios::trunc};
    current << R"({"target":"headless-null","manifestDigest":"ignored","generationPath":"../outside","artifactCount":"1"})";
    current.close();

    const auto resolved = ResolveCurrentCookGeneration(tmp.path);

    REQUIRE((resolved.HasError()));
}
