#include "Horo/Release/UpdateDownloadSession.h"
#include "Horo/Release/UpdateHttpDownload.h"
#include "Horo/Release/UpdateStageReady.h"
#include "Horo/Release/UpdateTransferCheckpointStore.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <miniz.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Horo::Release;

namespace {
    class TemporaryStage final {
    public:
        TemporaryStage()
            : path(std::filesystem::temp_directory_path() /
                   ("horo-update-download-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(path);
        }

        ~TemporaryStage() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

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

    [[nodiscard]] UpdatePackageRecord Package(const std::string &payload) {
        UpdatePackageRecord package;
        package.url = "https://updates.example.test/editor.zip";
        package.size = payload.size();
        package.digest = Horo::ComputeSha256(std::as_bytes(std::span{payload}));
        package.signature = {.publisherId = "com.horo.updates",
                             .keyId = "key-1",
                             .artifactDigest = package.digest,
                             .signature = std::vector<std::byte>(64U, std::byte{1})};
        return package;
    }

    [[nodiscard]] Horo::Security::ArtifactVerifier Verifier() {
        auto roots = std::make_shared<Horo::Security::TrustedRootStore>();
        std::vector<std::byte> key(65U, std::byte{1});
        key.front() = std::byte{0x04};
        REQUIRE(roots->Add({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = std::move(key)}).HasValue());
        return {std::make_shared<AcceptingProvider>(), std::move(roots)};
    }

    [[nodiscard]] UpdateTransferResponse Fresh(const UpdatePackageRecord &package) {
        return {.status = 200U,
                .requestedUrl = package.url,
                .effectiveUrl = package.url,
                .strongEtag = "\"release-42\"",
                .contentLength = package.size};
    }

    [[nodiscard]] UpdateTransferResponse Resume(const UpdatePackageRecord &package, const std::uint64_t start) {
        return {.status = 206U,
                .requestedUrl = package.url,
                .effectiveUrl = package.url,
                .strongEtag = "\"release-42\"",
                .contentLength = package.size - start,
                .rangeStart = start,
                .rangeEnd = package.size - 1U,
                .rangeTotal = package.size};
    }

    [[nodiscard]] UpdateDownloadPaths Paths(const TemporaryStage &stage) {
        return {stage.path / "package.partial", stage.path / "package.checkpoint"};
    }

    constexpr UpdateDownloadLimits Limits{.maximumPackageBytes = 1024U, .reserveBytes = 0U};

    [[nodiscard]] std::string ZipArchive(const std::string_view name, const std::string_view content,
                                         const std::optional<std::string_view> declaredContent = std::nullopt,
                                         const std::optional<std::pair<std::string_view, std::string_view>> extraFile = std::nullopt,
                                         const bool includeInventory = true, const bool uppercaseDigest = false,
                                         const bool declareExtra = false) {
        mz_zip_archive writer{};
        REQUIRE(mz_zip_writer_init_heap(&writer, 0U, 0U));
        const auto digest = Horo::ComputeSha256(std::as_bytes(std::span{declaredContent.value_or(content)}));
        std::string digestText = Horo::FormatSha256(digest);
        if (uppercaseDigest) {
            for (char &character : digestText) {
                if (character >= 'a' && character <= 'f')
                    character = static_cast<char>(character - 'a' + 'A');
            }
        }
        std::string inventory = std::string{UpdateFileInventoryHeader} + std::string{name} + '\t' + std::to_string(content.size()) + '\t' +
                                digestText + "\t0755\tentrypoint\n";
        if (extraFile && declareExtra) {
            const auto extraDigest = Horo::ComputeSha256(std::as_bytes(std::span{extraFile->second}));
            inventory += std::string{extraFile->first} + '\t' + std::to_string(extraFile->second.size()) + '\t' +
                         Horo::FormatSha256(extraDigest) + "\t0644\tcontent\n";
        }
        if (includeInventory)
            REQUIRE(
                mz_zip_writer_add_mem(&writer, UpdateFileInventoryPath.data(), inventory.data(), inventory.size(), MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_add_mem(&writer, std::string{name}.c_str(), content.data(), content.size(), MZ_DEFAULT_COMPRESSION));
        if (extraFile)
            REQUIRE(mz_zip_writer_add_mem(&writer, std::string{extraFile->first}.c_str(), extraFile->second.data(),
                                          extraFile->second.size(), MZ_DEFAULT_COMPRESSION));
        void *bytes = nullptr;
        std::size_t size = 0U;
        REQUIRE(mz_zip_writer_finalize_heap_archive(&writer, &bytes, &size));
        std::string archive{static_cast<const char *>(bytes), size};
        mz_free(bytes);
        mz_zip_writer_end(&writer);
        return archive;
    }

    [[nodiscard]] UpdateTransferCheckpoint CompleteCheckpoint(const UpdatePackageRecord &package) {
        auto plan = PlanUpdateTransfer(package, Fresh(package), std::nullopt);
        REQUIRE(plan.HasValue());
        auto checkpoint = AdvanceUpdateTransfer(plan.Value(), package.size);
        REQUIRE(checkpoint.HasValue());
        return std::move(checkpoint).Value();
    }
}  // namespace

TEST_CASE("Private download session checkpoints and verifies exact complete bytes", "[release][update]") {
    TemporaryStage stage;
    const std::string payload(100U, 'p');
    const auto package = Package(payload);
    Horo::NativeDurableFileSystem files;
    auto started = UpdateDownloadSession::Begin(package, Fresh(package), Paths(stage), Limits, files, {});
    REQUIRE(started.HasValue());
    auto session = std::move(started).Value();
    auto first = session.Append(std::as_bytes(std::span{payload.data(), 40U}));
    REQUIRE(first.HasValue());
    CHECK(first.Value().durableBytes == 40U);
    auto second = session.Append(std::as_bytes(std::span{payload.data() + 40U, 60U}));
    REQUIRE(second.HasValue());
    CHECK(second.Value().durableBytes == package.size);
    auto complete = session.Finish(Verifier());
    REQUIRE(complete.HasValue());
    CHECK(complete.Value().durableBytes == package.size);
    CHECK(session.Append(std::as_bytes(std::span{payload.data(), 1U})).HasError());
    auto recovered = LoadUpdateTransferCheckpoint(Paths(stage).partialFile, Paths(stage).checkpointFile);
    REQUIRE(recovered.HasValue());
    REQUIRE(recovered.Value().has_value());
    CHECK(recovered.Value()->durableBytes == package.size);
}

TEST_CASE("Cancelled private download resumes only with matching response identity", "[release][update]") {
    TemporaryStage stage;
    const std::string payload(100U, 'p');
    const auto package = Package(payload);
    Horo::NativeDurableFileSystem files;
    Horo::CancellationSource cancellation;
    auto started = UpdateDownloadSession::Begin(package, Fresh(package), Paths(stage), Limits, files, cancellation.Token());
    REQUIRE(started.HasValue());
    auto session = std::move(started).Value();
    REQUIRE(session.Append(std::as_bytes(std::span{payload.data(), 40U})).HasValue());
    cancellation.RequestCancellation();
    CHECK(session.Append(std::as_bytes(std::span{payload.data() + 40U, 1U})).HasError());
    CHECK(std::filesystem::file_size(Paths(stage).partialFile) == 40U);

    auto changed = Resume(package, 40U);
    changed.strongEtag = "\"changed\"";
    CHECK(UpdateDownloadSession::Begin(package, changed, Paths(stage), Limits, files, {}).HasError());
    auto resumed = UpdateDownloadSession::Begin(package, Resume(package, 40U), Paths(stage), Limits, files, {});
    REQUIRE(resumed.HasValue());
    auto resumedSession = std::move(resumed).Value();
    REQUIRE(resumedSession.Append(std::as_bytes(std::span{payload.data() + 40U, 60U})).HasValue());
    CHECK(resumedSession.Finish(Verifier()).HasValue());
}

TEST_CASE("Private download rejects excess, shortage, invalid paths, and insufficient capacity", "[release][update]") {
    TemporaryStage stage;
    const std::string payload(100U, 'p');
    const auto package = Package(payload);
    Horo::NativeDurableFileSystem files;
    CHECK(UpdateDownloadSession::Begin(package, Fresh(package), Paths(stage), {.maximumPackageBytes = 99U, .reserveBytes = 0U}, files, {})
              .HasError());
    CHECK(UpdateDownloadSession::Begin(package, Fresh(package), Paths(stage),
                                       {.maximumPackageBytes = 100U, .reserveBytes = std::numeric_limits<std::uint64_t>::max()}, files, {})
              .HasError());
    const UpdateDownloadPaths outside{stage.path / "package.partial", stage.path.parent_path() / "package.checkpoint"};
    CHECK(UpdateDownloadSession::Begin(package, Fresh(package), outside, Limits, files, {}).HasError());
    const UpdateDownloadPaths traversal{stage.path / "inner/../package.partial", stage.path / "inner/../package.checkpoint"};
    CHECK(UpdateDownloadSession::Begin(package, Fresh(package), traversal, Limits, files, {}).HasError());

    auto started = UpdateDownloadSession::Begin(package, Fresh(package), Paths(stage), Limits, files, {});
    REQUIRE(started.HasValue());
    auto session = std::move(started).Value();
    const std::string oversized(101U, 'x');
    CHECK(session.Append(std::as_bytes(std::span{oversized})).HasError());
    CHECK_FALSE(std::filesystem::exists(Paths(stage).partialFile));

    auto retry = UpdateDownloadSession::Begin(package, Fresh(package), Paths(stage), Limits, files, {});
    REQUIRE(retry.HasValue());
    auto retrySession = std::move(retry).Value();
    REQUIRE(retrySession.Append(std::as_bytes(std::span{payload.data(), 40U})).HasValue());
    CHECK(retrySession.Finish(Verifier()).HasError());
}

TEST_CASE("Private download never authenticates a changed package body", "[release][update]") {
    TemporaryStage stage;
    const std::string expected(100U, 'p');
    const auto package = Package(expected);
    auto changed = expected;
    changed.back() = 'x';
    Horo::NativeDurableFileSystem files;
    auto started = UpdateDownloadSession::Begin(package, Fresh(package), Paths(stage), Limits, files, {});
    REQUIRE(started.HasValue());
    auto session = std::move(started).Value();
    REQUIRE(session.Append(std::as_bytes(std::span{changed})).HasValue());
    CHECK(session.Finish(Verifier()).HasError());
    auto recovered = LoadUpdateTransferCheckpoint(Paths(stage).partialFile, Paths(stage).checkpointFile);
    REQUIRE(recovered.HasValue());
    REQUIRE(recovered.Value().has_value());
    CHECK(recovered.Value()->durableBytes == package.size);
}

TEST_CASE("HTTPS adapter authenticates a complete durable checkpoint without another request", "[release][update]") {
    TemporaryStage stage;
    const std::string payload(100U, 'p');
    const auto package = Package(payload);
    Horo::NativeDurableFileSystem files;
    auto started = UpdateDownloadSession::Begin(package, Fresh(package), Paths(stage), Limits, files, {});
    REQUIRE(started.HasValue());
    auto session = std::move(started).Value();
    REQUIRE(session.Append(std::as_bytes(std::span{payload})).HasValue());
    REQUIRE(session.Finish(Verifier()).HasValue());

    const auto recovered = DownloadUpdatePackageHttps({package, Paths(stage), Limits}, files, Verifier(), {});
    REQUIRE(recovered.HasValue());
    CHECK(recovered.Value().durableBytes == package.size);
}

TEST_CASE("HTTPS adapter rejects invalid policy and cancellation before opening transport", "[release][update]") {
    TemporaryStage stage;
    const std::string payload(100U, 'p');
    auto package = Package(payload);
    Horo::NativeDurableFileSystem files;
    package.url = "http://updates.example.test/editor.zip";
    CHECK(DownloadUpdatePackageHttps({package, Paths(stage), Limits}, files, Verifier(), {}).HasError());
    package.url = "https://updates.example.test/editor.zip";
    CHECK(DownloadUpdatePackageHttps({package, Paths(stage), Limits, {.connectTimeoutSeconds = 15U, .requestTimeoutSeconds = 14U}}, files,
                                     Verifier(), {})
              .HasError());
    Horo::CancellationSource cancellation;
    cancellation.RequestCancellation();
    CHECK(DownloadUpdatePackageHttps({package, Paths(stage), Limits}, files, Verifier(), cancellation.Token()).HasError());
    CHECK_FALSE(std::filesystem::exists(Paths(stage).partialFile));
}

TEST_CASE("Verified stage publishes a durable marker only for the exact private tree", "[release][update]") {
    TemporaryStage stage;
    const std::string payload(100U, 'p');
    const auto package = Package(payload);
    Horo::NativeDurableFileSystem files;
    auto started = UpdateDownloadSession::Begin(package, Fresh(package), Paths(stage), Limits, files, {});
    REQUIRE(started.HasValue());
    auto session = std::move(started).Value();
    REQUIRE(session.Append(std::as_bytes(std::span{payload})).HasValue());
    auto checkpoint = session.Finish(Verifier());
    REQUIRE(checkpoint.HasValue());

    const auto root = stage.path / "candidate";
    std::filesystem::create_directories(root / "bin");
    const std::string executable = "verified editor";
    {
        std::ofstream output(root / "bin/editor", std::ios::binary);
        output << executable;
    }
#if !defined(_WIN32)
    std::filesystem::permissions(root / "bin/editor", std::filesystem::perms{0755});
#endif
    const std::array inventory{UpdateStagedFile{"bin/editor", executable.size(), Horo::ComputeSha256(std::as_bytes(std::span{executable})),
                                                UpdateFileMode::Executable, UpdateFileRole::Entrypoint}};
    constexpr UpdateArchiveLimits archiveLimits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U};
    auto published = PublishVerifiedUpdateStage({package, checkpoint.Value(), Paths(stage).partialFile, root, inventory, archiveLimits},
                                                files, Verifier(), {});
    REQUIRE(published.HasValue());
    CHECK(std::filesystem::is_regular_file(published.Value()));

    {
        std::ofstream output(root / "bin/editor", std::ios::binary | std::ios::trunc);
        output << "changed";
    }
    CHECK(PublishVerifiedUpdateStage({package, checkpoint.Value(), Paths(stage).partialFile, root, inventory, archiveLimits}, files,
                                     Verifier(), {})
              .HasError());
    CHECK_FALSE(std::filesystem::exists(published.Value()));
}

TEST_CASE("Cancelled or invalid stage never publishes ready", "[release][update]") {
    TemporaryStage stage;
    const std::string payload(100U, 'p');
    const auto package = Package(payload);
    Horo::NativeDurableFileSystem files;
    const auto root = stage.path / "candidate";
    std::filesystem::create_directory(root);
    const std::array inventory{UpdateStagedFile{"bin/editor", 1U, Horo::ComputeSha256(std::as_bytes(std::span{payload})),
                                                UpdateFileMode::Executable, UpdateFileRole::Entrypoint}};
    constexpr UpdateArchiveLimits archiveLimits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U};
    Horo::CancellationSource cancellation;
    cancellation.RequestCancellation();
    CHECK(PublishVerifiedUpdateStage({package, {}, Paths(stage).partialFile, root, inventory, archiveLimits}, files, Verifier(),
                                     cancellation.Token())
              .HasError());
    CHECK_FALSE(std::filesystem::exists(root.string() + ".ready"));
    CHECK(PublishVerifiedUpdateStage({package, {}, Paths(stage).partialFile, "relative/candidate", inventory, archiveLimits}, files,
                                     Verifier(), {})
              .HasError());
    const auto marker = std::filesystem::path{root.string() + ".ready"};
    {
        std::ofstream output(marker, std::ios::binary);
        output << "preserve unrelated package path";
    }
    CHECK(PublishVerifiedUpdateStage({package, {}, marker, root, inventory, archiveLimits}, files, Verifier(), {}).HasError());
    CHECK(std::filesystem::is_regular_file(marker));
}

TEST_CASE("Verified ZIP staging extracts bounded content and publishes ready", "[release][update]") {
    TemporaryStage stage;
    const auto archive = ZipArchive("bin/editor", "verified editor");
    const auto package = Package(archive);
    const auto paths = Paths(stage);
    {
        std::ofstream output(paths.partialFile, std::ios::binary);
        output.write(archive.data(), static_cast<std::streamsize>(archive.size()));
    }
    Horo::NativeDurableFileSystem files;
    constexpr UpdateArchiveLimits archiveLimits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U};
    const auto root = stage.path / "candidate";
    auto published =
        StageVerifiedZipUpdate({package, CompleteCheckpoint(package), paths.partialFile, root, archiveLimits}, files, Verifier(), {});
    REQUIRE(published.HasValue());
    CHECK(std::filesystem::is_regular_file(published.Value()));
    std::ifstream input(root / "bin/editor", std::ios::binary);
    const std::string content{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    CHECK(content == "verified editor");
    const std::array inventory{UpdateStagedFile{"bin/editor", content.size(), Horo::ComputeSha256(std::as_bytes(std::span{content})),
                                                UpdateFileMode::Executable, UpdateFileRole::Entrypoint}};
    CHECK(VerifyReadyUpdateStage(package, CompleteCheckpoint(package), paths.partialFile, root, inventory, archiveLimits, Verifier())
              .HasValue());
#if !defined(_WIN32)
    CHECK((std::filesystem::status(root / "bin/editor").permissions() & std::filesystem::perms::mask) == std::filesystem::perms{0755});
    std::filesystem::permissions(root / "bin/editor", std::filesystem::perms{0644});
    CHECK(VerifyReadyUpdateStage(package, CompleteCheckpoint(package), paths.partialFile, root, inventory, archiveLimits, Verifier())
              .HasError());
#endif
}

TEST_CASE("A verified delta ZIP stays private without an activation ready marker", "[release][update][delta]") {
    TemporaryStage stage;
    const auto archive = ZipArchive("bin/editor", "patched editor");
    auto package = Package(archive);
    package.selection.format = DistributionPackageFormat::DeltaZipArchive;
    const auto paths = Paths(stage);
    {
        std::ofstream output(paths.partialFile, std::ios::binary);
        output.write(archive.data(), static_cast<std::streamsize>(archive.size()));
    }
    Horo::NativeDurableFileSystem files;
    constexpr UpdateArchiveLimits limits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U};
    const auto root = stage.path / "delta";
    auto extracted =
        StageVerifiedDeltaZipUpdate({package, CompleteCheckpoint(package), paths.partialFile, root, limits}, files, Verifier(), {});
    REQUIRE(extracted.HasValue());
    REQUIRE(extracted.Value().size() == 1U);
    CHECK(extracted.Value().front().path == "bin/editor");
    CHECK(std::filesystem::is_regular_file(root / "bin/editor"));
    CHECK_FALSE(std::filesystem::exists(stage.path / "delta.ready"));
    CHECK_FALSE(std::filesystem::exists(stage.path / "delta.ready.prepared"));
    CHECK(StageVerifiedZipUpdate({package, CompleteCheckpoint(package), paths.partialFile, stage.path / "rejected", limits}, files,
                                 Verifier(), {})
              .HasError());
    CHECK_FALSE(std::filesystem::exists(stage.path / "rejected"));
    {
        std::ofstream marker(stage.path / "blocked.ready", std::ios::binary);
        marker << "foreign marker";
    }
    CHECK(StageVerifiedDeltaZipUpdate({package, CompleteCheckpoint(package), paths.partialFile, stage.path / "blocked", limits}, files,
                                      Verifier(), {})
              .HasError());
    CHECK_FALSE(std::filesystem::exists(stage.path / "blocked"));
    CHECK(std::filesystem::is_regular_file(stage.path / "blocked.ready"));
}

