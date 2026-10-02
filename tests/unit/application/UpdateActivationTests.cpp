#include "Horo/Release/BootstrapInstallation.h"
#include "Horo/Release/UpdateActivation.h"
#include "Horo/Release/UpdateActivationErrors.h"
#include "Horo/Release/UpdateRetention.h"
#include "Horo/Release/UpdateRetentionErrors.h"
#include "Horo/Release/UpdateRollback.h"
#include "Horo/Release/UpdateRollbackErrors.h"
#include "UpdateActivationTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <utility>

using namespace Horo::Release;

namespace {
    class TemporaryInstall final {
    public:
        TemporaryInstall()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-update-activation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(root / "versions");
        }

        ~TemporaryInstall() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path root;
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

    [[nodiscard]] UpdatePackageRecord Package(const std::string &id, const std::string &payload) {
        auto version = ParseReleaseVersion(id == "old" ? "1.0.0" : "2.0.0");
        REQUIRE(version.HasValue());
        DistributionArtifactIdentity artifact{{DistributionProductKind::Editor, {}},
                                              EngineProductVersion{std::move(version).Value()},
                                              DistributionPlatform::Windows,
                                              DistributionArchitecture::X64,
                                              {"build-" + id},
                                              {id},
                                              DistributionInstallationId{"installation-1"},
                                              DistributionArtifactClass::InstallableProduct};
        auto selection = ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::ZipArchive);
        REQUIRE(selection.HasValue());
        UpdatePackageRecord package;
        package.selection = std::move(selection).Value();
        package.url = "https://updates.example.test/" + id + ".zip";
        package.size = payload.size();
        package.digest = Horo::ComputeSha256(std::as_bytes(std::span{payload}));
        package.signature = {.publisherId = "com.horo.updates",
                             .keyId = "key-1",
                             .artifactDigest = package.digest,
                             .signature = std::vector<std::byte>(64U, std::byte{1})};
        return package;
    }

    [[nodiscard]] UpdateTransferCheckpoint CompleteCheckpoint(const UpdatePackageRecord &package) {
        UpdateTransferResponse response{.status = 200U,
                                        .requestedUrl = package.url,
                                        .effectiveUrl = package.url,
                                        .strongEtag = "\"release-1\"",
                                        .contentLength = package.size};
        auto plan = PlanUpdateTransfer(package, response, std::nullopt);
        REQUIRE(plan.HasValue());
        auto checkpoint = AdvanceUpdateTransfer(plan.Value(), package.size);
        REQUIRE(checkpoint.HasValue());
        return std::move(checkpoint).Value();
    }

    [[nodiscard]] UpdateActivationVersion VersionAtRoot(const std::filesystem::path &installRoot, const std::string &id,
                                                        Horo::NativeDurableFileSystem &files,
                                                        const Horo::Security::ArtifactVerifier &verifier) {
        const std::string payload = "signed package " + id;
        const std::string content = "editor " + id;
        auto package = Package(id, payload);
        auto checkpoint = CompleteCheckpoint(package);
        const auto packageFile = installRoot / "versions" / (id + ".zip");
        const auto stageRoot = installRoot / "versions" / id;
        std::filesystem::create_directories(stageRoot / "bin");
        {
            std::ofstream output(packageFile, std::ios::binary);
            output << payload;
        }
        {
            std::ofstream output(stageRoot / "bin/editor", std::ios::binary);
            output << content;
        }
#if !defined(_WIN32)
        std::filesystem::permissions(stageRoot / "bin/editor", std::filesystem::perms{0755});
#endif
        std::vector<UpdateStagedFile> inventory{{"bin/editor", content.size(), Horo::ComputeSha256(std::as_bytes(std::span{content})),
                                                 UpdateFileMode::Executable, UpdateFileRole::Entrypoint}};
        constexpr UpdateArchiveLimits limits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U};
        REQUIRE(
            PublishVerifiedUpdateStage({package, checkpoint, packageFile, stageRoot, inventory, limits}, files, verifier, {}).HasValue());
        return {std::move(package), std::move(checkpoint), packageFile, stageRoot, std::move(inventory)};
    }

    [[nodiscard]] UpdateActivationRequest Request(const TemporaryInstall &install, Horo::NativeDurableFileSystem &files,
                                                  const Horo::Security::ArtifactVerifier &verifier) {
        auto current = VersionAtRoot(install.root, "old", files, verifier);
        auto staged = VersionAtRoot(install.root, "new", files, verifier);
        auto pointer = EncodeActiveUpdateRecord(current.package);
        REQUIRE(pointer.HasValue());
        REQUIRE(files.WriteDurable(install.root / "active-version", std::as_bytes(std::span{pointer.Value()})).HasValue());
        return {install.root,
                std::move(current),
                std::move(staged),
                {.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U},
                std::chrono::seconds{2}};
    }

    [[nodiscard]] std::string Read(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    class Host final : public IBootstrapInstallationHost {
    public:
        [[nodiscard]] Horo::Result<void> Preflight(const BootstrapInstallationRequest &) override {
            ++preflights;
            return admissible ? Horo::Result<void>::Success()
                              : Horo::Result<void>::Failure(Horo::Error{Horo::ErrorCode{"test.preflight"}, Horo::ErrorDomainId{"test"}});
        }

        [[nodiscard]] Horo::Result<void> Register(const BootstrapInstallationRequest &) override {
            ++registrations;
            return registrationSucceeds
                       ? Horo::Result<void>::Success()
                       : Horo::Result<void>::Failure(Horo::Error{Horo::ErrorCode{"test.register"}, Horo::ErrorDomainId{"test"}});
        }

        [[nodiscard]] Horo::Result<void> Unregister(const BootstrapInstallationRequest &) override {
            ++unregistrations;
            return unregistrationSucceeds
                       ? Horo::Result<void>::Success()
                       : Horo::Result<void>::Failure(Horo::Error{Horo::ErrorCode{"test.unregister"}, Horo::ErrorDomainId{"test"}});
        }

        [[nodiscard]] Horo::Result<void> RemoveOwnedVersion(const BootstrapInstallationRequest &request) override {
            ++ownedRemovals;
            if (lockProbeFiles != nullptr)
                lockHeldDuringRemoval =
                    lockProbeFiles->TryAcquireExclusive(request.installationRoot / ".activation.lock", "probe").HasError();
            if (!ownedRemovalSucceeds)
                return Horo::Result<void>::Failure(Horo::Error{Horo::ErrorCode{"test.remove_owned"}, Horo::ErrorDomainId{"test"}});
            if (removeFiles) {
                std::error_code error;
                std::filesystem::remove_all(request.candidate.stageRoot, error);
                if (error)
                    return Horo::Result<void>::Failure(Horo::Error{Horo::ErrorCode{"test.remove_owned"}, Horo::ErrorDomainId{"test"}});
                std::filesystem::remove(request.candidate.packageFile, error);
                auto marker = request.candidate.stageRoot;
                marker += ".ready";
                std::filesystem::remove(marker, error);
            }
            return Horo::Result<void>::Success();
        }

        [[nodiscard]] Horo::Result<void> EnsureProductsStopped(const std::filesystem::path &) override {
            ++stops;
            return Horo::Result<void>::Success();
        }

        [[nodiscard]] Horo::Result<void> ProbeStartupHealth(const std::filesystem::path &, std::chrono::seconds timeout) override {
            ++probes;
            observedTimeout = timeout;
            return healthy ? Horo::Result<void>::Success()
                           : Horo::Result<void>::Failure(Horo::Error{Horo::ErrorCode{"test.health"}, Horo::ErrorDomainId{"test"}});
        }

        bool healthy{true};
        bool admissible{true};
        bool registrationSucceeds{true};
        bool unregistrationSucceeds{true};
        bool ownedRemovalSucceeds{true};
        bool removeFiles{false};
        Horo::NativeDurableFileSystem *lockProbeFiles{};
        bool lockHeldDuringRemoval{};
        unsigned stops{};
        unsigned probes{};
        unsigned preflights{};
        unsigned registrations{};
        unsigned unregistrations{};
        unsigned ownedRemovals{};
        std::chrono::seconds observedTimeout{};
    };

    [[nodiscard]] BootstrapInstallationRequest BootstrapRequest(const TemporaryInstall &install, Horo::NativeDurableFileSystem &files,
                                                                const Horo::Security::ArtifactVerifier &verifier) {
        return {install.root,
                VersionAtRoot(install.root, "new", files, verifier),
                {.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U},
                std::chrono::seconds{2}};
    }

}  // namespace

