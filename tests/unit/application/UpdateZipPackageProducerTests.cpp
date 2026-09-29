#include "Horo/Release/UpdateDeltaStaging.h"
#include "Horo/Release/UpdateStageReady.h"
#include "Horo/Release/UpdateStagedTree.h"
#include "Horo/Release/UpdateTransferCheckpointStore.h"
#include "Horo/Release/UpdateZipPackageProducer.h"
#include "Horo/Release/UpdateZipStagingJob.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <miniz.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Horo;
using namespace Horo::Release;

namespace {
    struct TemporaryDirectory final {
        TemporaryDirectory()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-update-zip-producer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(root / "source/bin");
            std::filesystem::create_directories(root / "first");
            std::filesystem::create_directories(root / "second");
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path root;
    };

    [[nodiscard]] std::string ReadFile(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{input}, {}};
    }

    void WriteFile(const std::filesystem::path &path, const std::string_view bytes) {
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    [[nodiscard]] DistributionPackageSelection Selection() {
        DistributionArtifactIdentity artifact;
        artifact.product = {DistributionProductKind::GameRuntime, {}};
        artifact.version = GameProductVersion{{1U, 2U, 3U, {}, {}}};
        artifact.platform = DistributionPlatform::Windows;
        artifact.architecture = DistributionArchitecture::X64;
        artifact.build = {"build_42"};
        artifact.package = {"package_42"};
        artifact.installation = DistributionInstallationId{"game_42"};
        auto selection = ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::ZipArchive);
        REQUIRE(selection.HasValue());
        return std::move(selection).Value();
    }

    [[nodiscard]] ReleasePreSignInventory Inventory() {
        auto inventory = ReleasePreSignInventory::Create(ReleaseCandidateId{42U}, {{"bin/game", ReleaseArtifactRole::Binary, 4U,
                                                                                    ComputeSha256(std::as_bytes(std::span{"game", 4U}))}});
        REQUIRE(inventory.HasValue());
        return std::move(inventory).Value();
    }

    class AcceptingProvider final : public Security::SignatureProvider {
    public:
        [[nodiscard]] bool Supports(Security::SignatureAlgorithm) const noexcept override {
            return true;
        }

        [[nodiscard]] Result<void> Verify(Security::SignatureAlgorithm, std::span<const std::byte>, const Sha256Digest &,
                                          std::span<const std::byte>) const override {
            return Result<void>::Success();
        }
    };

    [[nodiscard]] Security::ArtifactVerifier Verifier() {
        auto roots = std::make_shared<Security::TrustedRootStore>();
        std::vector<std::byte> key(65U, std::byte{1});
        key.front() = std::byte{0x04};
        REQUIRE(roots->Add({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = std::move(key)}).HasValue());
        return {std::make_shared<AcceptingProvider>(), std::move(roots)};
    }

    [[nodiscard]] Sha256Digest InventoryDigest(std::span<const UpdateStagedFile> files, const UpdateArchiveLimits &limits) {
        auto canonical = BuildCanonicalUpdateFileInventory(files, limits);
        REQUIRE(canonical.HasValue());
        return ComputeSha256(std::as_bytes(std::span{canonical.Value()}));
    }

    [[nodiscard]] UpdateTransferCheckpoint CompleteCheckpoint(const UpdatePackageRecord &package) {
        const UpdateTransferResponse response{.status = 200U,
                                              .requestedUrl = package.url,
                                              .effectiveUrl = package.url,
                                              .strongEtag = "\"release-1\"",
                                              .contentLength = package.size};
        auto transfer = PlanUpdateTransfer(package, response, std::nullopt);
        REQUIRE(transfer.HasValue());
        auto checkpoint = AdvanceUpdateTransfer(transfer.Value(), package.size);
        REQUIRE(checkpoint.HasValue());
        return std::move(checkpoint).Value();
    }

    [[nodiscard]] std::string AlternateSignedZip(const std::span<const UpdateStagedFile> target, const UpdateArchiveLimits &limits) {
        auto inventory = BuildCanonicalUpdateFileInventory(target, limits);
        REQUIRE(inventory.HasValue());
        mz_zip_archive writer{};
        REQUIRE(mz_zip_writer_init_heap(&writer, 0U, 0U));
        REQUIRE(mz_zip_writer_add_mem(&writer, UpdateFileInventoryPath.data(), inventory.Value().data(), inventory.Value().size(),
                                      MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_add_mem(&writer, "bin/game", "game", 4U, MZ_DEFAULT_COMPRESSION));
        void *bytes = nullptr;
        std::size_t size = 0U;
        REQUIRE(mz_zip_writer_finalize_heap_archive(&writer, &bytes, &size));
        std::string archive{static_cast<const char *>(bytes), size};
        mz_free(bytes);
        mz_zip_writer_end(&writer);
        return archive;
    }
}  // namespace

