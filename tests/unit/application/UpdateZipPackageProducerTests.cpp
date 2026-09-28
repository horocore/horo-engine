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
}  // namespace

TEST_CASE("ZIP producer emits deterministic exact bytes and the staging inventory", "[release][update][package]") {
    TemporaryDirectory directory;
    WriteFile(directory.root / "source/bin/game", "game");
    auto inventory = Inventory();
    UpdateZipPackageProducer producer{{.maximumEntries = 8U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U}};
    IReleasePackageProducer *producers[]{&producer};
    ReleasePackageRequest first{Selection(), inventory, directory.root / "source", directory.root / "first"};
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
    auto declared = BuildCanonicalUpdateFileInventory(std::array{UpdateStagedFile{"bin/game", 4U, inventory.Artifacts().front().digest}},
                                                      {.maximumEntries = 8U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U});
    REQUIRE(declared.HasValue());
    std::string inventoryBytes(declared.Value().size(), '\0');
    REQUIRE(mz_zip_reader_extract_file_to_mem(&archive, UpdateFileInventoryPath.data(), inventoryBytes.data(), inventoryBytes.size(), 0U));
    CHECK(inventoryBytes == declared.Value());
    std::string game(4U, '\0');
    REQUIRE(mz_zip_reader_extract_file_to_mem(&archive, "bin/game", game.data(), game.size(), 0U));
    CHECK(game == "game");
    mz_zip_reader_end(&archive);

    ReleasePackageRequest second{Selection(), inventory, directory.root / "source", directory.root / "second"};
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
    ReleasePackageRequest request{Selection(), inventory, directory.root / "source", directory.root / "first"};
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
    auto inventory =
        ReleasePreSignInventory::Create(ReleaseCandidateId{42U}, {{"bin/empty", ReleaseArtifactRole::Binary, 0U, ComputeSha256({})}});
    REQUIRE(inventory.HasValue());
    UpdateZipPackageProducer producer{{.maximumEntries = 8U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U}};
    IReleasePackageProducer *producers[]{&producer};
    ReleasePackageRequest request{Selection(), inventory.Value(), directory.root / "source", directory.root / "first"};
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
    ReleasePackageRequest request{Selection(), inventory, directory.root / "source", directory.root / "first"};
    CHECK(ProduceReleasePackage(request, producers).HasError());
    CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(directory.root / "first/update.zip")));
}