namespace Horo::Release::TestSupport {
    Security::ArtifactVerifier MakeVerifier() {
        return Verifier();
    }

    UpdateActivationVersion MakeVersion(const std::filesystem::path &root, NativeDurableFileSystem &files,
                                        const Security::ArtifactVerifier &verifier) {
        return VersionAtRoot(root, "new", files, verifier);
    }
}  // namespace Horo::Release::TestSupport

TEST_CASE("Verified update activation atomically selects the healthy staged version", "[release][update]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    Host host;
    auto activated = ActivateVerifiedUpdate(request, files, verifier, host);
    REQUIRE(activated.HasValue());
    CHECK(activated.Value() == UpdateActivationOutcome::Activated);
    auto target = EncodeActiveUpdateRecord(request.staged.package);
    REQUIRE(target.HasValue());
    CHECK(Read(install.root / "active-version") == target.Value());
    auto previous = EncodeActiveUpdateRecord(request.current.package);
    REQUIRE(previous.HasValue());
    CHECK(Read(install.root / "last-known-good-version") == previous.Value());
    CHECK_FALSE(std::filesystem::exists(install.root / "activation.pending"));
    CHECK(host.stops == 1U);
    CHECK(host.probes == 1U);
    CHECK(host.observedTimeout == std::chrono::seconds{2});
}