TEST_CASE("Selected ZIP delivery uses verified delta then signed full fallback", "[release][update][delta]") {
    constexpr UpdateArchiveLimits limits{.maximumEntries = 8U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U};
    for (const unsigned scenario : {0U, 1U, 2U}) {
        const bool forceFallback = scenario != 0U;
        const bool byteMismatch = scenario == 2U;
        TemporaryDirectory directory;
        NativeDurableFileSystem files;
        std::filesystem::create_directories(directory.root / "base/bin");
        std::filesystem::create_directories(directory.root / "versions");
        WriteFile(directory.root / "base/bin/game", "old!");
        WriteFile(directory.root / "source/bin/game", "game");
        const std::array base{UpdateStagedFile{"bin/game", 4U, ComputeSha256(std::as_bytes(std::span{"old!", 4U}))}};
        const std::array target{UpdateStagedFile{"bin/game", 4U, ComputeSha256(std::as_bytes(std::span{"game", 4U}))}};
        UpdateZipPackageProducer producer{limits};
        auto produced = producer.Produce({Selection(), Inventory(), directory.root / "source", directory.root / "first"});
        REQUIRE(produced.HasValue());
        const auto deltaBytes = ReadFile(directory.root / "first/update.zip");
        const auto signedBytes = byteMismatch ? AlternateSignedZip(target, limits) : deltaBytes;
        if (byteMismatch)
            REQUIRE(signedBytes != deltaBytes);
        UpdatePackageRecord full;
        full.selection = Selection();
        full.url = "https://updates.example.test/game.zip";
        full.size = signedBytes.size();
        full.digest = ComputeSha256(std::as_bytes(std::span{signedBytes}));
        full.signature = {.publisherId = "com.horo.updates",
                          .keyId = "key-1",
                          .artifactDigest = full.digest,
                          .signature = std::vector<std::byte>(64U, std::byte{1})};
        auto delta = full;
        auto deltaArtifact = full.selection.artifact;
        deltaArtifact.package = {"delta_42"};
        auto deltaSelection = ValidateDistributionPackageSelection(deltaArtifact, DistributionPackageFormat::DeltaZipArchive);
        REQUIRE(deltaSelection.HasValue());
        delta.selection = std::move(deltaSelection).Value();
        delta.url = "https://updates.example.test/delta.zip";
        delta.size = deltaBytes.size();
        delta.digest = ComputeSha256(std::as_bytes(std::span{deltaBytes}));
        delta.signature.artifactDigest = delta.digest;
        UpdatePackageCandidates candidates{full,
                                           UpdateDeltaPackageRecord{delta, full.selection.artifact.package, InventoryDigest(base, limits),
                                                                    InventoryDigest(target, limits), InventoryDigest(target, limits)}};
        const auto versions = directory.root / "versions";
        const UpdateDownloadPaths deltaPaths{versions / "delta_42.zip", versions / "delta_42.checkpoint"};
        const UpdateDownloadPaths fullPaths{versions / "package_42.zip", versions / "package_42.checkpoint"};
        const auto stage = versions / "package_42";
        const auto deltaStage = versions / "delta_42";
        WriteFile(deltaPaths.partialFile, deltaBytes);
        REQUIRE(
            SaveUpdateTransferCheckpoint(files, deltaPaths.partialFile, deltaPaths.checkpointFile, CompleteCheckpoint(delta)).HasValue());
        if (forceFallback) {
            WriteFile(fullPaths.partialFile, signedBytes);
            REQUIRE(
                SaveUpdateTransferCheckpoint(files, fullPaths.partialFile, fullPaths.checkpointFile, CompleteCheckpoint(full)).HasValue());
        }
        const std::span<const UpdateStagedFile> requestedTarget =
            scenario == 1U ? std::span<const UpdateStagedFile>{base} : std::span<const UpdateStagedFile>{target};
        auto staged = PrepareSelectedZipUpdateStageHttps({candidates,
                                                          directory.root / "base",
                                                          base,
                                                          requestedTarget,
                                                          deltaPaths,
                                                          deltaStage,
                                                          fullPaths,
                                                          stage,
                                                          {.maximumPackageBytes = 4096U},
                                                          limits},
                                                         files, Verifier(), {});
        REQUIRE(staged.HasValue());
        CHECK(staged.Value().usedDelta == !forceFallback);
        CHECK(ReadFile(fullPaths.partialFile) == signedBytes);
        CHECK(ReadFile(stage / "bin/game") == "game");
        CHECK(VerifyReadyUpdateStage(full, staged.Value().checkpoint, fullPaths.partialFile, stage, target, limits, Verifier()).HasValue());
        if (forceFallback)
            CHECK_FALSE(std::filesystem::exists(deltaStage));
    }
}

