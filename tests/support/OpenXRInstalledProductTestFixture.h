#pragma once
/** @file OpenXRInstalledProductTestFixture.h
 * @brief Real signed archive/install/native-gate fixture for the application-owned OpenXR lifecycle.
 * @details Callers destroy every session and loaded native child before this fixture and its script parents.
 */
#include "Horo/Packages/PackageRequest.h"
#include "Horo/Security/SecurityErrors.h"
#include "HostSignatureDocument.h"
#include "OpenXRInstalledBindings.h"
#include "OpenXRInstalledProductSelection.h"
#include "PackageHostPolicy.h"
#include "support/OpenXRProductSigningTestSupport.h"
#include "unit/packages/PackageApplicationServiceTestSupport.h"

#include <fstream>
#include <iterator>

namespace Horo::Tests::OpenXRProduct {
    namespace PackageAPI = Horo::Packages;
    namespace Host = Horo::Application::Internal;
    using Json = nlohmann::json;

    template <typename T> T RequireValue(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "fixture operation succeeded"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline std::span<const std::byte> TextBytes(const std::string_view text) {
        return std::as_bytes(std::span{text.data(), text.size()});
    }

    /** @brief Supplies the exact signed archive; publisher verification remains in the production restore service. */
    class SignedSource final : public PackageAPI::IPackageRestoreSource {
    public:
        SignedSource(std::vector<std::byte> bytes, Security::DetachedSignatureEnvelope signature)
            : bytes_(std::move(bytes)), signature_(std::move(signature)) {}

        Result<PackageAPI::PackageRestoreArtifact> Fetch(const PackageAPI::PackageRestoreArtifactRequest &,
                                                         const CancellationToken &cancellation) override {
            if (cancellation.IsCancellationRequested())
                return Result<PackageAPI::PackageRestoreArtifact>::Failure(MakeError(PackageAPI::PackageRestoreErrors::Cancelled));
            return Result<PackageAPI::PackageRestoreArtifact>::Success({bytes_, signature_});
        }

    private:
        std::vector<std::byte> bytes_;
        Security::DetachedSignatureEnvelope signature_;
    };

    /** @brief Creates the sole sealed package snapshot using the real host, restore, install and ECDSA/native gates. */
    class InstalledProductFixture final {
    public:
        SigningKey key;
        Horo::Tests::Packages::TemporaryDirectory project{"horo XR application project ü"};
        Horo::Tests::Packages::TemporaryDirectory storage{"horo XR application storage ü"};
        NativeDurableFileSystem files;
        DeterministicClock metadataClock;
        SystemWallClock wall;
        Horo::Tests::PackageApplicationService::NoExecutionTrust executionTrust;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 8, .ioWorkerCount = 1}};
        const PackageAPI::HoroPackageId package{RequireValue(PackageAPI::HoroPackageId::Parse("com.horo.application-fixture"))};
        const PackageAPI::HoroPackageSourceId sourceId{RequireValue(PackageAPI::HoroPackageSourceId::Parse("fixture.source"))};
        const PackageAPI::PackageVersion version{RequireValue(PackageAPI::PackageVersion::Parse("1.0.0"))};
        const PackageAPI::PackagePlatform platform{RequireValue(Host::NativePackagePlatform())};
        std::shared_ptr<Security::TrustedRootStore> roots{std::make_shared<Security::TrustedRootStore>()};
        std::shared_ptr<const Security::ArtifactVerifier> verifier;
        std::shared_ptr<const Security::NativeArtifactGate> gate;
        Security::DetachedSignatureEnvelope loaderSignature;
        std::optional<Host::OpenXRInstalledProductPolicy> policy;
        std::unique_ptr<SignedSource> source;
        std::optional<PackageAPI::PackagePublisherVerificationService> publishers;
        std::unique_ptr<Host::PackageProjectHost> host;
        std::shared_ptr<Application::PackageApplication> application;

