#include "Horo/Release/UpdateActivation.h"
#include "Horo/Release/UpdateManifestErrors.h"
#include "Horo/Release/UpdateOfflineSource.h"
#include "Horo/Release/UpdateOfflineSourceErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <miniz.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace Horo;
using namespace Horo::Release;

namespace {
    class TemporaryRoot final {
    public:
        TemporaryRoot()
            : path(std::filesystem::temp_directory_path() /
                   ("horo-offline-update-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(path);
        }

        ~TemporaryRoot() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

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

    [[nodiscard]] Security::DetachedSignatureEnvelope Signature(const Sha256Digest &digest) {
        return {.publisherId = "com.horo.updates",
                .keyId = "key-1",
                .artifactDigest = digest,
                .signature = std::vector<std::byte>(64U, std::byte{1})};
    }

    [[nodiscard]] std::string Archive() {
        constexpr std::string_view content = "verified offline editor";
        const auto digest = ComputeSha256(std::as_bytes(std::span{content}));
        const std::string inventory =
            "horo-update-files-v2\nbin/editor\t" + std::to_string(content.size()) + "\t" + FormatSha256(digest) + "\t0755\tentrypoint\n";
        mz_zip_archive writer{};
        REQUIRE(mz_zip_writer_init_heap(&writer, 0U, 0U));
        REQUIRE(mz_zip_writer_add_mem(&writer, "horo-update-files-v2.txt", inventory.data(), inventory.size(), MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_add_mem(&writer, "bin/editor", content.data(), content.size(), MZ_DEFAULT_COMPRESSION));
        void *bytes = nullptr;
        std::size_t size = 0U;
        REQUIRE(mz_zip_writer_finalize_heap_archive(&writer, &bytes, &size));
        std::string archive{static_cast<const char *>(bytes), size};
        mz_free(bytes);
        mz_zip_writer_end(&writer);
        return archive;
    }

    struct Fixture final {
        UpdateOfflineSource source;
        UpdateAdmissionContext admission;
        UpdateTrustRootSnapshot roots;
        UpdateDownloadPaths privatePaths;
        std::filesystem::path stageRoot;
        UpdateDownloadLimits downloadLimits{.maximumPackageBytes = 4096U};
        UpdateArchiveLimits archiveLimits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U};
        std::string archive;
    };

    [[nodiscard]] Fixture MakeFixture(const TemporaryRoot &temporary) {
        const auto version = ParseReleaseVersion("0.4.2");
        const auto installed = ParseReleaseVersion("0.4.1");
        REQUIRE(version.HasValue());
        REQUIRE(installed.HasValue());
        UpdateTrustRootData root;
        root.product = {DistributionProductKind::Editor, {}};
        root.revision = 1U;
        root.minimumManifestSequence = 7U;
        root.expiresAt = 3000U;
        std::vector<std::byte> key(65U, std::byte{1});
        key.front() = std::byte{0x04};
        root.keys.push_back({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = std::move(key)});
        auto roots = UpdateTrustRootSnapshot::Bootstrap(std::move(root));
        REQUIRE(roots.HasValue());
        const auto privateRoot = temporary.path / "private";
        std::filesystem::create_directories(privateRoot);
        return {{UpdateOfflineSourceKind::RemovableMedia, "media-1", temporary.path / "media", "stable", 0U},
                {.installedProduct = {DistributionProductKind::Editor, {}},
                 .installedVersion = EngineProductVersion{installed.Value()},
                 .channel = "stable",
                 .platform = DistributionPlatform::Windows,
                 .architecture = DistributionArchitecture::X64,
                 .updaterVersion = 1U,
                 .minimumAcceptedSequence = 7U,
                 .now = 1500U},
                std::move(roots).Value(),
                {privateRoot / "package.partial", privateRoot / "package.checkpoint"},
                privateRoot / "candidate",
                {.maximumPackageBytes = 4096U},
                {.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U},
                Archive()};
    }

    void WriteMedia(const Fixture &fixture, const std::string_view packageBytes) {
        std::filesystem::create_directories(fixture.source.root / "packages");
        const auto version = ParseReleaseVersion("0.4.2");
        REQUIRE(version.HasValue());
        DistributionArtifactIdentity identity;
        identity.product = {DistributionProductKind::Editor, {}};
        identity.version = EngineProductVersion{version.Value()};
        identity.platform = DistributionPlatform::Windows;
        identity.architecture = DistributionArchitecture::X64;
        identity.build = {"build-42"};
        identity.package = {"editor-linux"};
        identity.installation = DistributionInstallationId{"horo-editor"};
        identity.artifactClass = DistributionArtifactClass::InstallableProduct;
        const auto selection = ValidateDistributionPackageSelection(identity, DistributionPackageFormat::ZipArchive);
        REQUIRE(selection.HasValue());
        const auto digest = ComputeSha256(std::as_bytes(std::span{fixture.archive}));
        UpdateManifestData data;
        data.product = identity.product;
        data.version = identity.version;
        data.build = identity.build;
        data.channel = "stable";
        data.sequence = 7U;
        data.publishedAt = 1000U;
        data.expiresAt = 2000U;
        data.minimumUpdaterVersion = 1U;
        data.minimumRootRevision = 1U;
        data.packages.push_back(
            {selection.Value(), "https://updates.example.test/editor.zip", fixture.archive.size(), digest, Signature(digest)});
        const auto payload = BuildCanonicalUpdatePayload(data);
        REQUIRE(payload.HasValue());
        const auto manifest =
            SignedUpdateManifest::Create(std::move(data), Signature(ComputeSha256(std::as_bytes(std::span{payload.Value()}))));
        REQUIRE(manifest.HasValue());
        std::ofstream(fixture.source.root / "manifest.json", std::ios::binary) << manifest.Value().CanonicalDocument();
        std::ofstream(fixture.source.root / "packages" / (FormatSha256(digest).substr(7U) + ".zip"), std::ios::binary)
            .write(packageBytes.data(), static_cast<std::streamsize>(packageBytes.size()));
    }

    [[nodiscard]] UpdateOfflineImportRequest Request(const Fixture &fixture) {
        return {fixture.source,    fixture.admission,      fixture.roots,        fixture.privatePaths,
                fixture.stageRoot, fixture.downloadLimits, fixture.archiveLimits};
    }

    class ActivationHost final : public IUpdateActivationHost {
    public:
        [[nodiscard]] Result<void> EnsureProductsStopped(const std::filesystem::path &) override {
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ProbeStartupHealth(const std::filesystem::path &, std::chrono::seconds) override {
            return Result<void>::Success();
        }
    };

    void ActivateImportedAgainstInstalledVersion(const Fixture &fixture, const std::filesystem::path &installation,
                                                 const ImportedOfflineUpdate &imported, NativeDurableFileSystem &files,
                                                 Security::ArtifactVerifier &verifier) {
        const auto versions = installation / "versions";
        const auto oldVersion = ParseReleaseVersion("0.4.1");
        REQUIRE(oldVersion.HasValue());
        DistributionArtifactIdentity oldIdentity;
        oldIdentity.product = {DistributionProductKind::Editor, {}};
        oldIdentity.version = EngineProductVersion{oldVersion.Value()};
        oldIdentity.platform = DistributionPlatform::Windows;
        oldIdentity.architecture = DistributionArchitecture::X64;
        oldIdentity.build = {"build-old"};
        oldIdentity.package = {"old"};
        oldIdentity.installation = DistributionInstallationId{"horo-editor"};
        oldIdentity.artifactClass = DistributionArtifactClass::InstallableProduct;
        const auto selection = ValidateDistributionPackageSelection(oldIdentity, DistributionPackageFormat::ZipArchive);
        REQUIRE(selection.HasValue());
        const auto digest = ComputeSha256(std::as_bytes(std::span{fixture.archive}));
        UpdatePackageRecord oldPackage{selection.Value(), "https://updates.example.test/old.zip", fixture.archive.size(), digest,
                                       Signature(digest)};
        const UpdateTransferCheckpoint oldCheckpoint{digest,         fixture.archive.size(), fixture.archive.size(),
                                                     oldPackage.url, oldPackage.url,         {}};
        const auto oldFile = versions / "old.zip";
        const auto oldStage = versions / "old";
        std::ofstream(oldFile, std::ios::binary).write(fixture.archive.data(), static_cast<std::streamsize>(fixture.archive.size()));
        REQUIRE(
            StageVerifiedZipUpdate({oldPackage, oldCheckpoint, oldFile, oldStage, fixture.archiveLimits}, files, verifier, {}).HasValue());
        const auto activeRecord = EncodeActiveUpdateRecord(oldPackage);
        REQUIRE(activeRecord.HasValue());
        REQUIRE(files.WriteDurable(installation / "active-version", std::as_bytes(std::span{activeRecord.Value()})).HasValue());
        constexpr std::string_view content = "verified offline editor";
        const std::vector<UpdateStagedFile> inventory{{"bin/editor", content.size(), ComputeSha256(std::as_bytes(std::span{content})),
                                                       UpdateFileMode::Executable, UpdateFileRole::Entrypoint}};
        UpdateActivationRequest activation{installation,
                                           {oldPackage, oldCheckpoint, oldFile, oldStage, inventory},
                                           {imported.package, imported.checkpoint, fixture.privatePaths.partialFile, fixture.stageRoot,
                                            inventory},
                                           fixture.archiveLimits};
        ActivationHost host;
        const auto outcome = ActivateVerifiedUpdate(activation, files, verifier, host);
        REQUIRE(outcome.HasValue());
        CHECK(outcome.Value() == UpdateActivationOutcome::Activated);
    }
}  // namespace

TEST_CASE("Offline source order is stable and identities are unique", "[release][update][offline]") {
    TemporaryRoot temporary;
    const std::vector sources{UpdateOfflineSource{UpdateOfflineSourceKind::LocalDirectory, "secondary", temporary.path, "stable", 2U},
                              UpdateOfflineSource{UpdateOfflineSourceKind::EnterpriseMirror, "primary", temporary.path, "stable", 1U}};
    auto ordered = OrderUpdateOfflineSources(sources);
    REQUIRE(ordered.HasValue());
    CHECK(ordered.Value().front()->sourceId == "primary");
    auto duplicate = sources;
    duplicate.back().sourceId = "secondary";
    CHECK(OrderUpdateOfflineSources(duplicate).HasError());
    CHECK(MayTryNextOfflineSource(MakeError(UpdateOfflineSourceErrors::Unavailable)));
    CHECK_FALSE(MayTryNextOfflineSource(MakeError(UpdateOfflineSourceErrors::MirrorMismatch)));
    CHECK_FALSE(MayTryNextOfflineSource(MakeError(UpdateManifestErrors::Stale)));
}

TEST_CASE("Missing offline media and linked source paths fail before private writes", "[release][update][offline]") {
    TemporaryRoot temporary;
    auto fixture = MakeFixture(temporary);
    NativeDurableFileSystem files;
    auto provider = std::make_shared<AcceptingProvider>();
    auto missing = ImportOfflineZipUpdate(Request(fixture), files, provider, {});
    REQUIRE(missing.HasError());
    CHECK(missing.ErrorValue().code.Value() == UpdateOfflineSourceErrors::Unavailable.code.Value());
    CHECK_FALSE(std::filesystem::exists(fixture.privatePaths.partialFile));
    std::error_code linkError;
    std::filesystem::create_directory_symlink(temporary.path / "private", fixture.source.root, linkError);
    if (!linkError) {
        auto linked = ImportOfflineZipUpdate(Request(fixture), files, provider, {});
        REQUIRE(linked.HasError());
        CHECK(linked.ErrorValue().code.Value() == UpdateOfflineSourceErrors::UnsafePath.code.Value());
        CHECK_FALSE(std::filesystem::exists(fixture.privatePaths.partialFile));
    }
}

TEST_CASE("Offline metadata expiry is bounded and cannot extend the trust root", "[release][update][offline]") {
    TemporaryRoot temporary;
    auto fixture = MakeFixture(temporary);
    WriteMedia(fixture, fixture.archive);
    NativeDurableFileSystem files;
    auto provider = std::make_shared<AcceptingProvider>();
    fixture.admission.now = 2001U;
    CHECK(ImportOfflineZipUpdate(Request(fixture), files, provider, {}).HasError());
    fixture.source.maximumExpiredManifestSeconds = 60U;
    fixture.admission.now = 2061U;
    CHECK(ImportOfflineZipUpdate(Request(fixture), files, provider, {}).HasError());
    fixture.source.maximumExpiredManifestSeconds = 2000U;
    fixture.admission.now = 3001U;
    CHECK(ImportOfflineZipUpdate(Request(fixture), files, provider, {}).HasError());
    CHECK_FALSE(std::filesystem::exists(fixture.privatePaths.partialFile));
}

TEST_CASE("Offline mirror mismatch cannot publish a ready stage", "[release][update][offline]") {
    TemporaryRoot temporary;
    auto fixture = MakeFixture(temporary);
    fixture.source.kind = UpdateOfflineSourceKind::EnterpriseMirror;
    auto altered = fixture.archive;
    altered.back() ^= 1;
    WriteMedia(fixture, altered);
    NativeDurableFileSystem files;
    auto result = ImportOfflineZipUpdate(Request(fixture), files, std::make_shared<AcceptingProvider>(), {});
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == UpdateOfflineSourceErrors::MirrorMismatch.code.Value());
    CHECK_FALSE(std::filesystem::exists(fixture.privatePaths.partialFile));
    CHECK_FALSE(std::filesystem::exists(fixture.stageRoot.string() + ".ready"));
}

TEST_CASE("Linked package cannot stand in for a manifest-declared digest file", "[release][update][offline]") {
    TemporaryRoot temporary;
    auto fixture = MakeFixture(temporary);
    WriteMedia(fixture, fixture.archive);
    const auto packagePath =
        fixture.source.root / "packages" / (FormatSha256(ComputeSha256(std::as_bytes(std::span{fixture.archive}))).substr(7U) + ".zip");
    std::filesystem::remove(packagePath);
    std::error_code linkError;
    std::filesystem::create_symlink(temporary.path / "private", packagePath, linkError);
    if (linkError)
        return;
    NativeDurableFileSystem files;
    auto result = ImportOfflineZipUpdate(Request(fixture), files, std::make_shared<AcceptingProvider>(), {});
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == UpdateOfflineSourceErrors::UnsafePath.code.Value());
    CHECK_FALSE(std::filesystem::exists(fixture.privatePaths.partialFile));
}

TEST_CASE("Administrator expiry grace admits only a signed package before root expiry", "[release][update][offline]") {
    TemporaryRoot temporary;
    auto fixture = MakeFixture(temporary);
    WriteMedia(fixture, fixture.archive);
    fixture.source.maximumExpiredManifestSeconds = 60U;
    fixture.admission.now = 2030U;
    NativeDurableFileSystem files;
    auto result = ImportOfflineZipUpdate(Request(fixture), files, std::make_shared<AcceptingProvider>(), {});
    REQUIRE(result.HasValue());
    CHECK(std::filesystem::is_regular_file(result.Value().readyMarker));
}

TEST_CASE("Offline downgrade needs both administrator policy and explicit host action", "[release][update][offline]") {
    TemporaryRoot temporary;
    auto fixture = MakeFixture(temporary);
    WriteMedia(fixture, fixture.archive);
    const auto newer = ParseReleaseVersion("0.4.3");
    REQUIRE(newer.HasValue());
    fixture.admission.installedVersion = EngineProductVersion{newer.Value()};
    NativeDurableFileSystem files;
    auto provider = std::make_shared<AcceptingProvider>();
    fixture.source.allowDowngrade = true;
    CHECK(ImportOfflineZipUpdate(Request(fixture), files, provider, {}).HasError());
    fixture.admission.authorizedDowngrade = true;
    fixture.source.allowDowngrade = false;
    CHECK(ImportOfflineZipUpdate(Request(fixture), files, provider, {}).HasError());
    fixture.source.allowDowngrade = true;
    CHECK(ImportOfflineZipUpdate(Request(fixture), files, provider, {}).HasValue());
}

TEST_CASE("Offline import applies the installed platform and channel checks", "[release][update][offline]") {
    TemporaryRoot temporary;
    auto fixture = MakeFixture(temporary);
    WriteMedia(fixture, fixture.archive);
    NativeDurableFileSystem files;
    auto provider = std::make_shared<AcceptingProvider>();
    fixture.admission.platform = DistributionPlatform::Linux;
    CHECK(ImportOfflineZipUpdate(Request(fixture), files, provider, {}).HasError());
    fixture.admission.platform = DistributionPlatform::Windows;
    fixture.source.channel = "preview";
    CHECK(ImportOfflineZipUpdate(Request(fixture), files, provider, {}).HasError());
    CHECK_FALSE(std::filesystem::exists(fixture.privatePaths.partialFile));
}

TEST_CASE("Authenticated offline ZIP reaches the same ready stage as HTTPS", "[release][update][offline]") {
    TemporaryRoot temporary;
    auto fixture = MakeFixture(temporary);
    WriteMedia(fixture, fixture.archive);
    NativeDurableFileSystem files;
    auto result = ImportOfflineZipUpdate(Request(fixture), files, std::make_shared<AcceptingProvider>(), {});
    REQUIRE(result.HasValue());
    CHECK(std::filesystem::is_regular_file(result.Value().readyMarker));
    CHECK(std::filesystem::is_regular_file(fixture.privatePaths.checkpointFile));
    std::ifstream input(fixture.stageRoot / "bin/editor", std::ios::binary);
    std::string content;
    std::getline(input, content);
    CHECK(content == "verified offline editor");
}

TEST_CASE("Authenticated offline package activates through the normal version switch", "[release][update][offline]") {
    TemporaryRoot temporary;
    auto fixture = MakeFixture(temporary);
    const auto installation = temporary.path / "installation";
    const auto versions = installation / "versions";
    std::filesystem::create_directories(versions);
    fixture.privatePaths = {versions / "editor-linux.zip", versions / "editor-linux.checkpoint"};
    fixture.stageRoot = versions / "editor-linux";
    WriteMedia(fixture, fixture.archive);
    NativeDurableFileSystem files;
    auto provider = std::make_shared<AcceptingProvider>();
    Security::ArtifactVerifier verifier{provider, fixture.roots.Roots()};
    auto imported = ImportOfflineZipUpdate(Request(fixture), files, provider, {});
    REQUIRE(imported.HasValue());

    ActivateImportedAgainstInstalledVersion(fixture, installation, imported.Value(), files, verifier);
}