TEST_CASE("A delta artifact cannot activate as a complete installation", "[release][update][delta]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    const auto selection =
        ValidateDistributionPackageSelection(request.staged.package.selection.artifact, DistributionPackageFormat::DeltaZipArchive);
    REQUIRE(selection.HasValue());
    request.staged.package.selection = selection.Value();
    Host host;
    CHECK(ActivateVerifiedUpdate(request, files, verifier, host).HasError());
    CHECK(host.stops == 0U);
    auto previous = EncodeActiveUpdateRecord(request.current.package);
    REQUIRE(previous.HasValue());
    CHECK(Read(install.root / "active-version") == previous.Value());
}

TEST_CASE("Failed startup health restores the previous active version", "[release][update]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    Host host;
    host.healthy = false;
    CHECK(ActivateVerifiedUpdate(request, files, verifier, host).HasError());
    auto previous = EncodeActiveUpdateRecord(request.current.package);
    REQUIRE(previous.HasValue());
    CHECK(Read(install.root / "active-version") == previous.Value());
    CHECK_FALSE(std::filesystem::exists(install.root / "activation.pending"));
}

TEST_CASE("Failed startup health preserves the existing last-known-good pin", "[release][update][rollback]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    auto priorPin = EncodeActiveUpdateRecord(Package("prior", "signed package prior"));
    REQUIRE(priorPin.HasValue());
    REQUIRE(files.WriteDurable(install.root / "last-known-good-version", std::as_bytes(std::span{priorPin.Value()})).HasValue());
    Host host;
    host.healthy = false;
    CHECK(ActivateVerifiedUpdate(request, files, verifier, host).HasError());
    CHECK(Read(install.root / "last-known-good-version") == priorPin.Value());
    CHECK_FALSE(std::filesystem::exists(install.root / "activation.pending"));
}

TEST_CASE("Retention removes only signed obsolete content and is repeatable", "[release][update][retention]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    auto obsolete = VersionAtRoot(install.root, "obsolete", files, verifier);
    Host host;
    REQUIRE(ActivateVerifiedUpdate(request, files, verifier, host).HasValue());
    std::filesystem::create_directories(install.root / "projects");
    {
        std::ofstream userFile(install.root / "projects" / "save.json");
        userFile << "user data";
    }
    const std::array candidates{
        UpdateRetentionCandidate{{{"new"}, 40U, 10U, UpdateRetentionRole::Active}, request.staged},
        UpdateRetentionCandidate{{{"old"}, 30U, 9U, UpdateRetentionRole::LastKnownGood}, request.current},
        UpdateRetentionCandidate{{{"obsolete"}, 20U, 1U, UpdateRetentionRole::Obsolete}, obsolete},
    };
    const UpdateRetentionCleanupRequest cleanup{install.root, candidates, request.archiveLimits, 70U};
    auto first = ApplyUpdateRetention(cleanup, files, verifier, host);
    REQUIRE(first.HasValue());
    CHECK(first.Value().remove.size() == 1U);
    CHECK_FALSE(std::filesystem::exists(obsolete.stageRoot));
    CHECK_FALSE(std::filesystem::exists(obsolete.packageFile));
    CHECK(std::filesystem::exists(request.current.stageRoot));
    CHECK(std::filesystem::exists(request.staged.stageRoot));
    CHECK(Read(install.root / "projects" / "save.json") == "user data");
    CHECK(ApplyUpdateRetention(cleanup, files, verifier, host).HasValue());
}

TEST_CASE("Retention resumes an authenticated interrupted cleanup", "[release][update][retention]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    auto obsolete = VersionAtRoot(install.root, "obsolete", files, verifier);
    Host host;
    REQUIRE(ActivateVerifiedUpdate(request, files, verifier, host).HasValue());
    const std::array candidates{
        UpdateRetentionCandidate{{{"new"}, 40U, 10U, UpdateRetentionRole::Active}, request.staged},
        UpdateRetentionCandidate{{{"old"}, 30U, 9U, UpdateRetentionRole::LastKnownGood}, request.current},
        UpdateRetentionCandidate{{{"obsolete"}, 20U, 1U, UpdateRetentionRole::Obsolete}, obsolete},
    };
    const UpdateRetentionCleanupRequest cleanup{install.root, candidates, request.archiveLimits, 70U};
    const std::string marker = "horo-update-retention-v1\nobsolete\n" + Horo::FormatSha256(obsolete.package.digest) + '\n';
    REQUIRE(files.AppendPrivateDurable(install.root / "versions" / "obsolete.cleanup-pending", 0U, std::as_bytes(std::span{marker}))
                .HasValue());
    REQUIRE(files.RemoveDurable(obsolete.stageRoot / "bin" / "editor").HasValue());
    auto resumed = ApplyUpdateRetention(cleanup, files, verifier, host);
    REQUIRE(resumed.HasValue());
    CHECK_FALSE(std::filesystem::exists(obsolete.stageRoot));
    CHECK_FALSE(std::filesystem::exists(install.root / "versions" / "obsolete.cleanup-pending"));
}

