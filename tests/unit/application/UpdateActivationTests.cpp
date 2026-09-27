#include "Horo/Release/UpdateActivation.h"
#include "Horo/Release/UpdateActivationErrors.h"
#include "Horo/Release/UpdateRollback.h"
#include "Horo/Release/UpdateRollbackErrors.h"

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

    class Host final : public IUpdateActivationHost {
    public:
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
        unsigned stops{};
        unsigned probes{};
        std::chrono::seconds observedTimeout{};
    };
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
