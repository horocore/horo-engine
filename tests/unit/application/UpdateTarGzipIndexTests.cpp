#include "Horo/Release/UpdateStageReady.h"
#include "Horo/Release/UpdateTarGzipStagingJob.h"
#include "Horo/Release/UpdateTransferCheckpointStore.h"
#if defined(__linux__)
#include "Horo/Release/LinuxPortableBootstrapHost.h"
#include "Horo/Release/UpdateRollback.h"
#endif

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
    class TemporaryPackage final {
    public:
        TemporaryPackage()
            : path(std::filesystem::temp_directory_path() /
                   ("horo-targzip-index-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(path);
        }

        ~TemporaryPackage() {
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

    [[nodiscard]] Horo::Security::ArtifactVerifier Verifier() {
        auto roots = std::make_shared<Horo::Security::TrustedRootStore>();
        std::vector<std::byte> key(65U, std::byte{1});
        key.front() = std::byte{0x04};
        REQUIRE(roots->Add({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = std::move(key)}).HasValue());
        return {std::make_shared<AcceptingProvider>(), std::move(roots)};
    }

    void PutOctal(const std::span<unsigned char> field, std::uint64_t value) {
        std::ranges::fill(field, static_cast<unsigned char>('0'));
        field.back() = 0U;
        for (std::size_t position = field.size() - 1U; position != 0U && value != 0U;) {
            --position;
            field[position] = static_cast<unsigned char>('0' + value % 8U);
            value /= 8U;
        }
        REQUIRE(value == 0U);
    }

    void AppendTarFile(std::vector<unsigned char> &tar, const std::string &name, const std::string &content,
                       const unsigned char type = '0') {
        REQUIRE(name.size() < 100U);
        std::array<unsigned char, 512> header{};
        std::ranges::copy(name, header.begin());
        PutOctal(std::span{header}.subspan(100, 8), 0644U);
        PutOctal(std::span{header}.subspan(124, 12), content.size());
        std::ranges::fill(std::span{header}.subspan(148, 8), static_cast<unsigned char>(' '));
        header[156] = type;
        std::ranges::copy(std::string_view{"ustar\0", 6U}, header.begin() + 257);
        header[263] = '0';
        header[264] = '0';
        std::uint64_t checksum = 0U;
        for (const auto value : header)
            checksum += value;
        PutOctal(std::span{header}.subspan(148, 8), checksum);
        tar.insert(tar.end(), header.begin(), header.end());
        tar.insert(tar.end(), content.begin(), content.end());
        tar.resize(tar.size() + (512U - content.size() % 512U) % 512U, 0U);
    }

    [[nodiscard]] std::vector<unsigned char> Tar(const std::string &name = "bin/editor", const unsigned char type = '0') {
        std::vector<unsigned char> tar;
        AppendTarFile(tar, std::string{UpdateFileInventoryPath}, "inventory");
        AppendTarFile(tar, name, "editor", type);
        tar.resize(tar.size() + 1024U, 0U);
        return tar;
    }

    [[nodiscard]] std::vector<unsigned char> InventoryTar(const std::string &content = "editor") {
        const UpdateStagedFile file{"bin/editor", content.size(), Horo::ComputeSha256(std::as_bytes(std::span{content})),
                                    UpdateFileMode::Executable, UpdateFileRole::Entrypoint};
        constexpr UpdateArchiveLimits limits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 2048U};
        auto encoded = BuildCanonicalUpdateFileInventory(std::span{&file, 1U}, limits);
        REQUIRE(encoded.HasValue());
        std::vector<unsigned char> tar;
        AppendTarFile(tar, std::string{UpdateFileInventoryPath}, encoded.Value());
        AppendTarFile(tar, file.path, content);
        tar.resize(tar.size() + 1024U, 0U);
        return tar;
    }

    void PutLittle32(std::vector<unsigned char> &bytes, const std::uint32_t value) {
        for (unsigned shift = 0U; shift < 32U; shift += 8U)
            bytes.push_back(static_cast<unsigned char>(value >> shift));
    }

    [[nodiscard]] std::vector<unsigned char> Gzip(const std::vector<unsigned char> &tar) {
        std::vector<unsigned char> compressed(mz_compressBound(tar.size()));
        mz_stream stream{};
        REQUIRE(mz_deflateInit2(&stream, MZ_DEFAULT_LEVEL, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 8, MZ_DEFAULT_STRATEGY) == MZ_OK);
        stream.next_in = tar.data();
        stream.avail_in = static_cast<mz_uint>(tar.size());
        stream.next_out = compressed.data();
        stream.avail_out = static_cast<mz_uint>(compressed.size());
        REQUIRE(mz_deflate(&stream, MZ_FINISH) == MZ_STREAM_END);
        compressed.resize(stream.total_out);
        REQUIRE(mz_deflateEnd(&stream) == MZ_OK);
        std::vector<unsigned char> gzip{0x1fU, 0x8bU, 8U, 0U, 0U, 0U, 0U, 0U, 0U, 255U};
        gzip.insert(gzip.end(), compressed.begin(), compressed.end());
        PutLittle32(gzip, static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, tar.data(), tar.size())));
        PutLittle32(gzip, static_cast<std::uint32_t>(tar.size()));
        return gzip;
    }

    struct SignedPackage final {
        UpdatePackageRecord record;
        UpdateTransferCheckpoint checkpoint;
        std::filesystem::path file;
    };

    [[nodiscard]] SignedPackage Sign(const TemporaryPackage &temporary, const std::vector<unsigned char> &bytes,
                                     const std::string &releaseVersion = "1.0.0", const std::string &packageId = "editor-linux",
                                     const std::string &fileName = "editor.tar.gz") {
        auto version = ParseReleaseVersion(releaseVersion);
        REQUIRE(version.HasValue());
        DistributionArtifactIdentity artifact{{DistributionProductKind::Editor, {}},
                                              EngineProductVersion{std::move(version).Value()},
                                              DistributionPlatform::Linux,
                                              DistributionArchitecture::X64,
                                              {"build-linux"},
                                              {packageId},
                                              DistributionInstallationId{"horo-editor"},
                                              DistributionArtifactClass::InstallableProduct};
        auto selection = ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::TarGzip);
        REQUIRE(selection.HasValue());
        UpdatePackageRecord record;
        record.selection = std::move(selection).Value();
        record.url = "https://updates.example.test/editor.tar.gz";
        record.size = bytes.size();
        record.digest = Horo::ComputeSha256(std::as_bytes(std::span{bytes}));
        record.signature = {.publisherId = "com.horo.updates",
                            .keyId = "key-1",
                            .artifactDigest = record.digest,
                            .signature = std::vector<std::byte>(64U, std::byte{1})};
        const UpdateTransferResponse response{.status = 200U,
                                              .requestedUrl = record.url,
                                              .effectiveUrl = record.url,
                                              .strongEtag = "\"linux-1\"",
                                              .contentLength = record.size};
        auto plan = PlanUpdateTransfer(record, response, std::nullopt);
        REQUIRE(plan.HasValue());
        auto checkpoint = AdvanceUpdateTransfer(plan.Value(), record.size);
        REQUIRE(checkpoint.HasValue());
        auto file = temporary.path / fileName;
        std::ofstream output(file, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        return {std::move(record), std::move(checkpoint).Value(), std::move(file)};
    }

    constexpr UpdateArchiveLimits Limits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 2048U};
