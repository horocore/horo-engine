#include "Horo/Assets/AssetArchive.h"
#include "Horo/Assets/AssetCook.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <utility>
#include <vector>

using namespace Horo;
using namespace Horo::Assets;

namespace {
    AssetId Id(const char *text) {
        auto value = AssetId::Parse(text);
        REQUIRE(value.HasValue());
        return std::move(value).Value();
    }

    AssetChunkId Chunk(const char *text) {
        auto value = AssetChunkId::Parse(text);
        REQUIRE(value.HasValue());
        return std::move(value).Value();
    }

    AssetCookTargetId Target(const char *text) {
        auto value = AssetCookTargetId::Parse(text);
        REQUIRE(value.HasValue());
        return std::move(value).Value();
    }

    AssetArchiveInput Cooked(const AssetId id, const AssetCookTargetId &target, std::vector<std::uint8_t> payload) {
        AssetCookArtifact artifact;
        artifact.id = id;
        artifact.type = AssetTypeId::Parse("core.mesh").Value();
        artifact.target = target;
        artifact.payload = std::move(payload);
        artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
        auto encoded = EncodeCookedArtifact(artifact);
        REQUIRE(encoded.HasValue());
        return {id, std::move(encoded).Value()};
    }

    AssetChunkPlan Plan(const AssetId first, const AssetId second) {
        const std::array chunks{AssetChunkDefinition{.id = Chunk("core"), .assets = {first}},
                                AssetChunkDefinition{.id = Chunk("world"),
                                                     .kind = AssetChunkKind::Optional,
                                                     .assets = {second},
                                                     .dependencies = {Chunk("core")}}};
        auto plan = AssetChunkPlan::Create(chunks);
        REQUIRE(plan.HasValue());
        return std::move(plan).Value();
    }
}  // namespace

TEST_CASE("Release archive is deterministic and provides exact cooked bytes", "[assets][release][archive]") {
    const auto first = Id("00000000-0000-0000-0000-000000000001");
    const auto second = Id("00000000-0000-0000-0000-000000000002");
    const auto target = Target("headless-null");
    const auto plan = Plan(first, second);
    const auto core = Cooked(first, target, {1U, 2U, 3U});
    const auto world = Cooked(second, target, {4U, 5U});
    const std::array forward{core, world};
    const std::array reverse{world, core};

    auto encoded = BuildAssetArchive(plan, target, forward);
    auto reordered = BuildAssetArchive(plan, target, reverse);
    REQUIRE(encoded.HasValue());
    REQUIRE(reordered.HasValue());
    CHECK(encoded.Value() == reordered.Value());

    auto opened = AssetArchiveProvider::Open(encoded.Value(), target);
    REQUIRE(opened.HasValue());
    auto provider = std::move(opened).Value();
    const CancellationToken cancellation;
    auto exists = provider.Exists(first, cancellation);
    REQUIRE(exists.HasValue());
    CHECK(exists.Value());
    auto loaded = provider.Load(world.id, cancellation);
    REQUIRE(loaded.HasValue());
    CHECK(loaded.Value() == world.bytes);
    CHECK(provider.Load(Id("00000000-0000-0000-0000-000000000003"), cancellation).HasError());
}