TEST_CASE("Retention refuses active deletion, unknown files, and a concurrent lock", "[release][update][retention]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    auto obsolete = VersionAtRoot(install.root, "obsolete", files, verifier);
    Host host;
    REQUIRE(ActivateVerifiedUpdate(request, files, verifier, host).HasValue());
    const std::array candidates{
        UpdateRetentionCandidate{{{"new"}, 40U, 10U, UpdateRetentionRole::Active}, request.staged},
        UpdateRetentionCandidate{{{"old"}, 30U, 9U, UpdateRetentionRole::LastKnownGood}, request.current},
        UpdateRetentionCandidate{{{"obsolete"}, 20U, 1U, UpdateRetentionRole::Obsolete}, obsolete},
    };
    const UpdateRetentionCleanupRequest cleanup{install.root, candidates, request.archiveLimits, 70U};
    {
        auto held = files.TryAcquireExclusive(install.root / ".activation.lock", "held");
        REQUIRE(held.HasValue());
        CHECK(ApplyUpdateRetention(cleanup, files, verifier, host).HasError());
    }
    {
        std::ofstream extra(obsolete.stageRoot / "foreign.txt");
        extra << "unowned";
    }
    CHECK(ApplyUpdateRetention(cleanup, files, verifier, host).HasError());
    CHECK(std::filesystem::exists(obsolete.stageRoot / "foreign.txt"));
    CHECK(std::filesystem::exists(obsolete.packageFile));
}

TEST_CASE("A concurrent installation lock prevents a second activation", "[release][update]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    auto held = files.TryAcquireExclusive(install.root / ".activation.lock", "first");
    REQUIRE(held.HasValue());
    Host host;
    CHECK(ActivateVerifiedUpdate(request, files, verifier, host).HasError());
    CHECK(host.stops == 0U);
    CHECK(host.probes == 0U);
}

TEST_CASE("Interrupted activation restores the previous version before a retry", "[release][update]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    auto previous = EncodeActiveUpdateRecord(request.current.package);
    auto target = EncodeActiveUpdateRecord(request.staged.package);
    REQUIRE(previous.HasValue());
    REQUIRE(target.HasValue());
    const std::string pending = "horo-update-activation-v2\n" + previous.Value() + target.Value() + "pin-absent\n";
    REQUIRE(files.WriteDurable(install.root / "activation.pending", std::as_bytes(std::span{pending})).HasValue());
    REQUIRE(files.WriteDurable(install.root / "active-version", std::as_bytes(std::span{target.Value()})).HasValue());
    Host host;
    auto recovered = ActivateVerifiedUpdate(request, files, verifier, host);
    REQUIRE(recovered.HasValue());
    CHECK(recovered.Value() == UpdateActivationOutcome::RecoveredPrevious);
    CHECK(Read(install.root / "active-version") == previous.Value());
    CHECK_FALSE(std::filesystem::exists(install.root / "activation.pending"));
    CHECK(host.probes == 0U);
}

TEST_CASE("Interrupted activation restores the previous rollback pin", "[release][update][rollback]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    auto previous = EncodeActiveUpdateRecord(request.current.package);
    auto target = EncodeActiveUpdateRecord(request.staged.package);
    auto priorPin = EncodeActiveUpdateRecord(Package("prior", "signed package prior"));
    REQUIRE(previous.HasValue());
    REQUIRE(target.HasValue());
    REQUIRE(priorPin.HasValue());
    const std::string pending = "horo-update-activation-v2\n" + previous.Value() + target.Value() + "pin-present\n" + priorPin.Value();
    REQUIRE(files.WriteDurable(install.root / "activation.pending", std::as_bytes(std::span{pending})).HasValue());
    REQUIRE(files.WriteDurable(install.root / "active-version", std::as_bytes(std::span{target.Value()})).HasValue());
    REQUIRE(files.WriteDurable(install.root / "last-known-good-version", std::as_bytes(std::span{previous.Value()})).HasValue());
    Host host;
    auto recovered = ActivateVerifiedUpdate(request, files, verifier, host);
    REQUIRE(recovered.HasValue());
    CHECK(recovered.Value() == UpdateActivationOutcome::RecoveredPrevious);
    CHECK(Read(install.root / "active-version") == previous.Value());
    CHECK(Read(install.root / "last-known-good-version") == priorPin.Value());
    CHECK_FALSE(std::filesystem::exists(install.root / "activation.pending"));
}

