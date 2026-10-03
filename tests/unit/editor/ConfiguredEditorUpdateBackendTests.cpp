#include "Horo/Release/UpdateManifestErrors.h"
#include "Horo/Release/UpdateTransferErrors.h"
#include "Horo/Security/SecurityErrors.h"
#include "HoroEditor/app/ConfiguredEditorUpdateBackend.h"
#include "HoroEditor/app/ConfiguredEditorUpdateManifestSource.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <span>

using namespace Horo;
using namespace Horo::Editor;
using namespace Horo::Release;

namespace {
    class AcceptTestSignature final : public Security::SignatureProvider {
    public:
        bool Supports(Security::SignatureAlgorithm algorithm) const noexcept override {
            return algorithm == Security::SignatureAlgorithm::EcdsaP256Sha256;
        }

        Result<void> Verify(Security::SignatureAlgorithm, std::span<const std::byte>, const Sha256Digest &,
                            std::span<const std::byte> signature) const override {
            if (signature.size() != 64U || signature.front() != std::byte{0x5a})
                return Result<void>::Failure(MakeError(SecurityErrors::InvalidSignature));
            return Result<void>::Success();
        }
    };

    [[nodiscard]] Security::DetachedSignatureEnvelope Signature(const Sha256Digest &digest) {
        return {.publisherId = "com.horo.updates",
                .keyId = "key-1",
                .artifactDigest = digest,
                .signature = std::vector<std::byte>(64U, std::byte{0x5a})};
    }

    [[nodiscard]] std::string SignedDocument() {
        const auto version = ParseReleaseVersion("0.4.2");
        REQUIRE(version.HasValue());
        DistributionArtifactIdentity artifact;
        artifact.product = {DistributionProductKind::Editor, {}};
        artifact.version = EngineProductVersion{version.Value()};
        artifact.platform = DistributionPlatform::Windows;
        artifact.architecture = DistributionArchitecture::X64;
        artifact.build = {"build-42"};
        artifact.package = {"editor-linux"};
        artifact.installation = DistributionInstallationId{"horo-editor"};
        const auto selection = ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::ZipArchive);
        REQUIRE(selection.HasValue());
        const std::string packageBytes = "package";
        const Sha256Digest digest = ComputeSha256(std::as_bytes(std::span{packageBytes}));
        UpdateManifestData data;
        data.product = artifact.product;
        data.version = artifact.version;
        data.build = artifact.build;
        data.channel = "stable";
        data.sequence = 7U;
        data.publishedAt = 1000U;
        data.expiresAt = 2000U;
        data.minimumUpdaterVersion = 1U;
        data.minimumRootRevision = 1U;
        data.releaseNotes = "Signed release notes";
        data.compatibilityImpacts = {"Plugin API changes"};
        data.packages.push_back({selection.Value(), "https://example.test/editor.zip", packageBytes.size(), digest, Signature(digest)});
        const auto payload = BuildCanonicalUpdatePayload(data);
        REQUIRE(payload.HasValue());
        const auto manifest =
            SignedUpdateManifest::Create(std::move(data), Signature(ComputeSha256(std::as_bytes(std::span{payload.Value()}))));
        REQUIRE(manifest.HasValue());
        return manifest.Value().CanonicalDocument();
    }

    [[nodiscard]] UpdateTrustRootSnapshot TrustedRoot() {
        UpdateTrustRootData data;
        data.product = {DistributionProductKind::Editor, {}};
        data.revision = 1U;
        data.minimumManifestSequence = 7U;
        data.expiresAt = 3000U;
        std::vector<std::byte> key(65U, std::byte{1});
        key.front() = std::byte{0x04};
        data.keys.push_back({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = std::move(key)});
        auto root = UpdateTrustRootSnapshot::Bootstrap(std::move(data));
        REQUIRE(root.HasValue());
        return std::move(root).Value();
    }

    class MemorySource final : public IEditorUpdateManifestSource {
    public:
        explicit MemorySource(std::string document = SignedDocument(), std::string expectedChannel = "stable", const bool fail = false)
            : document(std::move(document)), channels{std::move(expectedChannel), "nightly"}, fail(fail) {}

        std::string document;
        std::array<std::string, 2> channels;
        std::size_t nextChannel{};
        bool fail;

        Result<EditorUpdateMetadata> Fetch(const EditorUpdateChannel &, CancellationToken) override {
            if (fail)
                return Result<EditorUpdateMetadata>::Failure(MakeError(UpdateManifestErrors::Invalid));
            return Result<EditorUpdateMetadata>::Success({document, channels.at(nextChannel++)});
        }
    };

    class RecordingHandoff final : public IEditorUpdateHandoff {
    public:
        unsigned activationRequests{};
        unsigned rollbackRequests{};

        Result<void> RequestActivation(const UpdatePackageRecord &, const std::filesystem::path &, CancellationToken) override {
            ++activationRequests;
            return Result<void>::Success();
        }

        Result<void> RequestRollback(CancellationToken) override {
            ++rollbackRequests;
            return Result<void>::Success();
        }
    };

    class MemoryManifestHttp final : public IEditorUpdateManifestHttpClient {
    public:
        std::string requestedUrl;

        Result<std::string> Get(const std::string &url, const EditorUpdateManifestHttpPolicy &, CancellationToken) override {
            requestedUrl = url;
            return Result<std::string>::Success(SignedDocument());
        }
    };

    class MemoryStager final : public IEditorUpdatePackageStager {
    public:
        explicit MemoryStager(const bool failFirst = false) : failFirst(failFirst) {}

        unsigned requests{};
        bool failFirst;

        Result<std::filesystem::path> Prepare(const UpdatePackageRecord &, const Security::ArtifactVerifier &, CancellationToken,
                                              const UpdateDownloadProgress &progress) override {
            ++requests;
            if (progress)
                progress(2U, 7U);
            if (failFirst && requests == 1U)
                return Result<std::filesystem::path>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
            return Result<std::filesystem::path>::Success("/private/ready.marker");
        }
    };

    [[nodiscard]] ConfiguredEditorUpdatePolicy Policy() {
        const auto installed = ParseReleaseVersion("0.4.1");
        REQUIRE(installed.HasValue());
        ConfiguredEditorUpdatePolicy policy{.admission = {.installedProduct = {DistributionProductKind::Editor, {}},
                                                          .installedVersion = EngineProductVersion{installed.Value()},
                                                          .channel = "stable",
                                                          .platform = DistributionPlatform::Windows,
                                                          .architecture = DistributionArchitecture::X64,
                                                          .updaterVersion = 1U,
                                                          .minimumAcceptedSequence = 7U},
                                            .roots = TrustedRoot(),
                                            .signatureProvider = std::make_shared<AcceptTestSignature>(),
                                            .preferences = {{DistributionPackageFormat::ZipArchive}},
                                            .trustedUnixTime = [] {
            return 1500U;
        }};
        return policy;
    }
}  // namespace