TEST_CASE("Selected release archive exposes only admitted chunk assets", "[assets][release][archive]") {
    const auto first = Id("00000000-0000-0000-0000-000000000001");
    const auto second = Id("00000000-0000-0000-0000-000000000002");
    const auto target = Target("headless-null");
    const auto plan = Plan(first, second);
    const auto inputs = std::array{Cooked(first, target, {1U}), Cooked(second, target, {2U})};
    auto encoded = BuildAssetArchive(plan, target, inputs);
    REQUIRE(encoded.HasValue());
    Sha256Digest base{};
    base.bytes[0] = 1U;
    const CancellationToken cancellation;

    auto coreOnly = AssetArchiveProvider::OpenSelected(encoded.Value(), target, plan, std::array{Chunk("core")}, base);
    REQUIRE(coreOnly.HasValue());
    REQUIRE(coreOnly.Value().MountedChunks().size() == 1);
    CHECK(coreOnly.Value().MountedChunks().front().id == Chunk("core"));
    CHECK(coreOnly.Value().Target() == target);
    CHECK(coreOnly.Value().ArchiveDigest() == ComputeSha256(std::as_bytes(std::span{encoded.Value()})));
    CHECK(coreOnly.Value().Exists(first, cancellation).Value());
    CHECK_FALSE(coreOnly.Value().Exists(second, cancellation).Value());
    CHECK(coreOnly.Value().Load(second, cancellation).HasError());
    CHECK(coreOnly.Value().StoredByteLength(first) == inputs[0].bytes.size());
    CHECK_FALSE(coreOnly.Value().StoredByteLength(second).has_value());

    auto withOptional = AssetArchiveProvider::OpenSelected(encoded.Value(), target, plan, std::array{Chunk("world"), Chunk("core")}, base);
    REQUIRE(withOptional.HasValue());
    CHECK(withOptional.Value().MountedChunks().size() == 2);
    CHECK(withOptional.Value().ArchiveDigest() == coreOnly.Value().ArchiveDigest());
    CHECK(withOptional.Value().Load(second, cancellation).Value() == inputs[1].bytes);
    CHECK(AssetArchiveProvider::OpenSelected(encoded.Value(), target, plan, std::array{Chunk("world")}, base).HasError());
    CHECK(AssetArchiveProvider::OpenSelected(encoded.Value(), target, plan, std::array{Chunk("core"), Chunk("core")}, base).HasError());
}

TEST_CASE("Selected release archive rejects drift and incompatible DLC", "[assets][release][archive]") {
    const auto first = Id("00000000-0000-0000-0000-000000000001");
    const auto second = Id("00000000-0000-0000-0000-000000000002");
    const auto target = Target("headless-null");
    Sha256Digest base{};
    base.bytes[0] = 1U;
    const std::array definitions{AssetChunkDefinition{.id = Chunk("core"), .assets = {first}},
                                 AssetChunkDefinition{.id = Chunk("dlc"),
                                                      .kind = AssetChunkKind::Dlc,
                                                      .assets = {second},
                                                      .dependencies = {Chunk("core")},
                                                      .mountPriority = 1,
                                                      .requiredBaseManifest = base}};
    auto plan = AssetChunkPlan::Create(definitions);
    REQUIRE(plan.HasValue());
    const auto inputs = std::array{Cooked(first, target, {1U}), Cooked(second, target, {2U})};
    auto encoded = BuildAssetArchive(plan.Value(), target, inputs);
    REQUIRE(encoded.HasValue());
    const std::array selection{Chunk("core"), Chunk("dlc")};
    auto wrongBase = base;
    wrongBase.bytes[0] = 2U;
    CHECK(AssetArchiveProvider::OpenSelected(encoded.Value(), target, plan.Value(), selection, wrongBase).HasError());
    auto drifted = definitions;
    drifted[1].mountPriority = 2;
    auto wrongPlan = AssetChunkPlan::Create(drifted);
    REQUIRE(wrongPlan.HasValue());
    CHECK(AssetArchiveProvider::OpenSelected(encoded.Value(), target, wrongPlan.Value(), selection, base).HasError());
    auto tampered = encoded.Value();
    tampered[tampered.size() / 2U] ^= 1U;
    CHECK(AssetArchiveProvider::OpenSelected(tampered, target, plan.Value(), selection, base).HasError());
}

TEST_CASE("Release archive rejects missing, extra, or wrong-target cooked artifacts", "[assets][release][archive]") {
    const auto first = Id("00000000-0000-0000-0000-000000000001");
    const auto second = Id("00000000-0000-0000-0000-000000000002");
    const auto target = Target("headless-null");
    const auto plan = Plan(first, second);
    const auto core = Cooked(first, target, {1U});
    const auto wrongTarget = Cooked(second, Target("desktop-opengl"), {2U});
    CHECK(BuildAssetArchive(plan, target, std::array{core}).HasError());
    CHECK(BuildAssetArchive(plan, target, std::array{core, core}).HasError());
    CHECK(BuildAssetArchive(plan, target, std::array{core, wrongTarget}).HasError());
}

