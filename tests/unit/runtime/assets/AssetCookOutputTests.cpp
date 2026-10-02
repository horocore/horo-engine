#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Assets/AssetCookTransaction.h"
#include "Horo/Foundation/Sha256.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

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

    std::vector<std::uint8_t> MakePayload(const AssetId &id, std::size_t size, std::uint8_t fill = 0x42) {
        const std::vector<std::uint8_t> bytes(size, fill);
        auto encoded = EncodeCookedArtifact(AssetCookArtifact{.id = id,
                                                              .type = Type("core.mesh"),
                                                              .target = Target("headless-null"),
                                                              .payloadDigest = DigestOf(bytes),
                                                              .payload = bytes});
        REQUIRE(encoded.HasValue());
        return std::move(encoded).Value();
    }

    struct TempDir {
        std::filesystem::path path;

        TempDir() {
            auto tmp = std::filesystem::temp_directory_path() / "horo_output_test";
            std::filesystem::create_directories(tmp);
            auto unique = tmp / ("test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(unique);
            path = std::filesystem::canonical(unique);
        }

        ~TempDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    enum class PublicationFault {
        None,
        ArtifactWrite,
        ManifestWrite,
        PointerWrite,
        GenerationRename,
        GenerationFlush,
        RootFlush,
        PointerRename,
        PointerSync,
        PointerException
    };

    struct PublicationFiles final : DurableFileSystem {
        NativeDurableFileSystem native;
        PublicationFault fault{};
        std::filesystem::path root;

        Result<void> Failure() const {
            return Result<void>::Failure(Error{.code = ErrorCode{"test.publication_io"}, .message = "Injected publication I/O failure."});
        }

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, std::string_view owner) override {
            return native.TryAcquireExclusive(path, owner);
        }

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native.AvailableBytes(path);
        }

        Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            if ((fault == PublicationFault::ArtifactWrite && path.extension() == ".cooked") ||
                (fault == PublicationFault::ManifestWrite && path.filename() == "manifest.json") ||
                (fault == PublicationFault::PointerWrite && path.filename() == "current.json"))
                return Failure();
            return native.WriteDurable(path, bytes);
        }

        Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return native.CopyDurable(source, destination);
        }

        Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
            const bool pointer = destination.filename() == "current.json";
            if ((pointer && fault == PublicationFault::PointerRename) || (!pointer && fault == PublicationFault::GenerationRename))
                return Failure();
            auto result = native.AtomicReplace(prepared, destination);
            if (result.HasError())
                return result;
            if (pointer && fault == PublicationFault::PointerException)
                throw 42;
            if (pointer && fault == PublicationFault::PointerSync)
                return Failure();
            return result;
        }

        Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native.RemoveDurable(path);
        }

        Result<void> SyncDirectory(const std::filesystem::path &path) override {
            if ((fault == PublicationFault::GenerationFlush && path.filename() == "generation") ||
                (fault == PublicationFault::RootFlush && path == root))
                return Failure();
            return native.SyncDirectory(path);
        }
    };

    AssetCookManifestEntry Entry(const AssetId &id, const std::vector<std::uint8_t> &bytes) {
        return {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(bytes)};
    }

    AssetCookPublicationPolicy Policy(PublicationFiles &files, const std::string_view token) {
        return {.files = &files, .operationId = std::string(token)};
    }

    std::string ReadText(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    Result<AssetCookGeneration> PublishFixture(const std::filesystem::path &root, const AssetCookTargetId &target,
                                               std::span<const AssetCookManifestEntry> entries,
                                               std::span<const std::vector<std::uint8_t>> payloads, const AssetCookLimits &limits = {}) {
        NativeDurableFileSystem files;
        auto lock = files.TryAcquireExclusive(root / ".cook-writer.lock", "publication fixture");
        REQUIRE(lock.HasValue());
        AssetCookPublicationPolicy policy{.files = &files};
        policy.newOperationId = [] {
            static std::uint64_t next{};
            const auto value = ++next;
            std::array<std::uint8_t, 16> bytes{};
            for (std::size_t index = 0; index < sizeof(value); ++index)
                bytes[index] = static_cast<std::uint8_t>(value >> (index * 8U));
            return Result<AssetId>::Success(AssetId::FromBytes(bytes));
        };
        policy.writerLease = &lock.Value();
        return Horo::Assets::PublishCookGeneration(root, target, entries, payloads, limits, policy);
    }

}  // namespace

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
    const std::vector payloads{bytes};
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

