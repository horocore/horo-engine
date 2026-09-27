#include "Horo/Release/UpdateDownloadSession.h"
#include "Horo/Release/UpdateHttpDownload.h"
#include "Horo/Release/UpdateStageReady.h"
#include "Horo/Release/UpdateTransferCheckpointStore.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
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

    const auto recovered = DownloadUpdatePackageHttps(package, Paths(stage), Limits, files, Verifier(), {});
    REQUIRE(recovered.HasValue());
    CHECK(recovered.Value().durableBytes == package.size);
}

TEST_CASE("HTTPS adapter rejects invalid policy and cancellation before opening transport", "[release][update]") {
    TemporaryStage stage;
    const std::string payload(100U, 'p');
    auto package = Package(payload);
    Horo::NativeDurableFileSystem files;
    package.url = "http://updates.example.test/editor.zip";
    CHECK(DownloadUpdatePackageHttps(package, Paths(stage), Limits, files, Verifier(), {}).HasError());
    package.url = "https://updates.example.test/editor.zip";
    CHECK(DownloadUpdatePackageHttps(package, Paths(stage), Limits, files, Verifier(), {},
                                     {.connectTimeoutSeconds = 15U, .requestTimeoutSeconds = 14U})
              .HasError());
    Horo::CancellationSource cancellation;
    cancellation.RequestCancellation();
    CHECK(DownloadUpdatePackageHttps(package, Paths(stage), Limits, files, Verifier(), cancellation.Token()).HasError());
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
    const std::array inventory{
        UpdateStagedFile{"bin/editor", executable.size(), Horo::ComputeSha256(std::as_bytes(std::span{executable}))}};
    constexpr UpdateArchiveLimits archiveLimits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U};
    auto published = PublishVerifiedUpdateStage(package, checkpoint.Value(), Paths(stage).partialFile, root, inventory, archiveLimits,
                                                files, Verifier(), {});
    REQUIRE(published.HasValue());
    CHECK(std::filesystem::is_regular_file(published.Value()));

    {
        std::ofstream output(root / "bin/editor", std::ios::binary | std::ios::trunc);
        output << "changed";
    }
    CHECK(PublishVerifiedUpdateStage(package, checkpoint.Value(), Paths(stage).partialFile, root, inventory, archiveLimits, files,
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
    const std::array inventory{UpdateStagedFile{"bin/editor", 1U, Horo::ComputeSha256(std::as_bytes(std::span{payload}))}};
    constexpr UpdateArchiveLimits archiveLimits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U};
    Horo::CancellationSource cancellation;
    cancellation.RequestCancellation();
    CHECK(PublishVerifiedUpdateStage(package, {}, Paths(stage).partialFile, root, inventory, archiveLimits, files, Verifier(),
                                     cancellation.Token())
              .HasError());
    CHECK_FALSE(std::filesystem::exists(root.string() + ".ready"));
    CHECK(PublishVerifiedUpdateStage(package, {}, Paths(stage).partialFile, "relative/candidate", inventory, archiveLimits, files,
                                     Verifier(), {})
              .HasError());
    const auto marker = std::filesystem::path{root.string() + ".ready"};
    {
        std::ofstream output(marker, std::ios::binary);
        output << "preserve unrelated package path";
    }
    CHECK(PublishVerifiedUpdateStage(package, {}, marker, root, inventory, archiveLimits, files, Verifier(), {}).HasError());
    CHECK(std::filesystem::is_regular_file(marker));
}