TEST_CASE("Activation admission rejects a changed ready marker", "[release][update]") {
    TemporaryStage stage;
    const std::string content = "verified editor";
    const auto archive = ZipArchive("bin/editor", content);
    const auto package = Package(archive);
    const auto paths = Paths(stage);
    {
        std::ofstream output(paths.partialFile, std::ios::binary);
        output.write(archive.data(), static_cast<std::streamsize>(archive.size()));
    }
    Horo::NativeDurableFileSystem files;
    constexpr UpdateArchiveLimits limits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U};
    const auto root = stage.path / "candidate";
    auto published = StageVerifiedZipUpdate({package, CompleteCheckpoint(package), paths.partialFile, root, limits}, files, Verifier(), {});
    REQUIRE(published.HasValue());
    const std::array inventory{UpdateStagedFile{"bin/editor", content.size(), Horo::ComputeSha256(std::as_bytes(std::span{content})),
                                                UpdateFileMode::Executable, UpdateFileRole::Entrypoint}};
    {
        std::ofstream output(published.Value(), std::ios::binary | std::ios::trunc);
        output << "stale";
    }
    CHECK(VerifyReadyUpdateStage(package, CompleteCheckpoint(package), paths.partialFile, root, inventory, limits, Verifier()).HasError());
}

TEST_CASE("ZIP staging reserves disk space before creating the private tree", "[release][update]") {
    TemporaryStage stage;
    const auto archive = ZipArchive("bin/editor", "verified editor");
    const auto package = Package(archive);
    const auto paths = Paths(stage);
    {
        std::ofstream output(paths.partialFile, std::ios::binary);
        output.write(archive.data(), static_cast<std::streamsize>(archive.size()));
    }
    Horo::NativeDurableFileSystem files;
    constexpr UpdateArchiveLimits limits{.maximumEntries = 4U,
                                         .maximumFileBytes = 1024U,
                                         .maximumExpandedBytes = 1024U,
                                         .reserveBytes = std::numeric_limits<std::uint64_t>::max()};
    const auto root = stage.path / "candidate";
    CHECK(
        StageVerifiedZipUpdate({package, CompleteCheckpoint(package), paths.partialFile, root, limits}, files, Verifier(), {}).HasError());
    CHECK_FALSE(std::filesystem::exists(root));
    CHECK_FALSE(std::filesystem::exists(root.string() + ".ready"));
}