TEST_CASE("Delta reconstruction reproduces the exact signed full ZIP before publishing ready", "[release][update][delta]") {
    TemporaryDirectory directory;
    constexpr UpdateArchiveLimits limits{.maximumEntries = 8U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U};
    std::filesystem::create_directories(directory.root / "base/bin");
    std::filesystem::create_directories(directory.root / "base/assets");
    std::filesystem::create_directories(directory.root / "patch/bin");
    std::filesystem::create_directories(directory.root / "patch/assets");
    std::filesystem::create_directories(directory.root / "source/assets");
    std::filesystem::create_directories(directory.root / "versions");
    WriteFile(directory.root / "base/bin/game", "old!");
    WriteFile(directory.root / "base/assets/keep", "same");
    WriteFile(directory.root / "base/assets/removed", "gone");
    WriteFile(directory.root / "patch/bin/game", "game");
    WriteFile(directory.root / "patch/assets/added", "new!");
    WriteFile(directory.root / "source/bin/game", "game");
    WriteFile(directory.root / "source/assets/keep", "same");
    WriteFile(directory.root / "source/assets/added", "new!");
    const std::array base{UpdateStagedFile{"bin/game", 4U, ComputeSha256(std::as_bytes(std::span{"old!", 4U}))},
                          UpdateStagedFile{"assets/keep", 4U, ComputeSha256(std::as_bytes(std::span{"same", 4U}))},
                          UpdateStagedFile{"assets/removed", 4U, ComputeSha256(std::as_bytes(std::span{"gone", 4U}))}};
    const std::array target{UpdateStagedFile{"bin/game", 4U, ComputeSha256(std::as_bytes(std::span{"game", 4U}))},
                            UpdateStagedFile{"assets/keep", 4U, ComputeSha256(std::as_bytes(std::span{"same", 4U}))},
                            UpdateStagedFile{"assets/added", 4U, ComputeSha256(std::as_bytes(std::span{"new!", 4U}))}};
    const std::array patch{target[0], target[2]};
    auto plan = PlanUpdateFileDelta(base, target, patch, InventoryDigest(base, limits), InventoryDigest(target, limits),
                                    InventoryDigest(patch, limits), limits);
    REQUIRE(plan.HasValue());
    NativeDurableFileSystem files;
    const auto stage = directory.root / "versions/package_42";
    REQUIRE(ReconstructUpdateFileDeltaStage(plan.Value(), directory.root / "base", directory.root / "patch", stage, limits, files, {})
                .HasValue());

    UpdateZipPackageProducer producer{limits};
    auto releaseInventory =
        ReleasePreSignInventory::Create(ReleaseCandidateId{42U},
                                        {{target[0].path, ReleaseArtifactRole::Binary, target[0].size, target[0].digest},
                                         {target[1].path, ReleaseArtifactRole::Binary, target[1].size, target[1].digest},
                                         {target[2].path, ReleaseArtifactRole::Binary, target[2].size, target[2].digest}});
    REQUIRE(releaseInventory.HasValue());
    auto produced = producer.Produce({Selection(), releaseInventory.Value(), directory.root / "source", directory.root / "first"});
    REQUIRE(produced.HasValue());
    const auto signedBytes = ReadFile(directory.root / "first/update.zip");
    UpdatePackageRecord full;
    full.selection = Selection();
    full.url = "https://updates.example.test/game.zip";
    full.size = signedBytes.size();
    full.digest = ComputeSha256(std::as_bytes(std::span{signedBytes}));
    full.signature = {.publisherId = "com.horo.updates",
                      .keyId = "key-1",
                      .artifactDigest = full.digest,
                      .signature = std::vector<std::byte>(64U, std::byte{1})};
    const auto packageFile = directory.root / "versions/package_42.zip";
    const auto checkpointFile = directory.root / "versions/package_42.checkpoint";
    auto wrong = full;
    wrong.digest.bytes[0] ^= 1U;
    wrong.signature.artifactDigest = wrong.digest;
    CHECK(
        RepackVerifiedDeltaAsFullZip({wrong, plan.Value(), stage, packageFile, checkpointFile, limits}, files, Verifier(), {}).HasError());
    CHECK_FALSE(std::filesystem::exists(packageFile));
    CHECK_FALSE(std::filesystem::exists(checkpointFile));
    CHECK_FALSE(std::filesystem::exists(stage.string() + ".ready"));

    auto repacked = RepackVerifiedDeltaAsFullZip({full, plan.Value(), stage, packageFile, checkpointFile, limits}, files, Verifier(), {});
    REQUIRE(repacked.HasValue());
    CHECK(ReadFile(packageFile) == signedBytes);
    CHECK(std::filesystem::exists(repacked.Value().readyMarker));
    CHECK(VerifyReadyUpdateStage(full, repacked.Value().checkpoint, packageFile, stage, target, limits, Verifier()).HasValue());
    WriteFile(stage / "bin/game", "evil");
    CHECK(VerifyReadyUpdateStage(full, repacked.Value().checkpoint, packageFile, stage, target, limits, Verifier()).HasError());
}