#if defined(__linux__)
    class ProcessBridge final : public IUpdateActivationHost {
    public:
        [[nodiscard]] Horo::Result<void> EnsureProductsStopped(const std::filesystem::path &) override {
            ++stops;
            return Horo::Result<void>::Success();
        }

        [[nodiscard]] Horo::Result<void> ProbeStartupHealth(const std::filesystem::path &, std::chrono::seconds) override {
            ++probes;
            return healthy ? Horo::Result<void>::Success()
                           : Horo::Result<void>::Failure(Horo::Error{Horo::ErrorCode{"test.health"}, Horo::ErrorDomainId{"test"}});
        }

        bool healthy{false};
        unsigned stops{};
        unsigned probes{};
    };

    [[nodiscard]] UpdateActivationVersion StagePortableVersion(const TemporaryPackage &temporary, Horo::NativeDurableFileSystem &files,
                                                               const Horo::Security::ArtifactVerifier &verifier,
                                                               const std::string &releaseVersion, const std::string &packageId,
                                                               const std::string &content) {
        auto signedPackage = Sign(temporary, Gzip(InventoryTar(content)), releaseVersion, packageId, packageId + ".tar.gz");
        const auto versions = temporary.path / "versions";
        std::filesystem::create_directories(versions);
        const auto packageFile = versions / (packageId + ".tar.gz");
        std::filesystem::rename(signedPackage.file, packageFile);
        const auto stage = versions / packageId;
        REQUIRE(
            StageVerifiedTarGzipUpdate({signedPackage.record, signedPackage.checkpoint, packageFile, stage, Limits}, files, verifier, {})
                .HasValue());
        std::vector<UpdateStagedFile> inventory{{"bin/editor", content.size(), Horo::ComputeSha256(std::as_bytes(std::span{content})),
                                                 UpdateFileMode::Executable, UpdateFileRole::Entrypoint}};
        return {std::move(signedPackage.record), std::move(signedPackage.checkpoint), packageFile, stage, std::move(inventory)};
    }

    [[nodiscard]] std::string ReadFile(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }
#endif
}  // namespace

TEST_CASE("Signed Linux tar gzip is indexed before any extraction", "[release][update][tar]") {
    TemporaryPackage temporary;
    auto package = Sign(temporary, Gzip(Tar()));
    auto verifier = Verifier();
    auto result = IndexVerifiedTarGzipPackage(package.record, package.checkpoint, package.file, Limits, verifier);
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().size() == 2U);
    CHECK(result.Value()[1].path == "bin/editor");
    CHECK(result.Value()[1].expandedBytes == 6U);
}

TEST_CASE("Signed tar gzip rejects traversal and links", "[release][update][tar]") {
    TemporaryPackage temporary;
    auto verifier = Verifier();
    auto traversal = Sign(temporary, Gzip(Tar("../escape")));
    CHECK(IndexVerifiedTarGzipPackage(traversal.record, traversal.checkpoint, traversal.file, Limits, verifier).HasError());
    auto link = Sign(temporary, Gzip(Tar("bin/editor", '2')));
    CHECK(IndexVerifiedTarGzipPackage(link.record, link.checkpoint, link.file, Limits, verifier).HasError());
}

TEST_CASE("Signed tar gzip rejects corrupted trailer and oversized payload", "[release][update][tar]") {
    TemporaryPackage temporary;
    auto bytes = Gzip(Tar());
    bytes[bytes.size() - 8U] ^= 1U;
    auto package = Sign(temporary, bytes);
    auto verifier = Verifier();
    CHECK(IndexVerifiedTarGzipPackage(package.record, package.checkpoint, package.file, Limits, verifier).HasError());
    package = Sign(temporary, Gzip(Tar()));
    auto small = Limits;
    small.maximumExpandedBytes = 4U;
    CHECK(IndexVerifiedTarGzipPackage(package.record, package.checkpoint, package.file, small, verifier).HasError());
}

TEST_CASE("Signed tar gzip rejects malformed ustar checksums and incomplete termination", "[release][update][tar]") {
    TemporaryPackage temporary;
    auto verifier = Verifier();
    auto tar = Tar();
    tar[0] ^= 1U;
    auto package = Sign(temporary, Gzip(tar));
    CHECK(IndexVerifiedTarGzipPackage(package.record, package.checkpoint, package.file, Limits, verifier).HasError());
    tar = Tar();
    tar.resize(tar.size() - 512U);
    package = Sign(temporary, Gzip(tar));
    CHECK(IndexVerifiedTarGzipPackage(package.record, package.checkpoint, package.file, Limits, verifier).HasError());
}

TEST_CASE("Signed tar gzip refuses bytes changed after signature verification", "[release][update][tar]") {
    TemporaryPackage temporary;
    auto package = Sign(temporary, Gzip(Tar()));
    std::ofstream changed(package.file, std::ios::binary | std::ios::app);
    changed.put('x');
    changed.close();
    auto verifier = Verifier();
    CHECK(IndexVerifiedTarGzipPackage(package.record, package.checkpoint, package.file, Limits, verifier).HasError());
}

TEST_CASE("Signed Linux tar gzip stages its authenticated inventory and publishes ready", "[release][update][tar]") {
    TemporaryPackage temporary;
    auto package = Sign(temporary, Gzip(InventoryTar()));
    auto verifier = Verifier();
    Horo::NativeDurableFileSystem files;
    const auto stage = temporary.path / "editor-stage";
    auto published = StageVerifiedTarGzipUpdate({package.record, package.checkpoint, package.file, stage, Limits}, files, verifier, {});
    REQUIRE(published.HasValue());
    CHECK(std::filesystem::is_regular_file(published.Value()));
    const std::string content = "editor";
    const UpdateStagedFile file{"bin/editor", 6U, Horo::ComputeSha256(std::as_bytes(std::span{content})), UpdateFileMode::Executable,
                                UpdateFileRole::Entrypoint};
    CHECK(
        VerifyReadyUpdateStage(package.record, package.checkpoint, package.file, stage, std::span{&file, 1U}, Limits, verifier).HasValue());
}