TEST_CASE("Filesystem failure before pointer commit preserves the prior generation and recoverable staging", "[native]") {
    const PublicationFault faults[] = {PublicationFault::ArtifactWrite,   PublicationFault::ManifestWrite,
                                       PublicationFault::PointerWrite,    PublicationFault::GenerationRename,
                                       PublicationFault::GenerationFlush, PublicationFault::RootFlush,
                                       PublicationFault::PointerRename};
    for (const auto fault : faults) {
        TempDir tmp;
        const auto id = Id("00000000-0000-0000-0000-000000000001");
        const auto oldBytes = MakePayload(id, 8, 1);
        const auto newBytes = MakePayload(id, 8, 2);
        PublicationFiles files;
        files.root = tmp.path;
        REQUIRE(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, oldBytes), oldBytes, 4096U, {},
                                               Policy(files, "10000000-0000-0000-0000-000000000001"))
                    .HasValue());
        const auto pointer = ReadText(tmp.path / "current.json");
        files.fault = fault;
        const auto result = PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, newBytes), newBytes, 4096U, {},
                                                           Policy(files, "10000000-0000-0000-0000-000000000002"));
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "test.publication_io");
        CHECK(ReadText(tmp.path / "current.json") == pointer);
        CHECK(ResolveCurrentCookGeneration(tmp.path).HasValue());
        files.fault = PublicationFault::None;
        auto lock = files.native.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "restart");
        REQUIRE(lock.HasValue());
        REQUIRE(RecoverCookPublication(tmp.path, Target("headless-null"), 4096U, {}, {.files = &files, .writerLease = &lock.Value()})
                    .HasValue());
        CHECK(std::filesystem::is_empty(tmp.path / ".cook-staging"));
        CHECK(ReadText(tmp.path / "current.json") == pointer);
    }
}

TEST_CASE("Pointer durability failure and adapter exception retain true committed adoption", "[native]") {
    for (const auto fault : {PublicationFault::PointerSync, PublicationFault::PointerException}) {
        TempDir tmp;
        const auto id = Id("00000000-0000-0000-0000-000000000001");
        const auto bytes = MakePayload(id, 8);
        PublicationFiles files;
        files.root = tmp.path;
        files.fault = fault;
        auto policy = Policy(files, "10000000-0000-0000-0000-000000000001");
        bool adopted = false;
        policy.afterCommit = [&](const AssetCookGeneration &candidate) {
            adopted = true;
            CHECK(candidate.durabilityError.has_value());
        };
        const auto result = PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {}, policy);
        REQUIRE(result.HasValue());
        CHECK(result.Value().durabilityError.has_value());
        CHECK(adopted);
        CHECK(ResolveCurrentCookGeneration(tmp.path).Value().manifestDigest == result.Value().manifestDigest);
    }
}

TEST_CASE("Failed identical pointer replacement cannot masquerade as committed durability uncertainty", "[native]") {
    TempDir tmp;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8);
    PublicationFiles files;
    files.root = tmp.path;
    REQUIRE(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {},
                                           Policy(files, "10000000-0000-0000-0000-000000000001"))
                .HasValue());
    files.fault = PublicationFault::PointerRename;
    CHECK(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {},
                                         Policy(files, "10000000-0000-0000-0000-000000000002"))
              .HasError());
}

TEST_CASE("Final cancellation preserves pointer and cancellation after commit preserves terminal truth", "[native]") {
    TempDir tmp;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8);
    PublicationFiles files;
    files.root = tmp.path;
    REQUIRE(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {},
                                           Policy(files, "10000000-0000-0000-0000-000000000001"))
                .HasValue());
    const auto pointer = ReadText(tmp.path / "current.json");
    auto policy = Policy(files, "10000000-0000-0000-0000-000000000002");
    policy.beforeCommit = [] {
        return Result<void>::Failure(Error{.code = ErrorCode{"test.cancelled"}});
    };
    CHECK(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {}, policy).HasError());
    CHECK(ReadText(tmp.path / "current.json") == pointer);
    policy = Policy(files, "10000000-0000-0000-0000-000000000003");
    bool cancelled = false;
    policy.afterCommit = [&](const AssetCookGeneration &) {
        cancelled = true;
    };
    CHECK(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {}, policy).HasValue());
    CHECK(cancelled);
}