TEST_CASE("ZIP producer emits deterministic exact bytes and the staging inventory", "[release][update][package]") {
    TemporaryDirectory directory;
    WriteFile(directory.root / "source/bin/game", "game");
    auto inventory = Inventory();
    UpdateZipPackageProducer producer{{.maximumEntries = 8U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U}};
    IReleasePackageProducer *producers[]{&producer};
    ReleasePackageRequest first{Selection(), inventory, directory.root / "source", directory.root / "first", "bin/game", {"bin/game"}};
    auto unbound = first;
    unbound.productEntrypoint.clear();
    CHECK(ProduceReleasePackage(unbound, producers).HasError());
    ReleasePackageRequest noExecutable{Selection(), inventory, directory.root / "source", directory.root / "first", "bin/game", {}};
    CHECK(ProduceReleasePackage(noExecutable, producers).HasError());
    ReleasePackageRequest duplicateExecutable{Selection(),
                                              inventory,
                                              directory.root / "source",
                                              directory.root / "first",
                                              "bin/game",
                                              {"bin/game", "bin/game"}};
    CHECK(ProduceReleasePackage(duplicateExecutable, producers).HasError());
    auto result = ProduceReleasePackage(first, producers);
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().files.size() == 1U);
    CHECK(result.Value().files.front().path == "update.zip");
    const auto firstBytes = ReadFile(directory.root / "first/update.zip");
    CHECK(result.Value().files.front().size == firstBytes.size());
    CHECK(result.Value().files.front().digest == ComputeSha256(std::as_bytes(std::span{firstBytes})));

    mz_zip_archive archive{};
    REQUIRE(mz_zip_reader_init_file(&archive, (directory.root / "first/update.zip").string().c_str(), 0U));
    REQUIRE(mz_zip_reader_get_num_files(&archive) == 2U);
    auto declared = BuildCanonicalUpdateFileInventory(std::array{UpdateStagedFile{"bin/game", 4U, inventory.Artifacts().front().digest,
                                                                                  UpdateFileMode::Executable, UpdateFileRole::Entrypoint}},
                                                      {.maximumEntries = 8U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U});
    REQUIRE(declared.HasValue());
    std::string inventoryBytes(declared.Value().size(), '\0');
    REQUIRE(mz_zip_reader_extract_file_to_mem(&archive, UpdateFileInventoryPath.data(), inventoryBytes.data(), inventoryBytes.size(), 0U));
    CHECK(inventoryBytes == declared.Value());
    std::string game(4U, '\0');
    REQUIRE(mz_zip_reader_extract_file_to_mem(&archive, "bin/game", game.data(), game.size(), 0U));
    CHECK(game == "game");
    mz_zip_reader_end(&archive);

    ReleasePackageRequest second{Selection(), inventory, directory.root / "source", directory.root / "second", "bin/game", {"bin/game"}};
    REQUIRE(ProduceReleasePackage(second, producers).HasValue());
    CHECK(ReadFile(directory.root / "second/update.zip") == firstBytes);
    CHECK(ProduceReleasePackage(first, producers).HasError());

    WriteFile(directory.root / "source/bin/game", "evil");
    CHECK(ProduceReleasePackage(second, producers).HasError());
}

