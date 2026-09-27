#include "Horo/Release/BootstrapInstallation.h"
#include "Horo/Release/UpdateActivation.h"
#include "Horo/Release/UpdateActivationErrors.h"

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

    [[nodiscard]] UpdateActivationVersion Version(const TemporaryInstall &install, const std::string &id,
                                                  Horo::NativeDurableFileSystem &files, const Horo::Security::ArtifactVerifier &verifier) {
        const std::string payload = "signed package " + id;
        const std::string content = "editor " + id;
        auto package = Package(id, payload);
        auto checkpoint = CompleteCheckpoint(package);
        const auto packageFile = install.root / "versions" / (id + ".zip");
        const auto stageRoot = install.root / "versions" / id;
        std::filesystem::create_directories(stageRoot / "bin");
        {
            std::ofstream output(packageFile, std::ios::binary);
            output << payload;
        }
        {
            std::ofstream output(stageRoot / "bin/editor", std::ios::binary);
            output << content;
        }
        std::vector<UpdateStagedFile> inventory{{"bin/editor", content.size(), Horo::ComputeSha256(std::as_bytes(std::span{content}))}};
        constexpr UpdateArchiveLimits limits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U};
        REQUIRE(PublishVerifiedUpdateStage(package, checkpoint, packageFile, stageRoot, inventory, limits, files, verifier, {}).HasValue());
        return {std::move(package), std::move(checkpoint), packageFile, stageRoot, std::move(inventory)};
    }

    [[nodiscard]] UpdateActivationRequest Request(const TemporaryInstall &install, Horo::NativeDurableFileSystem &files,
                                                  const Horo::Security::ArtifactVerifier &verifier) {
        auto current = Version(install, "old", files, verifier);
        auto staged = Version(install, "new", files, verifier);
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
        unsigned stops{};
        unsigned probes{};
        unsigned preflights{};
        unsigned registrations{};
        unsigned unregistrations{};
        std::chrono::seconds observedTimeout{};
    };

    [[nodiscard]] BootstrapInstallationRequest BootstrapRequest(const TemporaryInstall &install, Horo::NativeDurableFileSystem &files,
                                                                const Horo::Security::ArtifactVerifier &verifier) {
        return {install.root,
                Version(install, "new", files, verifier),
                {.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U},
                std::chrono::seconds{2}};
    }
}  // namespace

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
    CHECK_FALSE(std::filesystem::exists(install.root / "activation.pending"));
    CHECK(host.stops == 1U);
    CHECK(host.probes == 1U);
    CHECK(host.observedTimeout == std::chrono::seconds{2});
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
    const std::string pending = "horo-update-activation-v1\n" + previous.Value() + target.Value();
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