TEST_CASE("Configured editor update backend projects only verified release notes and binds the selected offer", "[editor][update]") {
    MemorySource source;
    MemoryStager stager;
    RecordingHandoff handoff;
    ConfiguredEditorUpdateBackend backend{Policy(), source, stager, handoff};
    auto checked = backend.Check({}, {});
    REQUIRE(checked.HasValue());
    REQUIRE(checked.Value().has_value());
    CHECK(checked.Value()->version == "0.4.2");
    CHECK(checked.Value()->releaseNotes == "Signed release notes");
    CHECK(checked.Value()->compatibilityImpacts == std::vector<std::string>{"Plugin API changes"});
    CHECK(checked.Value()->id != 0U);
    auto stale = *checked.Value();
    ++stale.id;
    CHECK(backend.Prepare(stale, {}, {}).HasError());
    CHECK(backend.Activate({}).HasError());
    CHECK(backend.Prepare(*checked.Value(), {}, {}).HasValue());
    CHECK(stager.requests == 1U);
    CHECK(backend.Activate({}).HasValue());
    CHECK(handoff.activationRequests == 1U);

    CHECK(backend.Check({}, {}).HasError());
    CHECK(backend.Prepare(*checked.Value(), {}, {}).HasError());
    CHECK(backend.Activate({}).HasError());
    CHECK(backend.Rollback({}).HasValue());
    CHECK(handoff.rollbackRequests == 1U);
}