TEST_CASE("ZIP package producer output stages from a complete durable checkpoint", "[release][update][package]") {
    TemporaryDirectory directory;
    WriteFile(directory.root / "source/bin/game", "game");
    auto inventory = Inventory();
    UpdateZipPackageProducer producer{{.maximumEntries = 8U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U}};
    IReleasePackageProducer *producers[]{&producer};
    ReleasePackageRequest request{Selection(), inventory, directory.root / "source", directory.root / "first", "bin/game", {"bin/game"}};
    auto result = ProduceReleasePackage(request, producers);
    REQUIRE(result.HasValue());

    UpdatePackageRecord package;
    package.url = "https://updates.example.test/game.zip";
    package.size = result.Value().files.front().size;
    package.digest = result.Value().files.front().digest;
    package.signature = {.publisherId = "com.horo.updates",
                         .keyId = "key-1",
                         .artifactDigest = package.digest,
                         .signature = std::vector<std::byte>(64U, std::byte{1})};
    const UpdateTransferResponse response{.status = 200U,
                                          .requestedUrl = package.url,
                                          .effectiveUrl = package.url,
                                          .strongEtag = "\"build-42\"",
                                          .contentLength = package.size};
    auto plan = PlanUpdateTransfer(package, response, std::nullopt);
    REQUIRE(plan.HasValue());
    auto checkpoint = AdvanceUpdateTransfer(plan.Value(), package.size);
    REQUIRE(checkpoint.HasValue());
    NativeDurableFileSystem files;
    const UpdateDownloadPaths paths{directory.root / "first/update.zip", directory.root / "first/update.checkpoint"};
    REQUIRE(SaveUpdateTransferCheckpoint(files, paths.partialFile, paths.checkpointFile, checkpoint.Value()).HasValue());
    auto staged = PrepareZipUpdateStageHttps({package,
                                              paths,
                                              directory.root / "first/candidate",
                                              {.maximumPackageBytes = 4096U, .reserveBytes = 0U},
                                              {.maximumEntries = 8U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U}},
                                             files, Verifier(), {});
    REQUIRE(staged.HasValue());
    CHECK(ReadFile(directory.root / "first/candidate/bin/game") == "game");
}

TEST_CASE("ZIP producer preserves empty declared files", "[release][update][package]") {
    TemporaryDirectory directory;
    WriteFile(directory.root / "source/bin/empty", "");
    WriteFile(directory.root / "source/bin/game", "game");
    auto inventory =
        ReleasePreSignInventory::Create(ReleaseCandidateId{42U}, {{"bin/empty", ReleaseArtifactRole::Binary, 0U, ComputeSha256({})},
                                                                  {"bin/game", ReleaseArtifactRole::Binary, 4U,
                                                                   ComputeSha256(std::as_bytes(std::span{"game", 4U}))}});
    REQUIRE(inventory.HasValue());
    UpdateZipPackageProducer producer{{.maximumEntries = 8U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U}};
    IReleasePackageProducer *producers[]{&producer};
    ReleasePackageRequest request{Selection(), inventory.Value(), directory.root / "source", directory.root / "first",
                                  "bin/game",  {"bin/game"}};
    REQUIRE(ProduceReleasePackage(request, producers).HasValue());

    mz_zip_archive archive{};
    REQUIRE(mz_zip_reader_init_file(&archive, (directory.root / "first/update.zip").string().c_str(), 0U));
    const auto file = mz_zip_reader_locate_file(&archive, "bin/empty", nullptr, 0U);
    REQUIRE(file >= 0);
    mz_zip_archive_file_stat stat{};
    REQUIRE(mz_zip_reader_file_stat(&archive, static_cast<mz_uint>(file), &stat));
    CHECK(stat.m_uncomp_size == 0U);
    mz_zip_reader_end(&archive);
}

TEST_CASE("ZIP producer refuses an existing output symlink", "[release][update][package]") {
    TemporaryDirectory directory;
    WriteFile(directory.root / "source/bin/game", "game");
    std::error_code error;
    std::filesystem::create_symlink(directory.root / "missing.zip", directory.root / "first/update.zip", error);
    if (error)
        SKIP("The platform did not permit creating a test symlink");
    auto inventory = Inventory();
    UpdateZipPackageProducer producer{{.maximumEntries = 8U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U}};
    IReleasePackageProducer *producers[]{&producer};
    ReleasePackageRequest request{Selection(), inventory, directory.root / "source", directory.root / "first", "bin/game", {"bin/game"}};
    CHECK(ProduceReleasePackage(request, producers).HasError());
    CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(directory.root / "first/update.zip")));
}
