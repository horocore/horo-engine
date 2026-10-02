#include "assets/AssetCookOutputFixture.h"

using namespace Horo::Assets::OutputTestSupport;

TEST_CASE("PublishCookGeneration creates current.json and manifest.json", "[native]") {
    TempDir tmp;
    auto nullTarget = Target("headless-null");

    std::vector<AssetCookManifestEntry> entries;
    std::vector<std::vector<std::uint8_t>> payloads;

    auto id1 = Id("00000000-0000-0000-0000-000000000001");
    auto payload1 = MakePayload(id1, 64, 0xAB);
    entries.push_back(
        {.assetId = id1, .assetType = Type("core.mesh"), .artifactFile = id1.ToString() + ".cooked", .artifactHash = DigestOf(payload1)});
    payloads.push_back(payload1);

    auto id2 = Id("00000000-0000-0000-0000-000000000002");
    auto payload2 = MakePayload(id2, 128, 0xCD);
    entries.push_back(
        {.assetId = id2, .assetType = Type("core.mesh"), .artifactFile = id2.ToString() + ".cooked", .artifactHash = DigestOf(payload2)});
    payloads.push_back(payload2);

    auto result = PublishFixture(tmp.path, nullTarget, entries, payloads);
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
    const auto payload = MakePayload(id, 16, 0x7F);
    const std::vector<AssetCookManifestEntry> entries = {
        {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = "quoted\"artifact.cooked", .artifactHash = DigestOf(payload)}};
    const std::vector<std::vector<std::uint8_t>> payloads = {payload};

    const auto published = PublishFixture(tmp.path, target, entries, payloads);
    REQUIRE((published.HasError()));
    // current.json is written last: a rejected generation must leave no authority behind.
    REQUIRE((!std::filesystem::exists(tmp.path / "current.json")));
}

TEST_CASE("PublishCookGeneration rejects reserved and trailing-punctuation artifact filenames", "[native]") {
    TempDir tmp;
    const auto target = Target("headless-null");
    // Reserved DOS device names (case-insensitive, with or without extension),
    // trailing dots/spaces that Windows silently strips, and ".." segments.
    const std::string_view rejectedNames[] = {"CON.cooked",       "con.cooked",       "Con",
                                              "NUL.cooked",       "com0.cooked",      "com1.cooked",
                                              "LPT0.cooked",      "LPT4.cooked",      "aux",
                                              "clock$.cooked",    "conin$.txt",       "artifact.cooked.",
                                              "artifact.cooked ", "ar..tifact.cooked"};
    for (const auto name : rejectedNames) {
        const auto id = Id("00000000-0000-0000-0000-000000000005");
        const auto payload = MakePayload(id, 16, 0x7F);
        const std::vector<AssetCookManifestEntry> entries = {
            {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = std::string(name), .artifactHash = DigestOf(payload)}};
        const std::vector<std::vector<std::uint8_t>> payloads = {payload};

        INFO("rejected name: " << name);
        const auto published = PublishFixture(tmp.path, target, entries, payloads);
        REQUIRE((published.HasError()));
        REQUIRE((!std::filesystem::exists(tmp.path / "current.json")));
    }

    // Sanity: a normal portable name is still accepted.
    const auto id = Id("00000000-0000-0000-0000-000000000006");
    const auto payload = MakePayload(id, 16, 0x7F);
    const std::vector<AssetCookManifestEntry> entries = {
        {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(payload)}};
    const std::vector<std::vector<std::uint8_t>> payloads = {payload};
    REQUIRE((PublishFixture(tmp.path, target, entries, payloads).HasValue()));
}

TEST_CASE("PublishCookGeneration rejects duplicate asset IDs", "[native]") {
    TempDir tmp;
    auto nullTarget = Target("headless-null");

    auto id = Id("00000000-0000-0000-0000-000000000001");
    auto payload = MakePayload(id, 32);

    // Unsorted: same ID twice
    std::vector<AssetCookManifestEntry> entries =
        {{.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(payload)},
         {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(payload)}};
    std::vector<std::vector<std::uint8_t>> payloads = {payload, payload};

    auto result = PublishFixture(tmp.path, nullTarget, entries, payloads);
    REQUIRE((result.HasError()));
}

TEST_CASE("PublishCookGeneration rejects mismatch between entries and payloads", "[native]") {
    TempDir tmp;
    auto nullTarget = Target("headless-null");

    auto id = Id("00000000-0000-0000-0000-000000000001");
    auto payload = MakePayload(id, 32);

    std::vector<AssetCookManifestEntry> entries = {
        {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(payload)}};
    std::vector<std::vector<std::uint8_t>> payloads = {payload, payload};  // 2 payloads, 1 entry

    auto result = PublishFixture(tmp.path, nullTarget, entries, payloads);
    REQUIRE((result.HasError()));
}

TEST_CASE("PublishCookGeneration publishes a complete empty inventory through the same authority", "[native]") {
    TempDir tmp;
    auto nullTarget = Target("headless-null");

    std::vector<AssetCookManifestEntry> entries;
    std::vector<std::vector<std::uint8_t>> payloads;

    auto result = PublishFixture(tmp.path, nullTarget, entries, payloads);
    REQUIRE(result.HasValue());
    const auto resolved = ResolveCurrentCookGeneration(tmp.path);
    REQUIRE(resolved.HasValue());
    CHECK(resolved.Value().artifactCount == 0U);
    CHECK(ReadCookGenerationContents(resolved.Value(), 4096U).Value().entries.empty());
}