TEST_CASE("Installed manifest source cannot admit a signed document from another selected channel", "[editor][update]") {
    MemoryManifestHttp http;
    ConfiguredEditorUpdateManifestSource
        source{{{{EditorUpdateChannelKind::Stable, {}}, "stable", "https://updates.example.test/stable/manifest.json"},
                {{EditorUpdateChannelKind::Preview, {}}, "preview", "https://updates.example.test/preview/manifest.json"}},
               {},
               http};
    MemoryStager stager;
    RecordingHandoff handoff;
    ConfiguredEditorUpdateBackend backend{Policy(), source, stager, handoff};
    const auto stable = backend.Check({}, {});
    REQUIRE(stable.HasValue());
    REQUIRE(stable.Value().has_value());
    CHECK(stable.Value()->releaseNotes == "Signed release notes");
    CHECK(http.requestedUrl == "https://updates.example.test/stable/manifest.json");

    const auto preview = backend.Check({EditorUpdateChannelKind::Preview, {}}, {});
    REQUIRE(preview.HasError());
    CHECK(preview.ErrorValue().code.Value() == UpdateManifestErrors::Incompatible.code.Value());
    CHECK(http.requestedUrl == "https://updates.example.test/preview/manifest.json");
    CHECK(backend.Prepare(*stable.Value(), {}, {}).HasError());
    CHECK(backend.Activate({}).HasError());
    CHECK(stager.requests == 0U);
    CHECK(handoff.activationRequests == 0U);
}

TEST_CASE("ZIP editor stager rejects a package of another format before touching download paths", "[editor][update]") {
    NativeDurableFileSystem files;
    ZipEditorUpdatePackageStager stager{{}, files};
    UpdatePackageRecord package;
    package.selection.format = DistributionPackageFormat::TarGzip;
    const auto result = stager.Prepare(package, Security::ArtifactVerifier{std::make_shared<AcceptTestSignature>(), {}}, {}, {});
    CHECK(result.HasError());
}

TEST_CASE("Linux tar gzip editor stager rejects wrong formats and invalid private paths", "[editor][update]") {
    NativeDurableFileSystem files;
    TarGzipEditorUpdatePackageStager stager{{}, files};
    UpdatePackageRecord package;
    package.selection.format = DistributionPackageFormat::ZipArchive;
    const Security::ArtifactVerifier verifier{std::make_shared<AcceptTestSignature>(), {}};
    CHECK(stager.Prepare(package, verifier, {}, {}).HasError());
    package.selection.format = DistributionPackageFormat::TarGzip;
    CHECK(stager.Prepare(package, verifier, {}, {}).HasError());
}

TEST_CASE("Configured editor update backend rejects untrusted metadata before offering a package", "[editor][update]") {
    const auto checkRejected = [](std::string document, std::string expectedChannel, const bool fetchFails) {
        MemorySource source{std::move(document), std::move(expectedChannel), fetchFails};
        MemoryStager stager;
        RecordingHandoff handoff;
        ConfiguredEditorUpdateBackend backend{Policy(), source, stager, handoff};
        CHECK(backend.Check({}, {}).HasError());
        CHECK(stager.requests == 0U);
    };
    checkRejected(SignedDocument(), "stable", true);
    checkRejected(SignedDocument(), "", false);
    checkRejected("{}", "stable", false);
}

TEST_CASE("Configured editor update backend propagates staging failure and only activates a ready stage", "[editor][update]") {
    MemorySource source;
    MemoryStager stager{true};
    RecordingHandoff handoff;
    ConfiguredEditorUpdateBackend backend{Policy(), source, stager, handoff};
    const auto checked = backend.Check({}, {});
    REQUIRE(checked.HasValue());
    REQUIRE(checked.Value().has_value());
    std::vector<EditorUpdatePhase> phases;
    CHECK(backend
              .Prepare(*checked.Value(), {}, [&phases](EditorUpdatePhase phase, std::uint64_t, std::uint64_t) {
        phases.push_back(phase);
    }).HasError());
    CHECK(phases == std::vector<EditorUpdatePhase>{EditorUpdatePhase::Downloading});
    CHECK(backend.Activate({}).HasError());
    CHECK(backend
              .Prepare(*checked.Value(), {}, [&phases](EditorUpdatePhase phase, std::uint64_t, std::uint64_t) {
        phases.push_back(phase);
    }).HasValue());
    CHECK(phases.back() == EditorUpdatePhase::Verifying);
    CHECK(backend.Activate({}).HasValue());
    CHECK(handoff.activationRequests == 1U);
}
