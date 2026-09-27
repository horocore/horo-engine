#include "Horo/Release/UpdateManifest.h"
#include "Horo/Security/SecurityErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>

using namespace Horo;
using namespace Horo::Release;

namespace {
    class TestSignatureProvider final : public Security::SignatureProvider {
    public:
        [[nodiscard]] bool Supports(const Security::SignatureAlgorithm algorithm) const noexcept override {
            return algorithm == Security::SignatureAlgorithm::EcdsaP256Sha256;
        }

        [[nodiscard]] Result<void> Verify(Security::SignatureAlgorithm, std::span<const std::byte>, const Sha256Digest &,
                                          const std::span<const std::byte> signature) const override {
            if (signature.size() != 64U || signature.front() != std::byte{0x5a})
                return Result<void>::Failure(MakeError(SecurityErrors::InvalidSignature));
            return Result<void>::Success();
        }
    };

    [[nodiscard]] std::span<const std::byte> Bytes(const std::string_view value) {
        return std::as_bytes(std::span{value.data(), value.size()});
    }

    [[nodiscard]] Security::DetachedSignatureEnvelope Signature(const Sha256Digest &digest) {
        return {.publisherId = "com.horo.updates",
                .keyId = "key-1",
                .artifactDigest = digest,
                .signature = std::vector<std::byte>(64U, std::byte{0x5a})};
    }

    [[nodiscard]] UpdateManifestData ManifestData() {
        const auto version = ParseReleaseVersion("0.4.2");
        REQUIRE(version.HasValue());
        DistributionArtifactIdentity artifact;
        artifact.product = {DistributionProductKind::Editor, {}};
        artifact.version = EngineProductVersion{version.Value()};
        artifact.platform = DistributionPlatform::Linux;
        artifact.architecture = DistributionArchitecture::X64;
        artifact.build = {"build-42"};
        artifact.package = {"editor-linux"};
        artifact.installation = DistributionInstallationId{"horo-editor"};
        const auto selection = ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::TarGzip);
        REQUIRE(selection.HasValue());
        constexpr std::string_view PackageBytes = "package";
        const Sha256Digest digest = ComputeSha256(Bytes(PackageBytes));
        UpdateManifestData data;
        data.product = artifact.product;
        data.version = artifact.version;
        data.build = artifact.build;
        data.channel = "stable";
        data.sequence = 7U;
        data.publishedAt = 1000U;
        data.expiresAt = 2000U;
        data.minimumUpdaterVersion = 1U;
        data.minimumRootRevision = 2U;
        data.packages.push_back({selection.Value(), "https://example.test/editor.tar.gz", PackageBytes.size(), digest, Signature(digest)});
        return data;
    }

    [[nodiscard]] SignedUpdateManifest SignedManifest(UpdateManifestData data = ManifestData()) {
        auto payload = BuildCanonicalUpdatePayload(data);
        REQUIRE(payload.HasValue());
        auto signature = Signature(ComputeSha256(Bytes(payload.Value())));
        auto manifest = SignedUpdateManifest::Create(std::move(data), std::move(signature));
        REQUIRE(manifest.HasValue());
        return std::move(manifest).Value();
    }

    [[nodiscard]] UpdateAdmissionContext Context() {
        const auto version = ParseReleaseVersion("0.4.1");
        REQUIRE(version.HasValue());
        return {.installedProduct = {DistributionProductKind::Editor, {}},
                .installedVersion = EngineProductVersion{version.Value()},
                .channel = "stable",
                .platform = DistributionPlatform::Linux,
                .architecture = DistributionArchitecture::X64,
                .updaterVersion = 1U,
                .trustedRootRevision = 2U,
                .minimumAcceptedSequence = 7U,
                .now = 1500U};
    }

    [[nodiscard]] Security::ArtifactVerifier Verifier(const std::shared_ptr<Security::TrustedRootStore> &roots) {
        return {std::make_shared<TestSignatureProvider>(), roots};
    }
}  // namespace