TEST_CASE("ZIP staging rejects escaped and oversized entries before creating a tree", "[release][update]") {
    TemporaryStage stage;
    Horo::NativeDurableFileSystem files;
    constexpr UpdateArchiveLimits archiveLimits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U};
    const auto root = stage.path / "candidate";
    for (const auto name : {"../escaped", "bin/editor"}) {
        const auto content = name == std::string_view{"../escaped"} ? std::string{"safe"} : std::string(2000U, 'x');
        const auto archive = ZipArchive(name, content);
        const auto package = Package(archive);
        const auto paths = Paths(stage);
        {
            std::ofstream output(paths.partialFile, std::ios::binary | std::ios::trunc);
            output.write(archive.data(), static_cast<std::streamsize>(archive.size()));
        }
        CHECK(StageVerifiedZipUpdate({package, CompleteCheckpoint(package), paths.partialFile, root, archiveLimits}, files, Verifier(), {})
                  .HasError());
        CHECK_FALSE(std::filesystem::exists(root));
        CHECK_FALSE(std::filesystem::exists(root.string() + ".ready"));
    }
}

TEST_CASE("ZIP staging persists empty files and cancellation clears stale ready evidence", "[release][update]") {
    TemporaryStage stage;
    const auto archive =
        ZipArchive("bin/editor", "run", std::nullopt, std::pair<std::string_view, std::string_view>{"empty.txt", ""}, true, false, true);
    const auto package = Package(archive);
    const auto paths = Paths(stage);
    {
        std::ofstream output(paths.partialFile, std::ios::binary);
        output.write(archive.data(), static_cast<std::streamsize>(archive.size()));
    }
    Horo::NativeDurableFileSystem files;
    constexpr UpdateArchiveLimits archiveLimits{.maximumEntries = 3U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U};
    const auto root = stage.path / "candidate";
    const auto marker = std::filesystem::path{root.string() + ".ready"};
    {
        std::ofstream output(marker);
        output << "stale";
    }
    Horo::CancellationSource cancellation;
    cancellation.RequestCancellation();
    CHECK(StageVerifiedZipUpdate({package, CompleteCheckpoint(package), paths.partialFile, root, archiveLimits}, files, Verifier(),
                                 cancellation.Token())
              .HasError());
    CHECK_FALSE(std::filesystem::exists(marker));
    CHECK_FALSE(std::filesystem::exists(root));
    auto published =
        StageVerifiedZipUpdate({package, CompleteCheckpoint(package), paths.partialFile, root, archiveLimits}, files, Verifier(), {});
    REQUIRE(published.HasValue());
    CHECK(std::filesystem::is_regular_file(root / "empty.txt"));
    CHECK(std::filesystem::file_size(root / "empty.txt") == 0U);
}

