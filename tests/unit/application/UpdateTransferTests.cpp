#include "Horo/Release/UpdateArchiveIndex.h"
#include "Horo/Release/UpdateStagedTree.h"
#include "Horo/Release/UpdateTransfer.h"
#include "Horo/Release/UpdateTransferCheckpointStore.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <utility>
#include <vector>

using namespace Horo::Release;

namespace {
    class TemporaryStage final {
    public:
        TemporaryStage()
            : path(std::filesystem::temp_directory_path() /
                   ("horo-update-stage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(path);
        }

        ~TemporaryStage() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

    void WriteStageFile(const std::filesystem::path &path, const std::string_view bytes, const bool executable = false) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
#if !defined(_WIN32)
        std::filesystem::permissions(path, executable ? std::filesystem::perms{0755} : std::filesystem::perms{0644});
#endif
    }

    class AcceptingProvider final : public Horo::Security::SignatureProvider {
    public:
        [[nodiscard]] bool Supports(Horo::Security::SignatureAlgorithm) const noexcept override {
            return true;
        }

        [[nodiscard]] Horo::Result<void> Verify(Horo::Security::SignatureAlgorithm, std::span<const std::byte>, const Horo::Sha256Digest &,
                                                std::span<const std::byte>) const override {
            return Horo::Result<void>::Success();
        }
    };

    [[nodiscard]] UpdatePackageRecord Package() {
        UpdatePackageRecord package;
        package.url = "https://updates.example.test/editor.zip";
        package.size = 100U;
        package.digest.bytes[0] = 42U;
        return package;
    }

    [[nodiscard]] UpdateTransferResponse FreshResponse(const UpdatePackageRecord &package) {
        return {.status = 200U,
                .requestedUrl = package.url,
                .effectiveUrl = package.url,
                .strongEtag = "\"release-42\"",
                .contentLength = package.size};
    }

    [[nodiscard]] UpdateTransferCheckpoint PartialCheckpoint(const UpdatePackageRecord &package) {
        auto first = PlanUpdateTransfer(package, FreshResponse(package), std::nullopt);
        REQUIRE(first.HasValue());
        auto checkpoint = AdvanceUpdateTransfer(first.Value(), 40U);
        REQUIRE(checkpoint.HasValue());
        return std::move(checkpoint).Value();
    }