TEST_CASE("Signed Linux tar gzip stages from a complete durable HTTPS checkpoint", "[release][update][tar]") {
    TemporaryPackage temporary;
    auto package = Sign(temporary, Gzip(InventoryTar()));
    Horo::NativeDurableFileSystem files;
    const UpdateDownloadPaths paths{package.file, temporary.path / "editor.checkpoint"};
    REQUIRE(SaveUpdateTransferCheckpoint(files, paths.partialFile, paths.checkpointFile, package.checkpoint).HasValue());
    const auto stage = temporary.path / "editor-stage";
    auto verifier = Verifier();
    auto published =
        PrepareTarGzipUpdateStageHttps({package.record, paths, stage, {.maximumPackageBytes = 4096U, .reserveBytes = 0U}, Limits}, files,
                                       verifier, {});
    REQUIRE(published.HasValue());
    CHECK(std::filesystem::is_regular_file(published.Value()));
    CHECK(std::filesystem::is_regular_file(stage / "bin/editor"));
}

TEST_CASE("Signed Linux tar gzip refuses overlapping private download paths before network access", "[release][update][tar]") {
    TemporaryPackage temporary;
    auto package = Sign(temporary, Gzip(InventoryTar()));
    Horo::NativeDurableFileSystem files;
    const UpdateDownloadPaths paths{package.file, package.file};
    auto verifier = Verifier();
    CHECK(PrepareTarGzipUpdateStageHttps({package.record, paths, temporary.path / "editor-stage", {}, Limits}, files, verifier, {})
              .HasError());
}

TEST_CASE("Signed Linux tar gzip rejects an inventory content mismatch without publishing ready", "[release][update][tar]") {
    TemporaryPackage temporary;
    const std::string declared = "editor";
    const UpdateStagedFile file{"bin/editor", declared.size(), Horo::ComputeSha256(std::as_bytes(std::span{declared})),
                                UpdateFileMode::Executable, UpdateFileRole::Entrypoint};
    auto encoded = BuildCanonicalUpdateFileInventory(std::span{&file, 1U}, Limits);
    REQUIRE(encoded.HasValue());
    std::vector<unsigned char> tar;
    AppendTarFile(tar, std::string{UpdateFileInventoryPath}, encoded.Value());
    AppendTarFile(tar, "bin/editor", "broken");
    tar.resize(tar.size() + 1024U, 0U);
    auto package = Sign(temporary, Gzip(tar));
    auto verifier = Verifier();
    Horo::NativeDurableFileSystem files;
    const auto stage = temporary.path / "editor-stage";
    auto staged = StageVerifiedTarGzipUpdate({package.record, package.checkpoint, package.file, stage, Limits}, files, verifier, {});
    CHECK(staged.HasError());
    CHECK_FALSE(std::filesystem::exists(stage));
    CHECK_FALSE(std::filesystem::exists(stage.string() + ".ready"));
}

TEST_CASE("Signed Linux tar gzip rejects a noncanonical internal inventory", "[release][update][tar]") {
    TemporaryPackage temporary;
    auto package = Sign(temporary, Gzip(Tar()));
    auto verifier = Verifier();
    Horo::NativeDurableFileSystem files;
    const auto stage = temporary.path / "editor-stage";
    CHECK(StageVerifiedTarGzipUpdate({package.record, package.checkpoint, package.file, stage, Limits}, files, verifier, {}).HasError());
    CHECK_FALSE(std::filesystem::exists(stage));
    CHECK_FALSE(std::filesystem::exists(stage.string() + ".ready"));
}

TEST_CASE("Signed Linux tar gzip will not replace an existing stage", "[release][update][tar]") {
    TemporaryPackage temporary;
    auto package = Sign(temporary, Gzip(InventoryTar()));
    auto verifier = Verifier();
    Horo::NativeDurableFileSystem files;
    const auto stage = temporary.path / "editor-stage";
    std::filesystem::create_directory(stage);
    std::ofstream(stage / "user-data") << "keep";
    CHECK(StageVerifiedTarGzipUpdate({package.record, package.checkpoint, package.file, stage, Limits}, files, verifier, {}).HasError());
    CHECK(std::filesystem::is_regular_file(stage / "user-data"));
}