TEST_CASE("Release archive fails closed on tampering and unsupported target", "[assets][release][archive]") {
    const auto first = Id("00000000-0000-0000-0000-000000000001");
    const auto second = Id("00000000-0000-0000-0000-000000000002");
    const auto target = Target("headless-null");
    const auto inputs = std::array{Cooked(first, target, {1U}), Cooked(second, target, {2U})};
    auto encoded = BuildAssetArchive(Plan(first, second), target, inputs);
    REQUIRE(encoded.HasValue());

    CHECK(AssetArchiveProvider::Open(encoded.Value(), Target("desktop-opengl")).HasError());
    auto tampered = encoded.Value();
    tampered[tampered.size() / 2U] ^= 1U;
    CHECK(AssetArchiveProvider::Open(tampered, target).HasError());
    tampered = encoded.Value();
    tampered[8U] = 2U;
    const auto digest = ComputeSha256(std::as_bytes(std::span{tampered.data(), tampered.size() - 32U}));
    std::copy(digest.bytes.begin(), digest.bytes.end(), tampered.end() - 32);
    CHECK(AssetArchiveProvider::Open(tampered, target).HasError());

    tampered = encoded.Value();
    tampered[12U] = 1U;
    const auto featureDigest = ComputeSha256(std::as_bytes(std::span{tampered.data(), tampered.size() - 32U}));
    std::copy(featureDigest.bytes.begin(), featureDigest.bytes.end(), tampered.end() - 32);
    CHECK(AssetArchiveProvider::Open(tampered, target).HasError());
}

TEST_CASE("Release archive consumes only verified pinned cook generation", "[assets][release][archive]") {
    const auto first = Id("00000000-0000-0000-0000-000000000001");
    const auto second = Id("00000000-0000-0000-0000-000000000002");
    const auto target = Target("headless-null");
    const auto inputs = std::array{Cooked(first, target, {1U}), Cooked(second, target, {2U})};
    const auto type = AssetTypeId::Parse("core.mesh").Value();
    const std::array entries{AssetCookManifestEntry{first, type, first.ToString() + ".cooked",
                                                    ComputeSha256(std::as_bytes(std::span{inputs[0].bytes}))},
                             AssetCookManifestEntry{second, type, second.ToString() + ".cooked",
                                                    ComputeSha256(std::as_bytes(std::span{inputs[1].bytes}))}};
    const std::array payloads{inputs[0].bytes, inputs[1].bytes};
    const auto root = std::filesystem::temp_directory_path() /
                      ("horo_release_archive_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    struct Cleanup {
        std::filesystem::path root;

        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    } cleanup{root};

    std::filesystem::create_directories(root);
    NativeDurableFileSystem files;
    auto writer = files.TryAcquireExclusive(std::filesystem::weakly_canonical(root) / ".cook-writer.lock", "archive fixture");
    REQUIRE(writer.HasValue());
    const AssetCookPublicationPolicy policy{.files = &files,
                                            .operationId = "10000000-0000-0000-0000-000000000001",
                                            .writerLease = &writer.Value()};
    auto generation = PublishCookGeneration(std::filesystem::weakly_canonical(root), target, entries, payloads, {}, policy);
    REQUIRE(generation.HasValue());
    auto archive = BuildAssetArchive(Plan(first, second), generation.Value());
    REQUIRE(archive.HasValue());
    auto provider = AssetArchiveProvider::Open(archive.Value(), target);
    REQUIRE(provider.HasValue());

    auto undersized = BuildAssetArchive(Plan(first, second), generation.Value(), {.maximumArchiveBytes = 1U});
    CHECK(undersized.HasError());
    auto incorrectDigest = generation.Value();
    incorrectDigest.manifestDigest.bytes[0] ^= 1U;
    CHECK(BuildAssetArchive(Plan(first, second), incorrectDigest).HasError());

    const auto artifactPath = generation.Value().generationRoot / entries[0].artifactFile;
    std::ofstream tamper(artifactPath, std::ios::binary | std::ios::trunc);
    REQUIRE(tamper.good());
    tamper.put('x');
    tamper.close();
    CHECK(BuildAssetArchive(Plan(first, second), generation.Value()).HasError());
}