TEST_CASE("Writer contention rereads the then-current complete base and checkpoints freshness under lease", "[native]") {
    TempDir tmp;
    PublicationFiles files;
    files.root = tmp.path;
    const auto first = Id("00000000-0000-0000-0000-000000000001");
    const auto second = Id("00000000-0000-0000-0000-000000000002");
    const auto firstBytes = MakePayload(first, 8, 1);
    const auto secondBytes = MakePayload(second, 8, 2);
    auto nativeLease = files.native.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "competing writer");
    REQUIRE(nativeLease.HasValue());
    std::optional<ExclusiveFileLock> held{std::move(nativeLease).Value()};
    auto policy = Policy(files, "10000000-0000-0000-0000-000000000002");
    std::size_t waits{};
    policy.waitingForWriter = [&] {
        ++waits;
        held.reset();
        const auto competing = PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(first, firstBytes), firstBytes,
                                                              4096U, {}, Policy(files, "10000000-0000-0000-0000-000000000001"));
        REQUIRE(competing.HasValue());
        return Result<void>::Success();
    };
    bool checked = false;
    policy.afterWriterAcquired = [&] {
        checked = true;
        CHECK(files.native.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "checkpoint observer").HasError());
        return Result<void>::Success();
    };
    auto replacement =
        PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(second, secondBytes), secondBytes, 4096U, {}, policy);
    REQUIRE(replacement.HasValue());
    CHECK(waits == 1U);
    CHECK(checked);
    auto inventory = ReadCookGenerationContents(replacement.Value(), 4096U);
    REQUIRE(inventory.HasValue());
    REQUIRE(inventory.Value().entries.size() == 2U);
    CHECK(inventory.Value().artifacts.front() == firstBytes);
}

TEST_CASE("Restart cleans bounded owned staging without selecting inactive generations or repairing invalid authority", "[native]") {
    TempDir tmp;
    PublicationFiles files;
    files.root = tmp.path;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8);
    auto published = PublishFixture(tmp.path, Target("headless-null"), std::vector{Entry(id, bytes)}, std::vector{bytes});
    REQUIRE(published.HasValue());
    const auto staging = tmp.path / ".cook-staging" / "20000000-0000-0000-0000-000000000001";
    std::filesystem::create_directories(staging / "generation");
    {
        std::ofstream output(staging / "generation" / (id.ToString() + ".cooked"));
        output << "interrupted";
    }
    {
        std::ofstream output(staging / "current.json");
        output << "interrupted";
    }
    SECTION("invalid current") {
        std::ofstream output(tmp.path / "current.json", std::ios::trunc);
        output << "invalid";
    }
    SECTION("missing established current") {
        std::filesystem::remove(tmp.path / "current.json");
    }
    auto lease = files.native.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "restart");
    REQUIRE(lease.HasValue());
    CHECK(
        RecoverCookPublication(tmp.path, Target("headless-null"), 4096U, {}, {.files = &files, .writerLease = &lease.Value()}).HasError());
    CHECK(std::filesystem::is_empty(tmp.path / ".cook-staging"));
    CHECK(std::filesystem::exists(published.Value().generationRoot));
    CHECK(ReadCookGenerationContents(published.Value(), 4096U).HasValue());
}

TEST_CASE("Recovery prevalidates all owned files and preserves unexpected paths and outside links", "[native]") {
    TempDir tmp;
    PublicationFiles files;
    const auto operation = tmp.path / ".cook-staging" / "20000000-0000-0000-0000-000000000001";
    std::filesystem::create_directories(operation / "generation");
    const auto outside = tmp.path / "outside.txt";
    {
        std::ofstream output(outside);
        output << "protected";
    }
    const auto unexpected = operation / "generation" / "unexpected.txt";
    SECTION("unexpected file") {
        std::ofstream output(unexpected);
        output << "protected";
    }
    SECTION("symlink") {
        std::error_code error;
        std::filesystem::create_symlink(outside, operation / "current.json", error);
        if (error)
            SKIP("Host cannot create the symlink fixture.");
    }
    SECTION("hard link") {
        std::error_code error;
        std::filesystem::create_hard_link(outside, operation / "current.json", error);
        if (error)
            SKIP("Host cannot create the hard-link fixture.");
    }
    auto lease = files.native.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "restart");
    REQUIRE(lease.HasValue());
    CHECK(
        RecoverCookPublication(tmp.path, Target("headless-null"), 4096U, {}, {.files = &files, .writerLease = &lease.Value()}).HasError());
    CHECK(std::filesystem::exists(operation));
    CHECK(ReadText(outside) == "protected");
}