TEST_CASE("Signed update manifest is canonical and admits only authenticated matching metadata", "[release][update]") {
    const auto manifest = SignedManifest();
    auto parsed = SignedUpdateManifest::ParseCanonical(manifest.CanonicalDocument());
    REQUIRE(parsed.HasValue());
    CHECK(parsed.Value().CanonicalPayload() == manifest.CanonicalPayload());
    auto roots = std::make_shared<Security::TrustedRootStore>();
    REQUIRE(roots->Add({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = {std::byte{1}}}).HasValue());
    auto verifier = Verifier(roots);
    auto context = Context();
    CHECK(VerifyUpdateManifest(manifest, context, verifier).HasValue());
    CHECK(VerifyUpdatePackage(manifest.Data().packages.front(), Bytes("package"), verifier).HasValue());
    CHECK(VerifyUpdatePackage(manifest.Data().packages.front(), Bytes("changed"), verifier).HasError());
    REQUIRE(roots->Revoke("com.horo.updates", "key-1").HasValue());
    CHECK(VerifyUpdateManifest(manifest, context, verifier).HasError());
}

TEST_CASE("Update metadata rejects stale, wrong-target and rollback candidates", "[release][update]") {
    const auto manifest = SignedManifest();
    auto roots = std::make_shared<Security::TrustedRootStore>();
    REQUIRE(roots->Add({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = {std::byte{1}}}).HasValue());
    auto verifier = Verifier(roots);
    auto context = Context();
    context.now = 2001U;
    CHECK(VerifyUpdateManifest(manifest, context, verifier).HasError());
    context.now = 1500U;
    context.minimumAcceptedSequence = 8U;
    CHECK(VerifyUpdateManifest(manifest, context, verifier).HasError());
    context.minimumAcceptedSequence = 7U;
    context.trustedRootRevision = 1U;
    CHECK(VerifyUpdateManifest(manifest, context, verifier).HasError());
    context.trustedRootRevision = 2U;
    context.installedProduct.kind = DistributionProductKind::EngineCli;
    CHECK(VerifyUpdateManifest(manifest, context, verifier).HasError());
    context.installedProduct.kind = DistributionProductKind::Editor;
    context.platform = DistributionPlatform::Windows;
    CHECK(VerifyUpdateManifest(manifest, context, verifier).HasError());
    context.platform = DistributionPlatform::Linux;
    context.installedVersion = EngineProductVersion{ParseReleaseVersion("0.5.0").Value()};
    CHECK(VerifyUpdateManifest(manifest, context, verifier).HasError());
    context.authorizedDowngrade = true;
    CHECK(VerifyUpdateManifest(manifest, context, verifier).HasValue());
}

TEST_CASE("Update parser rejects duplicate, missing, oversized and version-skewed input", "[release][update]") {
    const auto manifest = SignedManifest();
    CHECK(SignedUpdateManifest::ParseCanonical("{}").HasError());
    CHECK(SignedUpdateManifest::ParseCanonical(std::string(129U * 1024U, 'x')).HasError());
    const auto document = nlohmann::json::parse(manifest.CanonicalDocument());
    auto duplicate = manifest.CanonicalDocument();
    duplicate.insert(1U, "\"manifest\":{},");
    CHECK(SignedUpdateManifest::ParseCanonical(duplicate).HasError());
    auto missing = document;
    missing["manifest"].erase("packages");
    CHECK(SignedUpdateManifest::ParseCanonical(missing.dump()).HasError());
    auto skewed = document;
    skewed["manifest"]["schemaVersion"] = 2;
    CHECK(SignedUpdateManifest::ParseCanonical(skewed.dump()).HasError());
    auto insecure = document;
    insecure["manifest"]["packages"][0]["url"] = "http://insecure.test/package";
    CHECK(SignedUpdateManifest::ParseCanonical(insecure.dump()).HasError());
    auto unsignedChange = document;
    unsignedChange["signature"]["signature"] = std::string(128U, '0');
    auto parsedChange = SignedUpdateManifest::ParseCanonical(unsignedChange.dump());
    REQUIRE(parsedChange.HasValue());
    auto roots = std::make_shared<Security::TrustedRootStore>();
    REQUIRE(roots->Add({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = {std::byte{1}}}).HasValue());
    CHECK(VerifyUpdateManifest(parsedChange.Value(), Context(), Verifier(roots)).HasError());
    auto duplicatePackage = ManifestData();
    duplicatePackage.packages.push_back(duplicatePackage.packages.front());
    CHECK(BuildCanonicalUpdatePayload(duplicatePackage).HasError());
}