TEST_CASE("PublishCookGeneration is deterministic", "[native]") {
    TempDir tmp;
    auto nullTarget = Target("headless-null");

    auto id = Id("00000000-0000-0000-0000-000000000001");
    auto payload = MakePayload(id, 64, 0xEE);

    std::vector<AssetCookManifestEntry> entries = {
        {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(payload)}};
    std::vector<std::vector<std::uint8_t>> payloads = {payload};

    auto result1 = PublishFixture(tmp.path, nullTarget, entries, payloads);
    REQUIRE((result1.HasValue()));

    auto result2 = PublishFixture(tmp.path, nullTarget, entries, payloads);
    REQUIRE((result2.HasValue()));

    // Same manifest digest = same generation path
    REQUIRE((result1.Value().manifestDigest.bytes == result2.Value().manifestDigest.bytes));
}

TEST_CASE("ResolveCurrentCookGeneration reads published generation", "[native]") {
    TempDir tmp;
    auto nullTarget = Target("headless-null");

    auto id = Id("00000000-0000-0000-0000-000000000005");
    auto payload = MakePayload(id, 48, 0x11);

    std::vector<AssetCookManifestEntry> entries = {
        {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(payload)}};
    std::vector<std::vector<std::uint8_t>> payloads = {payload};

    auto pubResult = PublishFixture(tmp.path, nullTarget, entries, payloads);
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
    const auto payload = MakePayload(id, 16, 0x22);
    const std::vector<AssetCookManifestEntry> entries = {
        {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(payload)}};
    const std::vector<std::vector<std::uint8_t>> payloads = {payload};
    auto published = PublishFixture(tmp.path, target, entries, payloads);
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

TEST_CASE("Publication verifies real envelopes, canonical filenames, digests and hard limits before staging", "[native]") {
    TempDir tmp;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8);
    auto entry = Entry(id, bytes);
    const std::vector<std::vector<std::uint8_t>> payloads{bytes};
    SECTION("identity") {
        entry.assetId = Id("00000000-0000-0000-0000-000000000002");
        entry.artifactFile = entry.assetId.ToString() + ".cooked";
    }
    SECTION("type") {
        entry.assetType = Type("core.texture");
    }
    SECTION("filename") {
        entry.artifactFile = "plain-artifact.cooked";
    }
    SECTION("hash") {
        entry.artifactHash = {};
    }
    SECTION("raw payload") {
        const std::vector<std::vector<std::uint8_t>> raw{{1, 2, 3}};
        entry.artifactHash = DigestOf(raw.front());
        CHECK(PublishFixture(tmp.path, Target("headless-null"), std::span{&entry, 1U}, raw).HasError());
        return;
    }
    SECTION("target") {
        CHECK(PublishFixture(tmp.path, Target("wrong-target"), std::span{&entry, 1U}, payloads).HasError());
        return;
    }
    SECTION("limit escalation") {
        AssetCookLimits limits;
        ++limits.maximumAssets;
        CHECK(PublishFixture(tmp.path, Target("headless-null"), std::span{&entry, 1U}, payloads, limits).HasError());
        return;
    }
    CHECK(PublishFixture(tmp.path, Target("headless-null"), std::span{&entry, 1U}, payloads).HasError());
    CHECK_FALSE(std::filesystem::exists(tmp.path / "current.json"));
}

TEST_CASE("Immutable replay never repairs or overwrites corrupted reader storage", "[native]") {
    TempDir tmp;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8);
    const std::vector entries{Entry(id, bytes)};
    const std::vector<std::vector<std::uint8_t>> payloads{bytes};
    auto first = PublishFixture(tmp.path, Target("headless-null"), entries, payloads);
    REQUIRE(first.HasValue());
    const auto pointer = ReadText(tmp.path / "current.json");
    const auto artifact = first.Value().generationRoot / entries.front().artifactFile;
    {
        std::ofstream output(artifact, std::ios::binary | std::ios::trunc);
        output << "corrupt";
    }
    CHECK(PublishFixture(tmp.path, Target("headless-null"), entries, payloads).HasError());
    CHECK(ReadText(artifact) == "corrupt");
    CHECK(ReadText(tmp.path / "current.json") == pointer);
    CHECK(ResolveCurrentCookGeneration(tmp.path).HasError());
}

TEST_CASE("Pinned generation remains complete across real pointer replacement", "[native]") {
    TempDir tmp;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto oldBytes = MakePayload(id, 8, 1);
    const auto newBytes = MakePayload(id, 8, 2);
    PublicationFiles files;
    files.root = tmp.path;
    auto oldGeneration = PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, oldBytes), oldBytes, 4096U, {},
                                                        Policy(files, "10000000-0000-0000-0000-000000000001"));
    REQUIRE(oldGeneration.HasValue());
    auto policy = Policy(files, "10000000-0000-0000-0000-000000000002");
    bool prepared = false;
    bool adopted = false;
    policy.prepareCommit = [&](const AssetCookGeneration &candidate) {
        prepared = true;
        CHECK(candidate.manifestDigest != oldGeneration.Value().manifestDigest);
        CHECK(ResolveCurrentCookGeneration(tmp.path).Value().manifestDigest == oldGeneration.Value().manifestDigest);
        return Result<void>::Success();
    };
    policy.afterCommit = [&](const AssetCookGeneration &candidate) {
        adopted = true;
        CHECK(prepared);
        CHECK(ResolveCurrentCookGeneration(tmp.path).Value().manifestDigest == candidate.manifestDigest);
        CHECK(files.native.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "observer").HasError());
    };
    auto replacement = PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, newBytes), newBytes, 4096U, {}, policy);
    REQUIRE(replacement.HasValue());
    CHECK(adopted);
    auto pinned = ReadCookGenerationContents(oldGeneration.Value(), 4096U);
    REQUIRE(pinned.HasValue());
    CHECK(pinned.Value().artifacts.front() == oldBytes);
    CHECK(ReadCookGenerationContents(replacement.Value(), 4096U).Value().artifacts.front() == newBytes);
}