TEST_CASE("Signed Linux tar gzip preserves a preexisting ready marker", "[release][update][tar]") {
    TemporaryPackage temporary;
    auto package = Sign(temporary, Gzip(InventoryTar()));
    auto verifier = Verifier();
    Horo::NativeDurableFileSystem files;
    const auto stage = temporary.path / "editor-stage";
    std::ofstream(stage.string() + ".ready") << "other-transaction";
    CHECK(StageVerifiedTarGzipUpdate({package.record, package.checkpoint, package.file, stage, Limits}, files, verifier, {}).HasError());
    std::ifstream marker(stage.string() + ".ready");
    std::string content;
    std::getline(marker, content);
    CHECK(content == "other-transaction");
    CHECK_FALSE(std::filesystem::exists(stage));
}

TEST_CASE("Signed Linux tar gzip refuses insufficient free capacity before staging", "[release][update][tar]") {
    TemporaryPackage temporary;
    auto package = Sign(temporary, Gzip(InventoryTar()));
    auto verifier = Verifier();
    Horo::NativeDurableFileSystem files;
    const auto stage = temporary.path / "editor-stage";
    auto limits = Limits;
    limits.reserveBytes = std::numeric_limits<std::uint64_t>::max();
    CHECK(StageVerifiedTarGzipUpdate({package.record, package.checkpoint, package.file, stage, limits}, files, verifier, {}).HasError());
    CHECK_FALSE(std::filesystem::exists(stage));
    CHECK_FALSE(std::filesystem::exists(stage.string() + ".ready"));
}

#if defined(__linux__)
TEST_CASE("Linux portable bootstrap installs repairs and uninstalls only owned version data", "[release][install][linux]") {
    TemporaryPackage temporary;
    auto package = Sign(temporary, Gzip(InventoryTar()));
    auto verifier = Verifier();
    Horo::NativeDurableFileSystem files;
    const auto versions = temporary.path / "versions";
    std::filesystem::create_directory(versions);
    const auto packageFile = versions / "editor-linux.tar.gz";
    std::filesystem::rename(package.file, packageFile);
    const auto stage = versions / "editor-linux";
    REQUIRE(StageVerifiedTarGzipUpdate({package.record, package.checkpoint, packageFile, stage, Limits}, files, verifier, {}).HasValue());
    CHECK((std::filesystem::status(stage / "bin/editor").permissions() & std::filesystem::perms::mask) == std::filesystem::perms{0755});
    const std::string content = "editor";
    std::vector<UpdateStagedFile> inventory{
        {"bin/editor", 6U, Horo::ComputeSha256(std::as_bytes(std::span{content})), UpdateFileMode::Executable, UpdateFileRole::Entrypoint}};
    UpdateActivationVersion candidate{package.record, package.checkpoint, packageFile, stage, inventory};
    BootstrapInstallationRequest request{temporary.path, candidate, Limits, std::chrono::seconds{2}};
    ProcessBridge processes;
    LinuxPortableBootstrapHost host(files, verifier, processes, {});
    const auto userProject = temporary.path / "projects" / "game.horo";
    std::filesystem::create_directory(userProject.parent_path());
    std::ofstream(userProject) << "project-data";
    LinuxPortableBootstrapHost noSpace(files, verifier, processes, {.minimumFreeBytes = std::numeric_limits<std::uint64_t>::max()});
    CHECK(BootstrapVerifiedInstallation(request, files, verifier, noSpace).HasError());
    CHECK_FALSE(std::filesystem::exists(temporary.path / "active-version"));
    CHECK(BootstrapVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK_FALSE(std::filesystem::exists(temporary.path / "active-version"));
    CHECK_FALSE(std::filesystem::exists(temporary.path / "bootstrap.pending"));
    processes.healthy = true;
    REQUIRE(BootstrapVerifiedInstallation(request, files, verifier, host).HasValue());
    REQUIRE(RepairVerifiedInstallation(request, files, verifier, host).HasValue());
    const auto unexpectedDirectory = stage / "unexpected";
    std::filesystem::create_directory(unexpectedDirectory);
    std::ofstream(unexpectedDirectory / "keep.txt") << "user content";
    CHECK(host.RemoveOwnedVersion(request).HasError());
    CHECK(std::filesystem::is_regular_file(unexpectedDirectory / "keep.txt"));
    CHECK(std::filesystem::is_regular_file(stage / "bin/editor"));
    std::filesystem::remove_all(unexpectedDirectory);

    const auto executable = stage / "bin/editor";
    const auto original = stage / "bin/editor.original";
    std::filesystem::rename(executable, original);
    std::error_code symlinkError;
    std::filesystem::create_symlink(original, executable, symlinkError);
    if (!symlinkError) {
        CHECK(host.RemoveOwnedVersion(request).HasError());
        CHECK(std::filesystem::is_regular_file(original));
        CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(executable)));
        std::filesystem::remove(executable);
    }
    std::filesystem::rename(original, executable);

    std::ofstream(stage / "user-note") << "preserve";
    CHECK(UninstallVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK(std::filesystem::is_regular_file(stage / "user-note"));
    CHECK(std::filesystem::is_regular_file(temporary.path / "active-version"));
    std::filesystem::remove(stage / "user-note");
    REQUIRE(UninstallVerifiedInstallation(request, files, verifier, host).HasValue());
    REQUIRE(host.RemoveOwnedVersion(request).HasValue());
    CHECK_FALSE(std::filesystem::exists(stage));
    CHECK_FALSE(std::filesystem::exists(packageFile));
    CHECK_FALSE(std::filesystem::exists(temporary.path / "active-version"));
    CHECK_FALSE(std::filesystem::exists(temporary.path / "bootstrap-uninstall.pending"));
    CHECK(std::filesystem::is_regular_file(userProject));
    CHECK(processes.probes == 3U);
}