TEST_CASE("ZIP staging never removes a package whose path aliases its ready marker", "[release][update]") {
    TemporaryStage stage;
    const auto archive = ZipArchive("bin/editor", "verified editor");
    const auto package = Package(archive);
    const auto root = stage.path / "candidate";
    const auto packageFile = std::filesystem::path{root.string() + ".ready"};
    {
        std::ofstream output(packageFile, std::ios::binary);
        output.write(archive.data(), static_cast<std::streamsize>(archive.size()));
    }
    Horo::NativeDurableFileSystem files;
    constexpr UpdateArchiveLimits archiveLimits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U};
    CHECK(
        StageVerifiedZipUpdate({package, CompleteCheckpoint(package), packageFile, root, archiveLimits}, files, Verifier(), {}).HasError());
    CHECK(std::filesystem::file_size(packageFile) == archive.size());
}

TEST_CASE("ZIP staging rejects undeclared files and declared digest mismatches", "[release][update]") {
    TemporaryStage stage;
    Horo::NativeDurableFileSystem files;
    constexpr UpdateArchiveLimits archiveLimits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 4096U};
    const auto root = stage.path / "candidate";
    const std::array archives{ZipArchive("bin/editor", "verified editor", "altered editor"),
                              ZipArchive("bin/editor", "verified editor", std::nullopt,
                                         std::pair<std::string_view, std::string_view>{"extra.txt", "undeclared"}),
                              ZipArchive("bin/editor", "verified editor", std::nullopt, std::nullopt, false),
                              ZipArchive("bin/editor", "verified editor", std::nullopt, std::nullopt, true, true)};
    for (const auto &archive : archives) {
        const auto package = Package(archive);
        const auto paths = Paths(stage);
        {
            std::ofstream output(paths.partialFile, std::ios::binary | std::ios::trunc);
            output.write(archive.data(), static_cast<std::streamsize>(archive.size()));
        }
        CHECK(StageVerifiedZipUpdate({package, CompleteCheckpoint(package), paths.partialFile, root, archiveLimits}, files, Verifier(), {})
                  .HasError());
        CHECK_FALSE(std::filesystem::exists(root));
        CHECK_FALSE(std::filesystem::exists(root.string() + ".ready"));
    }
}