    [[nodiscard]] UpdateTransferResponse RemainingResponse(const UpdatePackageRecord &package) {
        auto response = FreshResponse(package);
        response.status = 206U;
        response.contentLength = 60U;
        response.rangeStart = 40U;
        response.rangeEnd = 99U;
        response.rangeTotal = 100U;
        return response;
    }
}  // namespace

TEST_CASE("Update download capacity preflight preserves a recovery reserve", "[release][update]") {
    const auto package = Package();
    CHECK(CheckUpdateTransferSpace(package, 200U, 100U, 100U).HasValue());
    CHECK(CheckUpdateTransferSpace(package, 199U, 100U, 100U).HasError());
    CHECK(CheckUpdateTransferSpace(package, 200U, 99U, 0U).HasError());
    CHECK(CheckUpdateTransferSpace(package, 50U, 100U, 100U).HasError());
}

TEST_CASE("Update transfer admits an exact fresh body and durable progress", "[release][update]") {
    const auto package = Package();
    auto response = FreshResponse(package);
    auto plan = PlanUpdateTransfer(package, response, std::nullopt);
    REQUIRE(plan.HasValue());
    CHECK(plan.Value().writeOffset == 0U);
    CHECK(plan.Value().responseBytes == package.size);
    CHECK(plan.Value().resumable);
    auto partial = AdvanceUpdateTransfer(plan.Value(), 40U);
    REQUIRE(partial.HasValue());
    CHECK(partial.Value().durableBytes == 40U);
    CHECK(AdvanceUpdateTransfer(plan.Value(), 101U).HasError());
    response.effectiveUrl = "https://mirror.example.test/editor.zip";
    CHECK(PlanUpdateTransfer(package, response, std::nullopt).HasError());
    response = FreshResponse(package);
    response.strongEtag = "W/\"release-42\"";
    CHECK(PlanUpdateTransfer(package, response, std::nullopt).HasError());
}

TEST_CASE("Update resume accepts only an exact strong validator and byte range", "[release][update]") {
    const auto package = Package();
    const auto checkpoint = PartialCheckpoint(package);
    auto response = RemainingResponse(package);
    auto plan = PlanUpdateTransfer(package, response, checkpoint);
    REQUIRE(plan.HasValue());
    CHECK(plan.Value().writeOffset == 40U);
    CHECK(plan.Value().responseBytes == 60U);
    auto complete = AdvanceUpdateTransfer(plan.Value(), 60U);
    REQUIRE(complete.HasValue());
    CHECK(complete.Value().durableBytes == 100U);
    response.strongEtag = "\"changed\"";
    CHECK(PlanUpdateTransfer(package, response, checkpoint).HasError());
    response = RemainingResponse(package);
    response.rangeStart = 39U;
    CHECK(PlanUpdateTransfer(package, response, checkpoint).HasError());
    response = RemainingResponse(package);
    response.status = 200U;
    CHECK(PlanUpdateTransfer(package, response, checkpoint).HasError());
}

TEST_CASE("Update resume rejects stale package and checkpoint identities", "[release][update]") {
    auto package = Package();
    auto checkpoint = PartialCheckpoint(package);
    const auto response = RemainingResponse(package);
    package.digest.bytes[0] = 43U;
    CHECK(PlanUpdateTransfer(package, response, checkpoint).HasError());
    package = Package();
    checkpoint.effectiveUrl = "https://mirror.example.test/editor.zip";
    CHECK(PlanUpdateTransfer(package, response, checkpoint).HasError());
    checkpoint = PartialCheckpoint(package);
    checkpoint.durableBytes = package.size;
    CHECK(PlanUpdateTransfer(package, response, checkpoint).HasError());
}

TEST_CASE("Partial download checkpoints parse only canonical bounded schema", "[release][update]") {
    const auto checkpoint = PartialCheckpoint(Package());
    auto serialized = SerializeUpdateTransferCheckpoint(checkpoint);
    REQUIRE(serialized.HasValue());
    auto parsed = ParseUpdateTransferCheckpoint(serialized.Value());
    REQUIRE(parsed.HasValue());
    CHECK(parsed.Value().durableBytes == 40U);
    CHECK(parsed.Value().packageDigest == checkpoint.packageDigest);
    auto changed = serialized.Value();
    changed.replace(0U, 23U, "horo-update-transfer-v2");
    CHECK(ParseUpdateTransferCheckpoint(changed).HasError());
    changed = serialized.Value();
    changed.insert(changed.find("\n40\n") + 1U, "0");
    CHECK(ParseUpdateTransferCheckpoint(changed).HasError());
    CHECK(ParseUpdateTransferCheckpoint(std::string(4601U, 'x')).HasError());
    auto invalid = checkpoint;
    invalid.durableBytes = invalid.packageSize + 1U;
    CHECK(SerializeUpdateTransferCheckpoint(invalid).HasError());
}

TEST_CASE("Completed update bytes require the signed package hash and publisher", "[release][update]") {
    std::array<std::byte, 100U> bytes{};
    auto package = Package();
    package.digest = Horo::ComputeSha256(bytes);
    package.signature = {.publisherId = "com.horo.updates",
                         .keyId = "key-1",
                         .artifactDigest = package.digest,
                         .signature = std::vector<std::byte>(64U, std::byte{1})};
    auto roots = std::make_shared<Horo::Security::TrustedRootStore>();
    std::vector<std::byte> key(65U, std::byte{1});
    key.front() = std::byte{0x04};
    REQUIRE(roots->Add({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = std::move(key)}).HasValue());
    const Horo::Security::ArtifactVerifier verifier{std::make_shared<AcceptingProvider>(), roots};
    auto checkpoint = PartialCheckpoint(package);
    checkpoint.durableBytes = package.size;
    CHECK(VerifyCompletedUpdateTransfer(package, checkpoint, bytes, verifier).HasValue());
    TemporaryStage stage;
    const auto partialFile = stage.path / "package.partial";
    WriteStageFile(partialFile, std::string(100U, '\0'));
    CHECK(VerifyCompletedUpdateTransfer(package, checkpoint, partialFile, verifier).HasValue());
    WriteStageFile(partialFile, std::string(99U, '\0'));
    CHECK(VerifyCompletedUpdateTransfer(package, checkpoint, partialFile, verifier).HasError());
    WriteStageFile(partialFile, std::string(100U, 'x'));
    CHECK(VerifyCompletedUpdateTransfer(package, checkpoint, partialFile, verifier).HasError());
    std::filesystem::remove(partialFile);
    std::error_code linkError;
    std::filesystem::create_symlink(stage.path / "missing", partialFile, linkError);
    if (!linkError)
        CHECK(VerifyCompletedUpdateTransfer(package, checkpoint, partialFile, verifier).HasError());
    bytes[0] = std::byte{1};
    CHECK(VerifyCompletedUpdateTransfer(package, checkpoint, bytes, verifier).HasError());
    bytes[0] = std::byte{};
    checkpoint.durableBytes = package.size - 1U;
    CHECK(VerifyCompletedUpdateTransfer(package, checkpoint, bytes, verifier).HasError());
}

TEST_CASE("Large private update files are verified across streaming blocks", "[release][update]") {
    const std::string payload(128U * 1024U + 1U, 'p');
    auto package = Package();
    package.size = payload.size();
    package.digest = Horo::ComputeSha256(std::as_bytes(std::span{payload}));
    package.signature = {.publisherId = "com.horo.updates",
                         .keyId = "key-1",
                         .artifactDigest = package.digest,
                         .signature = std::vector<std::byte>(64U, std::byte{1})};
    auto roots = std::make_shared<Horo::Security::TrustedRootStore>();
    std::vector<std::byte> key(65U, std::byte{1});
    key.front() = std::byte{0x04};
    REQUIRE(roots->Add({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = std::move(key)}).HasValue());
    const Horo::Security::ArtifactVerifier verifier{std::make_shared<AcceptingProvider>(), roots};
    auto checkpoint = PartialCheckpoint(package);
    checkpoint.durableBytes = package.size;
    TemporaryStage stage;
    const auto partialFile = stage.path / "package.partial";
    WriteStageFile(partialFile, payload);
    CHECK(VerifyCompletedUpdateTransfer(package, checkpoint, partialFile, verifier).HasValue());
    auto changed = payload;
    changed[64U * 1024U] = 'x';
    WriteStageFile(partialFile, changed);
    CHECK(VerifyCompletedUpdateTransfer(package, checkpoint, partialFile, verifier).HasError());
}

TEST_CASE("Update archive preflight accepts bounded regular content", "[release][update]") {
    constexpr UpdateArchiveLimits limits{.maximumEntries = 4U, .maximumFileBytes = 12U, .maximumExpandedBytes = 20U};
    const std::array entries{UpdateArchiveEntry{"bin", UpdateArchiveEntryKind::Directory, 0U},
                             UpdateArchiveEntry{"bin/horo", UpdateArchiveEntryKind::File, 12U},
                             UpdateArchiveEntry{"README.txt", UpdateArchiveEntryKind::File, 8U}};
    CHECK(ValidateUpdateArchiveIndex(entries, limits).HasValue());
    CHECK(ValidateUpdateArchiveIndex(entries, {.maximumEntries = 2U, .maximumFileBytes = 12U, .maximumExpandedBytes = 20U}).HasError());
    CHECK(ValidateUpdateArchiveIndex(entries, {.maximumEntries = 4U, .maximumFileBytes = 11U, .maximumExpandedBytes = 20U}).HasError());
    CHECK(ValidateUpdateArchiveIndex(entries, {.maximumEntries = 4U, .maximumFileBytes = 12U, .maximumExpandedBytes = 19U}).HasError());
}

TEST_CASE("Update archive preflight rejects traversal, links, and ambiguous names", "[release][update]") {
    constexpr UpdateArchiveLimits limits{.maximumEntries = 4U, .maximumFileBytes = 12U, .maximumExpandedBytes = 20U};
    const std::array unsafePath{UpdateArchiveEntry{"../horo", UpdateArchiveEntryKind::File, 1U}};
    const std::array link{UpdateArchiveEntry{"horo", UpdateArchiveEntryKind::SymbolicLink, 1U}};
    const std::array duplicate{UpdateArchiveEntry{"Bin/horo", UpdateArchiveEntryKind::File, 1U},
                               UpdateArchiveEntry{"bin/HORO", UpdateArchiveEntryKind::File, 1U}};
    const std::array fileParent{UpdateArchiveEntry{"bin", UpdateArchiveEntryKind::File, 1U},
                                UpdateArchiveEntry{"bin/horo", UpdateArchiveEntryKind::File, 1U}};
    const std::array fileChild{UpdateArchiveEntry{"bin/horo", UpdateArchiveEntryKind::File, 1U},
                               UpdateArchiveEntry{"bin", UpdateArchiveEntryKind::File, 1U}};
    const std::array nonemptyDirectory{UpdateArchiveEntry{"bin", UpdateArchiveEntryKind::Directory, 1U}};
    CHECK(ValidateUpdateArchiveIndex(unsafePath, limits).HasError());
    CHECK(ValidateUpdateArchiveIndex(link, limits).HasError());
    CHECK(ValidateUpdateArchiveIndex(duplicate, limits).HasError());
    CHECK(ValidateUpdateArchiveIndex(fileParent, limits).HasError());
    CHECK(ValidateUpdateArchiveIndex(fileChild, limits).HasError());
    CHECK(ValidateUpdateArchiveIndex(nonemptyDirectory, limits).HasError());
}

TEST_CASE("Update staging verifies exact file bytes and rejects undeclared tree entries", "[release][update]") {
    TemporaryStage stage;
    constexpr std::string_view executable = "editor bytes";
    constexpr std::string_view notes = "release notes";
    WriteStageFile(stage.path / "bin/editor", executable, true);
    WriteStageFile(stage.path / "docs/notes.txt", notes);
    const std::array files{UpdateStagedFile{"bin/editor", executable.size(), Horo::ComputeSha256(std::as_bytes(std::span{executable})),
                                            UpdateFileMode::Executable, UpdateFileRole::Entrypoint},
                           UpdateStagedFile{"docs/notes.txt", notes.size(), Horo::ComputeSha256(std::as_bytes(std::span{notes}))}};
    constexpr UpdateArchiveLimits limits{.maximumEntries = 4U, .maximumFileBytes = 32U, .maximumExpandedBytes = 64U};
    REQUIRE(VerifyUpdateStagedTree(stage.path, files, limits).HasValue());
#if !defined(_WIN32)
    std::filesystem::permissions(stage.path / "bin/editor", std::filesystem::perms{0644});
    CHECK(VerifyUpdateStagedTree(stage.path, files, limits).HasError());
    std::filesystem::permissions(stage.path / "bin/editor", std::filesystem::perms{0755});
#endif

    WriteStageFile(stage.path / "bin/editor", "wrong bytes");
    CHECK(VerifyUpdateStagedTree(stage.path, files, limits).HasError());
    WriteStageFile(stage.path / "bin/editor", executable, true);
    WriteStageFile(stage.path / "extra.txt", "undeclared");
    CHECK(VerifyUpdateStagedTree(stage.path, files, limits).HasError());
    std::filesystem::remove(stage.path / "extra.txt");
    std::filesystem::create_directories(stage.path / "unused");
    CHECK(VerifyUpdateStagedTree(stage.path, files, limits).HasError());
}

TEST_CASE("Update staging rejects missing, linked, and oversized content", "[release][update]") {
    TemporaryStage stage;
    constexpr std::string_view bytes = "editor bytes";
    WriteStageFile(stage.path / "bin/editor", bytes, true);
    const std::array files{UpdateStagedFile{"bin/editor", bytes.size(), Horo::ComputeSha256(std::as_bytes(std::span{bytes})),
                                            UpdateFileMode::Executable, UpdateFileRole::Entrypoint}};
    constexpr UpdateArchiveLimits limits{.maximumEntries = 2U, .maximumFileBytes = 32U, .maximumExpandedBytes = 32U};
    REQUIRE(VerifyUpdateStagedTree(stage.path, files, limits).HasValue());
    CHECK(
        VerifyUpdateStagedTree(stage.path, files, {.maximumEntries = 2U, .maximumFileBytes = 2U, .maximumExpandedBytes = 32U}).HasError());
    std::filesystem::remove(stage.path / "bin/editor");
    CHECK(VerifyUpdateStagedTree(stage.path, files, limits).HasError());
    std::error_code linkError;
    std::filesystem::create_symlink(stage.path / "outside", stage.path / "bin/editor", linkError);
    if (!linkError)
        CHECK(VerifyUpdateStagedTree(stage.path, files, limits).HasError());
}

TEST_CASE("Update staging rejects hard links to files outside the private tree", "[release][update]") {
    TemporaryStage stage;
    constexpr std::string_view bytes = "editor bytes";
    const auto outside = stage.path.parent_path() / (stage.path.filename().string() + "-outside");
    WriteStageFile(outside, bytes);
    std::filesystem::create_directories(stage.path / "bin");
    std::error_code linkError;
    std::filesystem::create_hard_link(outside, stage.path / "bin/editor", linkError);
    if (!linkError) {
        const std::array files{UpdateStagedFile{"bin/editor", bytes.size(), Horo::ComputeSha256(std::as_bytes(std::span{bytes})),
                                                UpdateFileMode::Executable, UpdateFileRole::Entrypoint}};
        constexpr UpdateArchiveLimits limits{.maximumEntries = 2U, .maximumFileBytes = 32U, .maximumExpandedBytes = 32U};
        CHECK(VerifyUpdateStagedTree(stage.path, files, limits).HasError());
    }
    std::filesystem::remove(outside);
}

TEST_CASE("Update file inventory builder sorts paths and reserves its own archive entry", "[release][update]") {
    constexpr std::string_view first = "a";
    constexpr std::string_view second = "bb";
    const auto firstDigest = Horo::ComputeSha256(std::as_bytes(std::span{first}));
    const auto secondDigest = Horo::ComputeSha256(std::as_bytes(std::span{second}));
    const std::array files{UpdateStagedFile{"bin/b", second.size(), secondDigest},
                           UpdateStagedFile{"bin/a", first.size(), firstDigest, UpdateFileMode::Executable, UpdateFileRole::Entrypoint}};
    constexpr UpdateArchiveLimits limits{.maximumEntries = 3U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U};
    auto built = BuildCanonicalUpdateFileInventory(files, limits);
    REQUIRE(built.HasValue());
    CHECK(built.Value() == std::string{UpdateFileInventoryHeader} + "bin/a\t1\t" + Horo::FormatSha256(firstDigest) +
                               "\t0755\tentrypoint\n" + "bin/b\t2\t" + Horo::FormatSha256(secondDigest) + "\t0644\tcontent\n");
    auto parsed = ParseCanonicalUpdateFileInventory(built.Value(), limits);
    REQUIRE(parsed.HasValue());
    REQUIRE(parsed.Value().size() == 2U);
    CHECK(parsed.Value().front().role == UpdateFileRole::Entrypoint);
    CHECK(parsed.Value().front().mode == UpdateFileMode::Executable);
    CHECK(ParseCanonicalUpdateFileInventory("horo-update-files-v1\n", limits).HasError());
    auto altered = built.Value();
    altered.replace(altered.find("\t0755\tentrypoint"), std::string{"\t0755\tentrypoint"}.size(), "\t0644\tentrypoint");
    CHECK(ParseCanonicalUpdateFileInventory(altered, limits).HasError());
    altered = built.Value();
    altered.replace(altered.find("\t0644\tcontent"), std::string{"\t0644\tcontent"}.size(), "\t0755\tentrypoint");
    CHECK(ParseCanonicalUpdateFileInventory(altered, limits).HasError());
    CHECK(BuildCanonicalUpdateFileInventory(files, {.maximumEntries = 2U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U})
              .HasError());
    const std::array reserved{UpdateStagedFile{std::string{UpdateFileInventoryPath}, 1U, firstDigest}};
    CHECK(BuildCanonicalUpdateFileInventory(reserved, limits).HasError());
    const std::array collision{UpdateStagedFile{"bin/a", 1U, firstDigest}, UpdateStagedFile{"Bin/A", 1U, firstDigest}};
    CHECK(BuildCanonicalUpdateFileInventory(collision, limits).HasError());
}

TEST_CASE("Private transfer checkpoint recovery requires exact durable partial bytes", "[release][update]") {
    TemporaryStage stage;
    const auto partialPath = stage.path / "package partial";
    const auto checkpointPath = stage.path / "package checkpoint";
    auto empty = LoadUpdateTransferCheckpoint(partialPath, checkpointPath);
    REQUIRE(empty.HasValue());
    CHECK_FALSE(empty.Value().has_value());

    WriteStageFile(partialPath, std::string(40U, 'p'));
    CHECK(LoadUpdateTransferCheckpoint(partialPath, checkpointPath).HasError());
    Horo::NativeDurableFileSystem files;
    const auto checkpoint = PartialCheckpoint(Package());
    REQUIRE(SaveUpdateTransferCheckpoint(files, partialPath, checkpointPath, checkpoint).HasValue());
    auto recovered = LoadUpdateTransferCheckpoint(partialPath, checkpointPath);
    REQUIRE(recovered.HasValue());
    REQUIRE(recovered.Value().has_value());
    CHECK(recovered.Value()->durableBytes == 40U);
    CHECK(recovered.Value()->packageDigest == checkpoint.packageDigest);

    WriteStageFile(partialPath, std::string(39U, 'p'));
    CHECK(LoadUpdateTransferCheckpoint(partialPath, checkpointPath).HasError());
    CHECK(SaveUpdateTransferCheckpoint(files, partialPath, checkpointPath, checkpoint).HasError());
    WriteStageFile(partialPath, std::string(40U, 'p'));
    WriteStageFile(checkpointPath, "horo-update-transfer-v2\n");
    CHECK(LoadUpdateTransferCheckpoint(partialPath, checkpointPath).HasError());
    WriteStageFile(checkpointPath, std::string(4601U, 'x'));
    CHECK(LoadUpdateTransferCheckpoint(partialPath, checkpointPath).HasError());
}

TEST_CASE("Durable private chunks resume from the exact recovered checkpoint", "[release][update]") {
    TemporaryStage stage;
    const auto partialPath = stage.path / "package partial";
    const auto checkpointPath = stage.path / "package checkpoint";
    const std::string payload(100U, 'p');
    auto package = Package();
    package.digest = Horo::ComputeSha256(std::as_bytes(std::span{payload}));
    package.signature = {.publisherId = "com.horo.updates",
                         .keyId = "key-1",
                         .artifactDigest = package.digest,
                         .signature = std::vector<std::byte>(64U, std::byte{1})};
    Horo::NativeDurableFileSystem files;
    auto fresh = PlanUpdateTransfer(package, FreshResponse(package), std::nullopt);
    REQUIRE(fresh.HasValue());
    REQUIRE(files.AppendPrivateDurable(partialPath, 0U, std::as_bytes(std::span{payload.data(), 40U})).HasValue());
    auto firstCheckpoint = AdvanceUpdateTransfer(fresh.Value(), 40U);
    REQUIRE(firstCheckpoint.HasValue());
    REQUIRE(SaveUpdateTransferCheckpoint(files, partialPath, checkpointPath, firstCheckpoint.Value()).HasValue());

    auto recovered = LoadUpdateTransferCheckpoint(partialPath, checkpointPath);
    REQUIRE(recovered.HasValue());
    REQUIRE(recovered.Value().has_value());
    auto resume = PlanUpdateTransfer(package, RemainingResponse(package), recovered.Value());
    REQUIRE(resume.HasValue());
    CHECK(files.AppendPrivateDurable(partialPath, 39U, std::as_bytes(std::span{payload.data() + 40U, 60U})).HasError());
    REQUIRE(files.AppendPrivateDurable(partialPath, resume.Value().writeOffset, std::as_bytes(std::span{payload.data() + 40U, 60U}))
                .HasValue());
    auto complete = AdvanceUpdateTransfer(resume.Value(), 60U);
    REQUIRE(complete.HasValue());
    REQUIRE(SaveUpdateTransferCheckpoint(files, partialPath, checkpointPath, complete.Value()).HasValue());
    auto roots = std::make_shared<Horo::Security::TrustedRootStore>();
    std::vector<std::byte> key(65U, std::byte{1});
    key.front() = std::byte{0x04};
    REQUIRE(roots->Add({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = std::move(key)}).HasValue());
    const Horo::Security::ArtifactVerifier verifier{std::make_shared<AcceptingProvider>(), roots};
    CHECK(VerifyCompletedUpdateTransfer(package, complete.Value(), partialPath, verifier).HasValue());
}

TEST_CASE("Private checkpoint publication rejects aliases of package bytes", "[release][update]") {
    TemporaryStage stage;
    const auto partialPath = stage.path / "package.partial";
    const auto checkpointPath = stage.path / "package.checkpoint";
    WriteStageFile(partialPath, std::string(40U, 'p'));
    const auto checkpoint = PartialCheckpoint(Package());
    Horo::NativeDurableFileSystem files;
    std::error_code linkError;
    std::filesystem::create_hard_link(partialPath, checkpointPath, linkError);
    if (!linkError) {
        CHECK(LoadUpdateTransferCheckpoint(partialPath, checkpointPath).HasError());
        CHECK(SaveUpdateTransferCheckpoint(files, partialPath, checkpointPath, checkpoint).HasError());
        std::filesystem::remove(checkpointPath);
    }
    auto prepared = checkpointPath;
    prepared += ".next";
    linkError.clear();
    std::filesystem::create_hard_link(partialPath, prepared, linkError);
    if (!linkError) {
        CHECK(SaveUpdateTransferCheckpoint(files, partialPath, checkpointPath, checkpoint).HasError());
        CHECK(std::filesystem::file_size(partialPath) == 40U);
    }
}