TEST_CASE("Linux portable install, failed update, rollback, repair, and uninstall preserve user data",
          "[release][install][update][rollback][linux]") {
    TemporaryPackage temporary;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    const auto oldVersion = StagePortableVersion(temporary, files, verifier, "1.0.0", "editor-linux-v1", "editor-v1");
    const auto newVersion = StagePortableVersion(temporary, files, verifier, "2.0.0", "editor-linux-v2", "editor-v2");
    const BootstrapInstallationRequest initial{temporary.path, oldVersion, Limits, std::chrono::seconds{2}};
    const auto userProject = temporary.path / "projects" / "game.horo";
    std::filesystem::create_directories(userProject.parent_path());
    std::ofstream(userProject) << "preserved project";
    ProcessBridge processes;
    processes.healthy = true;
    LinuxPortableBootstrapHost host(files, verifier, processes, {});
    REQUIRE(BootstrapVerifiedInstallation(initial, files, verifier, host).HasValue());

    const auto oldPointer = EncodeActiveUpdateRecord(oldVersion.package);
    const auto newPointer = EncodeActiveUpdateRecord(newVersion.package);
    REQUIRE(oldPointer.HasValue());
    REQUIRE(newPointer.HasValue());
    const UpdateActivationRequest update{temporary.path, oldVersion, newVersion, Limits, std::chrono::seconds{2}};
    ProcessBridge failingProcesses;
    LinuxPortableBootstrapHost failingHost(files, verifier, failingProcesses, {});
    CHECK(ActivateVerifiedUpdate(update, files, verifier, failingHost).HasError());
    CHECK(ReadFile(temporary.path / "active-version") == oldPointer.Value());
    CHECK_FALSE(std::filesystem::exists(temporary.path / "activation.pending"));

    REQUIRE(ActivateVerifiedUpdate(update, files, verifier, host).HasValue());
    CHECK(ReadFile(temporary.path / "active-version") == newPointer.Value());
    CHECK(ReadFile(temporary.path / "last-known-good-version") == oldPointer.Value());
    REQUIRE(RepairVerifiedInstallation({temporary.path, newVersion, Limits, std::chrono::seconds{2}}, files, verifier, host).HasValue());

    const UpdateRollbackRequest rollback{{temporary.path, newVersion, oldVersion, Limits, std::chrono::seconds{2}},
                                         UpdateRollbackReason::ExplicitUserRequest,
                                         UpdateRollbackAuthority::NormalPolicy,
                                         oldVersion.package.selection.artifact.version,
                                         true};
    REQUIRE(RollbackVerifiedUpdate(rollback, files, verifier, host).HasValue());
    CHECK(ReadFile(temporary.path / "active-version") == oldPointer.Value());
    CHECK(ReadFile(temporary.path / "last-known-good-version") == newPointer.Value());
    CHECK_FALSE(std::filesystem::exists(temporary.path / "activation.pending"));
    REQUIRE(UninstallVerifiedInstallation(initial, files, verifier, host).HasValue());
    CHECK_FALSE(std::filesystem::exists(temporary.path / "active-version"));
    CHECK_FALSE(std::filesystem::exists(oldVersion.stageRoot));
    CHECK(ReadFile(userProject) == "preserved project");
}
#endif