        explicit InstalledProductFixture(const std::filesystem::path &loader, std::optional<std::string> bindingDocument = std::nullopt) {
            REQUIRE(roots->Add(key.root).HasValue());
            verifier = std::make_shared<Security::ArtifactVerifier>(Security::CreateMbedTlsSignatureProvider(), roots);
            std::ifstream input{loader, std::ios::binary};
            REQUIRE(input.good());
            const std::string native{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
            REQUIRE_FALSE(native.empty());
            loaderSignature = key.Sign(TextBytes(native));
            gate = std::make_shared<Security::DetachedFileArtifactGate>(verifier,
                                                                        [signature = loaderSignature](const std::filesystem::path &path)
                                                                            -> Result<Security::DetachedSignatureEnvelope> {
                if (path.filename() != "libopenxr_loader.so")
                    return Result<Security::DetachedSignatureEnvelope>::Failure(MakeError(Horo::SecurityErrors::MissingEvidence));
                return Result<Security::DetachedSignatureEnvelope>::Success(signature);
            });
            const auto deployment = Deployment(loaderSignature.artifactDigest).dump();
            const auto component = Component(loaderSignature.artifactDigest, ComputeSha256(TextBytes(deployment))).dump();
            const auto bindings = bindingDocument ? std::move(*bindingDocument) : Bindings().dump();
            const std::vector<std::pair<std::string, std::string>> contents{{"horo-package.toml", "schemaVersion = 1\n"},
                                                                            {"runtime/xr/component.json", component},
                                                                            {"runtime/xr/deployment.json", deployment},
                                                                            {"runtime/xr/bindings.json", bindings},
                                                                            {"runtime/xr/libopenxr_loader.so", native}};
            const auto bytes = Archive(contents);
            const auto archive = RequireValue(PackageAPI::ValidatedPackageArchive::Verify(bytes));
            source = std::make_unique<SignedSource>(bytes, key.Sign(bytes));
            policy.emplace(Host::OpenXRInstalledProductPolicy{.package = package,
                                                              .componentDescriptor =
                                                                  RequireValue(PackageAPI::PackagePath::Parse("runtime/xr/component.json")),
                                                              .deploymentDocument = RequireValue(
                                                                  PackageAPI::PackagePath::Parse("runtime/xr/deployment.json")),
                                                              .component = {.platform = platform,
                                                                            .backend = RequireValue(XR::XRBackendId::Create(7)),
                                                                            .contract = XR::CurrentXRContractVersion,
                                                                            .publisher = RequireValue(PackageAPI::PackagePublisherId::Parse(
                                                                                key.root.publisherId)),
                                                                            .signingKey = key.root.keyId,
                                                                            .deploymentContract = "horo-test-linux-abi-v1",
                                                                            .platformBridge = 1,
                                                                            .rendererBridge = 1},
                                                              .deploymentSignature = key.Sign(TextBytes(deployment)),
                                                              .verifier = verifier,
                                                              .nativeGate = gate,
                                                              .temporaryParent = std::filesystem::canonical(storage.Path())});
            PrepareHost(archive);
            Install();
        }

        InstalledProductFixture(const InstalledProductFixture &) = delete;
        InstalledProductFixture &operator=(const InstalledProductFixture &) = delete;

        ~InstalledProductFixture() {
            if (host)
                static_cast<void>(host->Close());
            jobs.Shutdown(ShutdownPolicy::Cancel);
            if (host && host->Close().HasError())
                std::terminate();
            application.reset();
            host.reset();
        }

        [[nodiscard]] std::filesystem::path Root() const {
            return std::filesystem::canonical(project.Path());
        }

        [[nodiscard]] Host::OpenXRInstalledProductCandidate Candidate() const {
            return RequireValue(Host::SelectInstalledOpenXRProduct(*host, Root(), host->Generation(), &*policy, {}));
        }

        [[nodiscard]] std::shared_ptr<const Host::OpenXRApplicationBindings> ReadBindings() const {
            const auto candidate = Candidate();
            return RequireValue(Host::ReadInstalledOpenXRBindings(candidate.component, RequireValue(PackageAPI::PackagePath::Parse(
                                                                                           "runtime/xr/bindings.json"))));
        }

        /** @brief Returns the same canonical fixture declaration bytes for exact signed parser-input regressions. */
        static std::string BindingDocument() {
            return Bindings().dump();
        }

        /** @brief Writes independent operator-approved host inputs, never extracting trust from the package. */
        std::vector<std::string> BootstrapArguments() {
            const auto directory = std::filesystem::canonical(storage.Path()) / "operator-approval";
            std::filesystem::create_directories(directory);
            const auto rootsDocument = ApprovedRootsDocument();
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(wall.UtcNow().time_since_epoch()).count();
            const Json catalog{{"schemaVersion", 1},
                               {"backend", 7},
                               {"package", package.Value()},
                               {"component", "runtime/xr/component.json"},
                               {"deployment", "runtime/xr/deployment.json"},
                               {"deploymentSignature", Host::EncodeDetachedSignatureDocument(policy->deploymentSignature)},
                               {"loaderSignature", Host::EncodeDetachedSignatureDocument(loaderSignature)},
                               {"bindings", "runtime/xr/bindings.json"},
                               {"deploymentContract", "horo-test-linux-abi-v1"},
                               {"platformBridge", 1},
                               {"rendererBridge", 1},
                               {"expiresAt", now + 3600},
                               {"profile", 0},
                               {"limits", {{"views", 2}, {"spaces", 16}, {"actions", 16}, {"devices", 3}}}};
            const auto catalogBytes = catalog.dump();
            const std::array documents{std::pair{"roots.json", rootsDocument.dump()}, std::pair{"catalog.json", catalogBytes},
                                       std::pair{"signature.json",
                                                 Host::EncodeDetachedSignatureDocument(key.Sign(TextBytes(catalogBytes)))}};
            for (const auto &[name, bytes] : documents) {
                std::ofstream output{directory / name, std::ios::binary | std::ios::trunc};
                output << bytes;
                REQUIRE(output.good());
            }
            return {"HoroEditor",
                    "--xr-host-roots",
                    (directory / "roots.json").string(),
                    "--xr-host-catalog",
                    (directory / "catalog.json").string(),
                    "--xr-host-catalog-signature",
                    (directory / "signature.json").string()};
        }

        /** @brief Completes the installer handoff before the executable constructs its sole live package owner. */
        void RetireInstaller() {
            REQUIRE(host->Close().HasValue());
            application.reset();
            host.reset();
            jobs.Shutdown(ShutdownPolicy::Cancel);
        }

    private:
        /** @brief Serializes independent operator trust roots without deriving authority from package contents. */
        Json ApprovedRootsDocument() const {
            std::string publicKey;
            constexpr std::string_view digits = "0123456789abcdef";
            for (const auto byte : key.root.publicKey) {
                const auto value = std::to_integer<unsigned>(byte);
                publicKey.push_back(digits[value >> 4U]);
                publicKey.push_back(digits[value & 15U]);
            }
            return Json{{"schemaVersion", 1},
                        {"allowUnsigned", false},
                        {"publishers",
                         {{{"publisherId", key.root.publisherId},
                           {"keyId", key.root.keyId},
                           {"algorithm", "ecdsa-p256-sha256"},
                           {"publicKeyHex", publicKey},
                           {"expiresAtUnixMilliseconds", 0},
                           {"revoked", false}}}}};
        }

        Json Deployment(const Sha256Digest &loader) const {
            return {{"schemaVersion", 1},
                    {"backend", 7},
                    {"build", "test-build"},
                    {"releaseChannel", "development"},
                    {"platform", {{"os", platform.operatingSystem}, {"architecture", platform.architecture}, {"sdkAbi", platform.sdkAbi}}},
                    {"deploymentContract", "horo-test-linux-abi-v1"},
                    {"loaderDigest", FormatSha256(loader)},
                    {"bundledDependencies", Json::array()},
                    {"osAbiDependencies", {"libc.so.6"}}};
        }

        Json Component(const Sha256Digest &loader, const Sha256Digest &provenance) const {
            return {{"schemaVersion", 1},
                    {"backend", 7},
                    {"contractVersion", {1, 1, 0}},
                    {"build", "test-build"},
                    {"releaseChannel", "development"},
                    {"platform", {{"os", platform.operatingSystem}, {"architecture", platform.architecture}, {"sdkAbi", platform.sdkAbi}}},
                    {"linkage", "dynamic"},
                    {"loaderSource", "bundled_verified"},
                    {"loader", {{"path", "runtime/xr/libopenxr_loader.so"}, {"sha256", FormatSha256(loader)}}},
                    {"loaderVersions", {{"minimum", {1, 0, 0}}, {"maximum", {1, 1, 63}}}},
                    {"bridges", {{"platform", 1}, {"renderer", 1}}},
                    {"dependencies", Json::array()},
                    {"licenses", FormatSha256(ComputeSha256({}))},
                    {"provenance", FormatSha256(provenance)}};
        }

        static Json Bindings() {
            const auto role = static_cast<std::uint64_t>(XR::XRTrackedDeviceRole::LeftController);
            const auto type = static_cast<std::uint64_t>(Input::ActionValueType::Digital);
            return {{"schemaVersion", 1},
                    {"genericFallback", 1},
                    {"profiles", {{{"id", 1}, {"path", "/interaction_profiles/khr/simple_controller"}}}},
                    {"sets", {{{"id", 1}, {"label", "Gameplay"}}}},
                    {"actions",
                     {{{"id", "select"},
                       {"context", "gameplay"},
                       {"type", type},
                       {"set", 1},
                       {"role", role},
                       {"required", true},
                       {"label", "Select"}}}},
                    {"controls",
                     {{{"profile", 1}, {"id", 1}, {"role", role}, {"type", type}, {"path", "/user/hand/left/input/select/click"}}}},
                    {"suggestions", {{{"action", "select"}, {"profile", 1}, {"control", 1}}}},
                    {"poses", Json::array()}};
        }

        static std::vector<std::byte> Archive(const std::vector<std::pair<std::string, std::string>> &contents) {
            Json entries = Json::array();
            mz_zip_archive zip{};
            REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
            for (const auto &[name, bytes] : contents) {
                auto entry = Horo::Tests::Packages::FileInventoryEntry(name, bytes);
                if (name.starts_with("runtime/xr/"))
                    entry["contributionRoot"] = "runtime/xr";
                entry["executable"] = name.ends_with(".so");
                entries.push_back(std::move(entry));
                REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(), MZ_BEST_COMPRESSION));
            }
            const auto inventory = Json{{"schemaVersion", 1}, {"files", entries}}.dump();
            REQUIRE(mz_zip_writer_add_mem(&zip, "files.manifest.json", inventory.data(), inventory.size(), MZ_BEST_COMPRESSION));
            auto archive = Horo::Tests::Packages::FinalizeArchive(zip);
            for (const auto &file : files) {
                if (file.first.ends_with(".so"))
                    Horo::Tests::Packages::SetArchiveFileUnixMode(archive, file.first, 0100755);
            }
            return archive;
        }

        void PrepareHost(const PackageAPI::ValidatedPackageArchive &archive) {
            std::filesystem::create_directories(Root() / ".horo");
            const Json intent{{"sources", {{"fixture.source", {{"kind", "public-registry"}, {"registry", "official"}}}}},
                              {"dependencies", {{"com.horo.application-fixture", {{"source", "fixture.source"}, {"version", "1.0.0"}}}}}};
            const auto request = RequireValue(PackageAPI::ValidatedPackageRequest::Parse(intent.dump()));
            const PackageAPI::PackageResolutionPlan plan{.packages = {{package, version, sourceId, archive.Digest(), {}}}};
            const PackageAPI::PackageLockArtifact artifact{.package = package,
                                                           .version = version,
                                                           .source = sourceId,
                                                           .artifactDigest = archive.Digest(),
                                                           .manifestDigest = archive.PackageManifestDigest(),
                                                           .fileManifestDigest = archive.Manifest().Digest(),
                                                           .packageFormatVersion = 1,
                                                           .platforms = {},
                                                           .contributions = {"assets"}};
            const auto lock = RequireValue(PackageAPI::ValidatedPackageLockfileV1::Generate(plan, std::span{&package, 1U}, request.Digest(),
                                                                                            std::span{&artifact, 1U}));
            Write("packages.json", intent.dump());
            Write("packages.lock", lock.SerializeCanonical());
            publishers.emplace(RequireValue(
                PackageAPI::PackagePublisherVerificationService::Create({.allowUnsigned = false,
                                                                         .publishers = {{.publisher = policy->component.publisher,
                                                                                         .keyId = key.root.keyId,
                                                                                         .algorithm = key.root.algorithm,
                                                                                         .publicKey = key.root.publicKey}},
                                                                         .auditCapacity = 32},
                                                                        Security::CreateMbedTlsSignatureProvider())));
            host = RequireValue(Host::PackageProjectHost::Create({jobs, files, metadataClock, wall, executionTrust},
                                                                 {.cacheRoot = std::filesystem::canonical(storage.Path()) / "cache",
                                                                  .platform = platform,
                                                                  .lifecycle = {.temporaryRoot = std::filesystem::canonical(storage.Path()),
                                                                                .artifactGate = gate},
                                                                  .mode = PackageAPI::PackageRestoreMode::Online},
                                                                 Duration::FromMilliseconds(1000), source.get(), &*publishers));
        }

        void Write(const char *name, const std::string &bytes) const {
            std::ofstream output{Root() / ".horo" / name, std::ios::binary | std::ios::trunc};
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(output.good());
        }

        void Install() {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < deadline) {
                auto acquired = host->Acquire(Root());
                if (acquired.HasValue()) {
                    application = std::move(acquired).Value();
                    break;
                }
                REQUIRE(acquired.ErrorValue().code.Value() == "packages.host.preparing");
                std::this_thread::yield();
            }
            REQUIRE(application);
            const auto operation = RequireValue(application->Install());
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < deadline) {
                REQUIRE(host->Pump(PackageAPI::PackageActivationBoundary::Quiescent).HasValue());
                const auto current = RequireValue(application->Query(operation));
                if (current.outcome != Application::PackageOperationOutcome::Running) {
                    REQUIRE(current.outcome == Application::PackageOperationOutcome::Succeeded);
                    return;
                }
                std::this_thread::yield();
            }
            FAIL("Signed application fixture installation exceeded its owned deadline");
        }
    };
}  // namespace Horo::Tests::OpenXRProduct