TEST_CASE("Activation rejects a staged tree changed after ready publication", "[release][update]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    {
        std::ofstream output(request.staged.stageRoot / "bin/editor", std::ios::binary | std::ios::app);
        output << "changed";
    }
    Host host;
    CHECK(ActivateVerifiedUpdate(request, files, verifier, host).HasError());
    auto previous = EncodeActiveUpdateRecord(request.current.package);
    REQUIRE(previous.HasValue());
    CHECK(Read(install.root / "active-version") == previous.Value());
    CHECK_FALSE(std::filesystem::exists(install.root / "activation.pending"));
    CHECK(host.probes == 0U);
}

TEST_CASE("Activation refuses a current pointer that names another version", "[release][update]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    auto target = EncodeActiveUpdateRecord(request.staged.package);
    REQUIRE(target.HasValue());
    REQUIRE(files.WriteDurable(install.root / "active-version", std::as_bytes(std::span{target.Value()})).HasValue());
    Host host;
    CHECK(ActivateVerifiedUpdate(request, files, verifier, host).HasError());
    CHECK(Read(install.root / "active-version") == target.Value());
    CHECK_FALSE(std::filesystem::exists(install.root / "activation.pending"));
    CHECK(host.probes == 0U);
}

TEST_CASE("A foreign recovery journal cannot switch an installation", "[release][update]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    const std::string foreign = "horo-update-activation-v1\nforeign\n";
    REQUIRE(files.WriteDurable(install.root / "activation.pending", std::as_bytes(std::span{foreign})).HasValue());
    Host host;
    CHECK(ActivateVerifiedUpdate(request, files, verifier, host).HasError());
    auto previous = EncodeActiveUpdateRecord(request.current.package);
    REQUIRE(previous.HasValue());
    CHECK(Read(install.root / "active-version") == previous.Value());
    CHECK(Read(install.root / "activation.pending") == foreign);
    CHECK(host.probes == 0U);
}

TEST_CASE("First install activates only an authenticated healthy version", "[release][install]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    Host host;
    auto result = BootstrapVerifiedInstallation(request, files, verifier, host);
    REQUIRE(result.HasValue());
    CHECK(result.Value() == BootstrapInstallationOutcome::Installed);
    auto expected = EncodeActiveUpdateRecord(request.candidate.package);
    REQUIRE(expected.HasValue());
    CHECK(Read(install.root / "active-version") == expected.Value());
    CHECK_FALSE(std::filesystem::exists(install.root / "bootstrap.pending"));
    CHECK(host.preflights == 1U);
    CHECK(host.registrations == 1U);
    CHECK(host.unregistrations == 0U);
    CHECK(host.probes == 1U);
}