TEST_CASE("Native writer leases reject link aliases before truncating diagnostic metadata", "[native]") {
    TempDir tmp;
    NativeDurableFileSystem files;
    const auto outside = tmp.path / "outside.txt";
    {
        std::ofstream output(outside);
        output << "protected";
    }
    const auto lock = tmp.path / ".cook-writer.lock";
    std::error_code error;
    SECTION("symlink") {
        std::filesystem::create_symlink(outside, lock, error);
    }
    SECTION("hard link") {
        std::filesystem::create_hard_link(outside, lock, error);
    }
    if (error)
        SKIP("Host cannot create the lock alias fixture.");
    CHECK(files.TryAcquireExclusive(lock, "must not truncate").HasError());
    CHECK(ReadText(outside) == "protected");
}

TEST_CASE("Publication has no unlocked filesystem fallback", "[native]") {
    TempDir tmp;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8);
    const std::vector entries{Entry(id, bytes)};
    const std::vector payloads{bytes};
    CHECK(Horo::Assets::PublishCookGeneration(tmp.path, Target("headless-null"), entries, payloads).HasError());
    CHECK_FALSE(std::filesystem::exists(tmp.path / "generations"));
    NativeDurableFileSystem files;
    const ExclusiveFileLock empty;
    CHECK(
        Horo::Assets::PublishCookGeneration(tmp.path, Target("headless-null"), entries, payloads, {},
                                            {.files = &files, .operationId = "10000000-0000-0000-0000-000000000001", .writerLease = &empty})
            .HasError());
    auto unrelated = files.TryAcquireExclusive(tmp.path / "unrelated.lock", "wrong authority");
    REQUIRE(unrelated.HasValue());
    CHECK(Horo::Assets::PublishCookGeneration(tmp.path, Target("headless-null"), entries, payloads, {},
                                              {.files = &files,
                                               .operationId = "10000000-0000-0000-0000-000000000001",
                                               .writerLease = &unrelated.Value()})
              .HasError());
    CHECK(RecoverCookPublication(tmp.path, Target("headless-null"), 4096U, {}, {.files = &files, .writerLease = &unrelated.Value()})
              .HasError());
    CHECK_FALSE(std::filesystem::exists(tmp.path / "generations"));
}

TEST_CASE("Native lease authority survives a move and rejects default and foreign paths", "[native]") {
    TempDir tmp;
    NativeDurableFileSystem files;
    const auto path = tmp.path / ".cook-writer.lock";
    auto acquired = files.TryAcquireExclusive(path, "owner");
    REQUIRE(acquired.HasValue());
    CHECK(acquired.Value().ProtectsPath(path));
    CHECK_FALSE(acquired.Value().ProtectsPath(tmp.path / "other.lock"));
    ExclusiveFileLock moved = std::move(acquired).Value();
    CHECK_FALSE(acquired.Value().ProtectsPath(path));
    CHECK(moved.ProtectsPath(path));
    CHECK_FALSE(ExclusiveFileLock{}.ProtectsPath(path));
}

TEST_CASE("Serialized publication and restart recovery support spaces and non-ASCII host roots", "[native]") {
    TempDir tmp;
    const auto root = tmp.path / std::filesystem::path{u8"nav output örnek"};
    std::filesystem::create_directory(root);
    PublicationFiles files;
    files.root = root;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8);
    auto published = PublishCookArtifactReplacement(root, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {},
                                                    Policy(files, "10000000-0000-0000-0000-000000000001"));
    REQUIRE(published.HasValue());
    CHECK(ResolveCurrentCookGeneration(root).Value().manifestDigest == published.Value().manifestDigest);
    auto lease = files.native.TryAcquireExclusive(root / ".cook-writer.lock", "restart");
    REQUIRE(lease.HasValue());
    CHECK(RecoverCookPublication(root, Target("headless-null"), 4096U, {}, {.files = &files, .writerLease = &lease.Value()}).HasValue());
    CHECK(std::filesystem::is_empty(root / ".cook-staging"));
}

#if !defined(_WIN32)
TEST_CASE("Writer contention is enforced by the OS across independent processes", "[native]") {
    TempDir tmp;
    int wake[2]{};
    REQUIRE(pipe(wake) == 0);
    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        close(wake[1]);
        char signal{};
        if (read(wake[0], &signal, 1) != 1)
            _exit(2);
        NativeDurableFileSystem childFiles;
        auto contested = childFiles.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "child");
        const bool busy = contested.HasError() && contested.ErrorValue().code.Value() == "filesystem.lock_busy";
        _exit(busy ? 0 : 3);
    }
    close(wake[0]);
    // Acquire after fork: the child registry is empty, so only the native OS lock can reject it.
    NativeDurableFileSystem files;
    auto lease = files.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "parent");
    const char signal = 'x';
    CHECK(write(wake[1], &signal, 1) == 1);
    close(wake[1]);
    int status{};
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(lease.HasValue());
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
#endif