TEST_CASE("A running product lease blocks install activation without publishing an active version", "[release][install][lease]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    auto running = files.TryAcquireProductLaunch(install.root);
    REQUIRE(running.HasValue());
    Host host;
    CHECK(BootstrapVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK_FALSE(std::filesystem::exists(install.root / "active-version"));
    CHECK_FALSE(std::filesystem::exists(install.root / "bootstrap.pending"));
    CHECK(host.registrations == 0U);
}

TEST_CASE("A running product lease blocks update activation before mutation", "[release][update][lease]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = Request(install, files, verifier);
    const auto oldActive = Read(install.root / "active-version");
    auto running = files.TryAcquireProductLaunch(install.root);
    REQUIRE(running.HasValue());
    Host host;
    CHECK(ActivateVerifiedUpdate(request, files, verifier, host).HasError());
    CHECK(Read(install.root / "active-version") == oldActive);
    CHECK_FALSE(std::filesystem::exists(install.root / "activation.pending"));
    CHECK(host.probes == 0U);
}

TEST_CASE("Native bootstrap target rejects a different operating system or architecture", "[release][install]") {
    auto native = DetectBootstrapBuildTarget();
    REQUIRE(native.HasValue());
    auto artifact = Package("new", "signed package new").selection.artifact;
    artifact.platform = native.Value().platform;
    artifact.architecture = native.Value().architecture;
    CHECK(CheckBootstrapBuildTarget(artifact).HasValue());
    artifact.architecture =
        native.Value().architecture == DistributionArchitecture::X64 ? DistributionArchitecture::Arm64 : DistributionArchitecture::X64;
    CHECK(CheckBootstrapBuildTarget(artifact).HasError());
    artifact.architecture = native.Value().architecture;
    artifact.platform =
        native.Value().platform == DistributionPlatform::Linux ? DistributionPlatform::Windows : DistributionPlatform::Linux;
    CHECK(CheckBootstrapBuildTarget(artifact).HasError());
}

TEST_CASE("Failed first launch leaves no active product or operating-system registration", "[release][install]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    Host host;
    host.healthy = false;
    CHECK(BootstrapVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK_FALSE(std::filesystem::exists(install.root / "active-version"));
    CHECK_FALSE(std::filesystem::exists(install.root / "bootstrap.pending"));
    CHECK(host.registrations == 1U);
    CHECK(host.unregistrations == 1U);
}

TEST_CASE("First install refuses an existing active version and a failed preflight", "[release][install]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    Host host;
    host.admissible = false;
    CHECK(BootstrapVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK(host.registrations == 0U);
    host.admissible = true;
    const std::string existing = "another-installed-product";
    REQUIRE(files.WriteDurable(install.root / "active-version", std::as_bytes(std::span{existing})).HasValue());
    CHECK(BootstrapVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK(Read(install.root / "active-version") == existing);
    CHECK(host.registrations == 0U);
}

TEST_CASE("Interrupted first install removes only its own active pointer", "[release][install]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    auto record = EncodeActiveUpdateRecord(request.candidate.package);
    REQUIRE(record.HasValue());
    const std::string pending = "horo-bootstrap-install-v1\n" + record.Value();
    REQUIRE(files.WriteDurable(install.root / "bootstrap.pending", std::as_bytes(std::span{pending})).HasValue());
    REQUIRE(files.WriteDurable(install.root / "active-version", std::as_bytes(std::span{record.Value()})).HasValue());
    Host host;
    auto recovered = BootstrapVerifiedInstallation(request, files, verifier, host);
    REQUIRE(recovered.HasValue());
    CHECK(recovered.Value() == BootstrapInstallationOutcome::RecoveredIncomplete);
    CHECK_FALSE(std::filesystem::exists(install.root / "active-version"));
    CHECK_FALSE(std::filesystem::exists(install.root / "bootstrap.pending"));
    CHECK(host.unregistrations == 1U);
    CHECK(host.preflights == 0U);
    CHECK(host.probes == 0U);
}

TEST_CASE("First install rejects a changed staged tree before registration", "[release][install]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    {
        std::ofstream output(request.candidate.stageRoot / "bin/editor", std::ios::binary | std::ios::app);
        output << "changed";
    }
    Host host;
    CHECK(BootstrapVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK_FALSE(std::filesystem::exists(install.root / "active-version"));
    CHECK(host.registrations == 0U);
}

TEST_CASE("Failed operating-system registration is undone before an install can retry", "[release][install]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    Host host;
    host.registrationSucceeds = false;
    CHECK(BootstrapVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK_FALSE(std::filesystem::exists(install.root / "active-version"));
    CHECK_FALSE(std::filesystem::exists(install.root / "bootstrap.pending"));
    CHECK(host.registrations == 1U);
    CHECK(host.unregistrations == 1U);
    CHECK(host.probes == 0U);
}

TEST_CASE("A foreign first-install journal cannot remove another product", "[release][install]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    const std::string foreign = "horo-bootstrap-install-v1\nforeign\n";
    REQUIRE(files.WriteDurable(install.root / "bootstrap.pending", std::as_bytes(std::span{foreign})).HasValue());
    Host host;
    CHECK(BootstrapVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK(Read(install.root / "bootstrap.pending") == foreign);
    CHECK(host.unregistrations == 0U);
    CHECK(host.registrations == 0U);
}

TEST_CASE("First install respects the shared activation lock", "[release][install]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    auto held = files.TryAcquireExclusive(install.root / ".activation.lock", "existing-installation");
    REQUIRE(held.HasValue());
    Host host;
    CHECK(BootstrapVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK(host.preflights == 0U);
    CHECK(host.registrations == 0U);
    CHECK(host.probes == 0U);
}

TEST_CASE("Repair reauthenticates the active package before restoring integration", "[release][install]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    Host host;
    REQUIRE(BootstrapVerifiedInstallation(request, files, verifier, host).HasValue());
    REQUIRE(RepairVerifiedInstallation(request, files, verifier, host).HasValue());
    CHECK(host.registrations == 2U);
    CHECK(host.probes == 2U);
    {
        std::ofstream output(request.candidate.stageRoot / "bin/editor", std::ios::binary | std::ios::app);
        output << "corrupt";
    }
    CHECK(RepairVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK(host.registrations == 2U);
    CHECK(host.probes == 2U);
}

TEST_CASE("Uninstall deactivates before removing only the authenticated version", "[release][install]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    Host host;
    REQUIRE(BootstrapVerifiedInstallation(request, files, verifier, host).HasValue());
    std::filesystem::create_directories(install.root / "projects");
    const std::string project = "keep my project";
    REQUIRE(files.WriteDurable(install.root / "projects" / "game.horo", std::as_bytes(std::span{project})).HasValue());
    host.removeFiles = true;
    REQUIRE(UninstallVerifiedInstallation(request, files, verifier, host).HasValue());
    CHECK_FALSE(std::filesystem::exists(install.root / "active-version"));
    CHECK_FALSE(std::filesystem::exists(install.root / "bootstrap-uninstall.pending"));
    CHECK_FALSE(std::filesystem::exists(request.candidate.stageRoot));
    CHECK(Read(install.root / "projects" / "game.horo") == project);
    CHECK(host.unregistrations == 1U);
    CHECK(host.ownedRemovals == 1U);
}

TEST_CASE("Interrupted uninstall remains inactive and retries the same owned package", "[release][install]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    Host host;
    host.lockProbeFiles = &files;
    REQUIRE(BootstrapVerifiedInstallation(request, files, verifier, host).HasValue());
    host.ownedRemovalSucceeds = false;
    CHECK(UninstallVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK_FALSE(std::filesystem::exists(install.root / "active-version"));
    CHECK(std::filesystem::exists(install.root / "bootstrap-uninstall.pending"));
    CHECK(RepairVerifiedInstallation(request, files, verifier, host).HasError());
    host.ownedRemovalSucceeds = true;
    host.removeFiles = true;
    REQUIRE(UninstallVerifiedInstallation(request, files, verifier, host).HasValue());
    CHECK_FALSE(std::filesystem::exists(install.root / "bootstrap-uninstall.pending"));
    CHECK(host.unregistrations == 2U);
    CHECK(host.ownedRemovals == 2U);
    CHECK(host.lockHeldDuringRemoval);
}

TEST_CASE("Failed integration removal retains an inactive uninstall journal", "[release][install]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    Host host;
    REQUIRE(BootstrapVerifiedInstallation(request, files, verifier, host).HasValue());
    host.unregistrationSucceeds = false;
    CHECK(UninstallVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK_FALSE(std::filesystem::exists(install.root / "active-version"));
    CHECK(std::filesystem::exists(install.root / "bootstrap-uninstall.pending"));
    CHECK(host.ownedRemovals == 0U);
    host.unregistrationSucceeds = true;
    REQUIRE(UninstallVerifiedInstallation(request, files, verifier, host).HasValue());
    CHECK_FALSE(std::filesystem::exists(install.root / "bootstrap-uninstall.pending"));
    CHECK(host.ownedRemovals == 1U);
}

TEST_CASE("Uninstall refuses a different active product without removing its files", "[release][install]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto request = BootstrapRequest(install, files, verifier);
    Host host;
    const std::string foreign = "another-installed-product";
    REQUIRE(files.WriteDurable(install.root / "active-version", std::as_bytes(std::span{foreign})).HasValue());
    CHECK(UninstallVerifiedInstallation(request, files, verifier, host).HasError());
    CHECK(Read(install.root / "active-version") == foreign);
    CHECK(host.unregistrations == 0U);
    CHECK(host.ownedRemovals == 0U);
}

TEST_CASE("An acknowledged rollback selects the verified retained version", "[release][update][rollback]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto activation = Request(install, files, verifier);
    std::swap(activation.current, activation.staged);
    auto active = EncodeActiveUpdateRecord(activation.current.package);
    REQUIRE(active.HasValue());
    REQUIRE(files.WriteDurable(install.root / "active-version", std::as_bytes(std::span{active.Value()})).HasValue());
    UpdateRollbackRequest rollback{std::move(activation), UpdateRollbackReason::ExplicitUserRequest, UpdateRollbackAuthority::NormalPolicy,
                                   std::nullopt, true};
    rollback.minimumAllowedVersion = rollback.activation.staged.package.selection.artifact.version;
    Host host;
    auto result = RollbackVerifiedUpdate(rollback, files, verifier, host);
    REQUIRE(result.HasValue());
    CHECK(result.Value() == UpdateActivationOutcome::Activated);
    auto prior = EncodeActiveUpdateRecord(rollback.activation.staged.package);
    REQUIRE(prior.HasValue());
    CHECK(Read(install.root / "active-version") == prior.Value());
    CHECK(host.probes == 1U);
}

TEST_CASE("Explicit downgrade requires acknowledgement and respects the version floor", "[release][update][rollback]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto activation = Request(install, files, verifier);
    std::swap(activation.current, activation.staged);
    auto active = EncodeActiveUpdateRecord(activation.current.package);
    REQUIRE(active.HasValue());
    REQUIRE(files.WriteDurable(install.root / "active-version", std::as_bytes(std::span{active.Value()})).HasValue());
    UpdateRollbackRequest rollback{std::move(activation), UpdateRollbackReason::ExplicitUserRequest, UpdateRollbackAuthority::NormalPolicy,
                                   std::nullopt, false};
    Host host;
    auto unacknowledged = RollbackVerifiedUpdate(rollback, files, verifier, host);
    REQUIRE(unacknowledged.HasError());
    CHECK(unacknowledged.ErrorValue().code.Value() == UpdateRollbackErrors::ConfirmationRequired.code.Value());
    rollback.explicitWarningAcknowledged = true;
    rollback.minimumAllowedVersion = rollback.activation.current.package.selection.artifact.version;
    auto denied = RollbackVerifiedUpdate(rollback, files, verifier, host);
    REQUIRE(denied.HasError());
    CHECK(denied.ErrorValue().code.Value() == UpdateRollbackErrors::PolicyDenied.code.Value());
    rollback.minimumAllowedVersion.reset();
    auto missingFloor = RollbackVerifiedUpdate(rollback, files, verifier, host);
    REQUIRE(missingFloor.HasError());
    CHECK(missingFloor.ErrorValue().code.Value() == UpdateRollbackErrors::PolicyDenied.code.Value());
    CHECK(Read(install.root / "active-version") == active.Value());
    CHECK(host.stops == 0U);
}

TEST_CASE("Administrator recovery can restore a verified version below the normal floor", "[release][update][rollback]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto activation = Request(install, files, verifier);
    std::swap(activation.current, activation.staged);
    auto active = EncodeActiveUpdateRecord(activation.current.package);
    REQUIRE(active.HasValue());
    REQUIRE(files.WriteDurable(install.root / "active-version", std::as_bytes(std::span{active.Value()})).HasValue());
    UpdateRollbackRequest rollback{std::move(activation), UpdateRollbackReason::FailedActivation,
                                   UpdateRollbackAuthority::AdministratorRecovery, std::nullopt, false};
    rollback.minimumAllowedVersion = rollback.activation.current.package.selection.artifact.version;
    Host host;
    auto result = RollbackVerifiedUpdate(rollback, files, verifier, host);
    REQUIRE(result.HasValue());
    CHECK(result.Value() == UpdateActivationOutcome::Activated);
}

TEST_CASE("Rollback rejects a target that is not an older version", "[release][update][rollback]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    UpdateRollbackRequest rollback{Request(install, files, verifier), UpdateRollbackReason::FailedActivation,
                                   UpdateRollbackAuthority::NormalPolicy, std::nullopt, false};
    rollback.minimumAllowedVersion = rollback.activation.current.package.selection.artifact.version;
    Host host;
    auto rejected = RollbackVerifiedUpdate(rollback, files, verifier, host);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == UpdateRollbackErrors::InvalidTarget.code.Value());
    CHECK(host.stops == 0U);
}

TEST_CASE("An invalid rollback authority cannot bypass the version floor", "[release][update][rollback]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto activation = Request(install, files, verifier);
    std::swap(activation.current, activation.staged);
    UpdateRollbackRequest rollback{std::move(activation), UpdateRollbackReason::FailedActivation, static_cast<UpdateRollbackAuthority>(255),
                                   std::nullopt, false};
    Host host;
    auto rejected = RollbackVerifiedUpdate(rollback, files, verifier, host);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == UpdateRollbackErrors::PolicyDenied.code.Value());
    CHECK(host.stops == 0U);
}

TEST_CASE("Failed health during rollback leaves the newer version active", "[release][update][rollback]") {
    TemporaryInstall install;
    Horo::NativeDurableFileSystem files;
    auto verifier = Verifier();
    auto activation = Request(install, files, verifier);
    std::swap(activation.current, activation.staged);
    auto active = EncodeActiveUpdateRecord(activation.current.package);
    REQUIRE(active.HasValue());
    REQUIRE(files.WriteDurable(install.root / "active-version", std::as_bytes(std::span{active.Value()})).HasValue());
    UpdateRollbackRequest rollback{std::move(activation), UpdateRollbackReason::FailedActivation, UpdateRollbackAuthority::NormalPolicy,
                                   std::nullopt, false};
    rollback.minimumAllowedVersion = rollback.activation.staged.package.selection.artifact.version;
    Host host;
    host.healthy = false;
    CHECK(RollbackVerifiedUpdate(rollback, files, verifier, host).HasError());
    CHECK(Read(install.root / "active-version") == active.Value());
    CHECK_FALSE(std::filesystem::exists(install.root / "activation.pending"));
}
