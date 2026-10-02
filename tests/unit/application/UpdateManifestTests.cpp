#include "Horo/Release/UpdateManifest.h"
#include "Horo/Release/UpdateTrustRoot.h"
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

    [[nodiscard]] Security::DetachedSignatureEnvelope Signature(const Sha256Digest &digest, const std::string_view keyId = "key-1") {
        return {.publisherId = "com.horo.updates",
                .keyId = std::string{keyId},
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
        data.minimumRootRevision = 1U;
        data.packages.push_back({selection.Value(), "https://example.test/editor.tar.gz", PackageBytes.size(), digest, Signature(digest)});
        return data;
    }

    [[nodiscard]] UpdateManifestData DeltaManifestData() {
        auto data = ManifestData();
        auto artifact = data.packages.front().selection.artifact;
        artifact.package = {"editor-delta"};
        const auto selection = ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::DeltaZipArchive);
        REQUIRE(selection.HasValue());
        constexpr std::string_view PatchBytes = "patch";
        const auto patchDigest = ComputeSha256(Bytes(PatchBytes));
        data.deltas.push_back({.package = {selection.Value(), "https://example.test/editor-delta.zip", PatchBytes.size(), patchDigest,
                                           Signature(patchDigest)},
                               .fullPackage = data.packages.front().selection.artifact.package,
                               .baseInventoryDigest = ComputeSha256(Bytes("base inventory")),
                               .deltaInventoryDigest = ComputeSha256(Bytes("delta inventory")),
                               .targetInventoryDigest = ComputeSha256(Bytes("target inventory"))});
        return data;
    }

    [[nodiscard]] SignedUpdateManifest SignedManifest(UpdateManifestData data = ManifestData(), const std::string_view keyId = "key-1") {
        auto payload = BuildCanonicalUpdatePayload(data);
        REQUIRE(payload.HasValue());
        auto signature = Signature(ComputeSha256(Bytes(payload.Value())), keyId);
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
                .minimumAcceptedSequence = 7U,
                .now = 1500U};
    }

    [[nodiscard]] UpdateTrustRootData RootData(const std::string_view keyId = "key-1") {
        UpdateTrustRootData data;
        data.product = {DistributionProductKind::Editor, {}};
        data.revision = 1U;
        data.parentRevision = 0U;
        data.minimumManifestSequence = 7U;
        data.expiresAt = 3000U;
        std::vector<std::byte> publicKey(65U, std::byte{1});
        publicKey.front() = std::byte{0x04};
        data.keys.push_back({.publisherId = "com.horo.updates", .keyId = std::string{keyId}, .publicKey = std::move(publicKey)});
        return data;
    }

    [[nodiscard]] UpdateTrustRootSnapshot Root() {
        auto root = UpdateTrustRootSnapshot::Bootstrap(RootData());
        REQUIRE(root.HasValue());
        return std::move(root).Value();
    }
}  // namespace

TEST_CASE("Signed update manifest is canonical and admits only authenticated matching metadata", "[release][update]") {
    const auto manifest = SignedManifest();
    auto parsed = SignedUpdateManifest::ParseCanonical(manifest.CanonicalDocument());
    REQUIRE(parsed.HasValue());
    CHECK(parsed.Value().CanonicalPayload() == manifest.CanonicalPayload());
    auto roots = Root();
    auto provider = std::make_shared<TestSignatureProvider>();
    Security::ArtifactVerifier verifier{provider, roots.Roots()};
    auto context = Context();
    CHECK(VerifyUpdateManifest(manifest, context, roots, provider).HasValue());
    CHECK(VerifyUpdatePackage(manifest.Data().packages.front(), Bytes("package"), verifier).HasValue());
    CHECK(VerifyUpdatePackage(manifest.Data().packages.front(), Bytes("changed"), verifier).HasError());
    CHECK(VerifyUpdateManifest(manifest, context, roots, nullptr).HasError());
}

TEST_CASE("Signed update manifest v2 binds delta artifacts to one full package and exact inventories", "[release][update][delta]") {
    const auto manifest = SignedManifest(DeltaManifestData());
    CHECK(nlohmann::json::parse(manifest.CanonicalPayload()).at("schemaVersion") == 2);
    auto parsed = SignedUpdateManifest::ParseCanonical(manifest.CanonicalDocument());
    REQUIRE(parsed.HasValue());
    REQUIRE(parsed.Value().Data().deltas.size() == 1U);
    const auto &delta = parsed.Value().Data().deltas.front();
    CHECK(delta.fullPackage == parsed.Value().Data().packages.front().selection.artifact.package);
    CHECK(delta.baseInventoryDigest == ComputeSha256(Bytes("base inventory")));
    CHECK(delta.deltaInventoryDigest == ComputeSha256(Bytes("delta inventory")));
    CHECK(delta.targetInventoryDigest == ComputeSha256(Bytes("target inventory")));
    const auto roots = Root();
    const auto provider = std::make_shared<TestSignatureProvider>();
    Security::ArtifactVerifier verifier{provider, roots.Roots()};
    CHECK(VerifyUpdateManifest(parsed.Value(), Context(), roots, provider).HasValue());
    CHECK(VerifyUpdatePackage(delta.package, Bytes("patch"), verifier).HasValue());
    CHECK(VerifyUpdatePackage(delta.package, Bytes("altered"), verifier).HasError());
    const auto fullId = parsed.Value().Data().packages.front().selection.artifact.package;
    auto candidates = SelectUpdatePackageCandidates(parsed.Value(), Context(), roots, provider, fullId, delta.baseInventoryDigest);
    REQUIRE(candidates.HasValue());
    REQUIRE(candidates.Value().delta.has_value());
    CHECK(candidates.Value().delta->package.digest == delta.package.digest);
    auto firstAttempt = PlanUpdatePackageAttempt(candidates.Value(), UpdatePackageAttempt::Initial);
    REQUIRE(firstAttempt.has_value());
    CHECK(firstAttempt->selection.artifact.package == delta.package.selection.artifact.package);
    auto fallback = PlanUpdatePackageAttempt(candidates.Value(), UpdatePackageAttempt::AfterDeltaFailure);
    REQUIRE(fallback.has_value());
    CHECK(fallback->selection.artifact.package == fullId);
    CHECK_FALSE(PlanUpdatePackageAttempt(candidates.Value(), UpdatePackageAttempt::AfterFullFailure).has_value());
    auto fullOnly =
        SelectUpdatePackageCandidates(parsed.Value(), Context(), roots, provider, fullId, ComputeSha256(Bytes("another installed base")));
    REQUIRE(fullOnly.HasValue());
    CHECK(!fullOnly.Value().delta.has_value());
    auto fullAttempt = PlanUpdatePackageAttempt(fullOnly.Value(), UpdatePackageAttempt::Initial);
    REQUIRE(fullAttempt.has_value());
    CHECK(fullAttempt->selection.artifact.package == fullId);
    CHECK_FALSE(PlanUpdatePackageAttempt(fullOnly.Value(), UpdatePackageAttempt::AfterDeltaFailure).has_value());
    CHECK(
        SelectUpdatePackageCandidates(parsed.Value(), Context(), roots, provider, {"unknown-full"}, delta.baseInventoryDigest).HasError());
}

TEST_CASE("Delta metadata rejects missing full packages and ambiguous base identities", "[release][update][delta]") {
    auto data = DeltaManifestData();
    data.deltas.front().fullPackage = {"missing"};
    CHECK(BuildCanonicalUpdatePayload(data).HasError());

    data = DeltaManifestData();
    data.deltas.front().baseInventoryDigest = data.deltas.front().targetInventoryDigest;
    CHECK(BuildCanonicalUpdatePayload(data).HasError());

    data = DeltaManifestData();
    data.deltas.front().package.selection.artifact.package = data.packages.front().selection.artifact.package;
    CHECK(BuildCanonicalUpdatePayload(data).HasError());

    data = DeltaManifestData();
    data.deltas.push_back(data.deltas.front());
    CHECK(BuildCanonicalUpdatePayload(data).HasError());

    const auto manifest = SignedManifest(DeltaManifestData());
    auto document = nlohmann::json::parse(manifest.CanonicalDocument());
    document["manifest"].erase("deltas");
    CHECK(SignedUpdateManifest::ParseCanonical(document.dump()).HasError());
    document = nlohmann::json::parse(manifest.CanonicalDocument());
    document["manifest"]["deltas"][0]["targetInventorySha256"] = std::string(64U, '0');
    CHECK(SignedUpdateManifest::ParseCanonical(document.dump()).HasError());
}

TEST_CASE("Release notes and compatibility impacts are bounded signed manifest content", "[release][update]") {
    auto data = ManifestData();
    data.releaseNotes = "Improved editor stability.\nProject files stay unchanged.";
    data.compatibilityImpacts = {"Plugin API revision 2 is required."};
    const auto manifest = SignedManifest(data);
    auto parsed = SignedUpdateManifest::ParseCanonical(manifest.CanonicalDocument());
    REQUIRE(parsed.HasValue());
    CHECK(parsed.Value().Data().releaseNotes == data.releaseNotes);
    CHECK(parsed.Value().Data().compatibilityImpacts == data.compatibilityImpacts);
    CHECK(VerifyUpdateManifest(parsed.Value(), Context(), Root(), std::make_shared<TestSignatureProvider>()).HasValue());

    auto tampered = nlohmann::json::parse(manifest.CanonicalDocument());
    tampered["manifest"]["releaseNotes"] = "Untrusted instructions";
    auto tamperedManifest = SignedUpdateManifest::ParseCanonical(tampered.dump());
    REQUIRE(tamperedManifest.HasValue());
    CHECK(tamperedManifest.Value().CanonicalPayload() != manifest.CanonicalPayload());
    data.releaseNotes.assign(32U * 1024U + 1U, 'x');
    CHECK(BuildCanonicalUpdatePayload(data).HasError());
    data.releaseNotes.clear();
    data.compatibilityImpacts.assign(17U, "impact");
    CHECK(BuildCanonicalUpdatePayload(data).HasError());
    data.compatibilityImpacts.clear();
    data.releaseNotes = std::string{"safe\0hidden", 11U};
    CHECK(BuildCanonicalUpdatePayload(data).HasError());
    data.releaseNotes.clear();
    data.compatibilityImpacts = {std::string{"safe\0hidden", 11U}};
    CHECK(BuildCanonicalUpdatePayload(data).HasError());
}

TEST_CASE("Delta manifests retain signed presentation fields", "[release][update][delta]") {
    auto data = DeltaManifestData();
    data.releaseNotes = "Verified delta update.";
    data.compatibilityImpacts = {"Plugin compatibility must be checked."};
    data.minimumAllowedVersion = data.version;
    const auto manifest = SignedManifest(data);
    auto parsed = SignedUpdateManifest::ParseCanonical(manifest.CanonicalDocument());
    REQUIRE(parsed.HasValue());
    CHECK(parsed.Value().Data().releaseNotes == data.releaseNotes);
    CHECK(parsed.Value().Data().compatibilityImpacts == data.compatibilityImpacts);
    CHECK(parsed.Value().Data().deltas.size() == 1U);
    auto document = nlohmann::json::parse(manifest.CanonicalDocument());
    document["manifest"]["unknown"] = "rejected";
    CHECK(SignedUpdateManifest::ParseCanonical(document.dump()).HasError());
}

TEST_CASE("Update metadata rejects stale, wrong-target and rollback candidates", "[release][update]") {
    const auto manifest = SignedManifest();
    auto roots = Root();
    auto provider = std::make_shared<TestSignatureProvider>();
    auto context = Context();
    context.now = 2001U;
    CHECK(VerifyUpdateManifest(manifest, context, roots, provider).HasError());
    context.now = 1500U;
    context.minimumAcceptedSequence = 8U;
    CHECK(VerifyUpdateManifest(manifest, context, roots, provider).HasError());
    context.minimumAcceptedSequence = 7U;
    auto nextRootRequired = ManifestData();
    nextRootRequired.minimumRootRevision = 2U;
    CHECK(VerifyUpdateManifest(SignedManifest(nextRootRequired), context, roots, provider).HasError());
    context.installedProduct.kind = DistributionProductKind::EngineCli;
    CHECK(VerifyUpdateManifest(manifest, context, roots, provider).HasError());
    context.installedProduct.kind = DistributionProductKind::Editor;
    context.platform = DistributionPlatform::Windows;
    CHECK(VerifyUpdateManifest(manifest, context, roots, provider).HasError());
    context.platform = DistributionPlatform::Linux;
    context.installedVersion = EngineProductVersion{ParseReleaseVersion("0.5.0").Value()};
    CHECK(VerifyUpdateManifest(manifest, context, roots, provider).HasError());
    context.authorizedDowngrade = true;
    CHECK(VerifyUpdateManifest(manifest, context, roots, provider).HasValue());
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
    CHECK(VerifyUpdateManifest(parsedChange.Value(), Context(), Root(), std::make_shared<TestSignatureProvider>()).HasError());
    auto duplicatePackage = ManifestData();
    duplicatePackage.packages.push_back(duplicatePackage.packages.front());
    CHECK(BuildCanonicalUpdatePayload(duplicatePackage).HasError());
}

TEST_CASE("Trust-root rotation requires the installed old key and increases revision and sequence", "[release][update]") {
    const auto old = Root();
    auto nextData = RootData("key-2");
    nextData.revision = 2U;
    nextData.parentRevision = 1U;
    nextData.minimumManifestSequence = 8U;
    auto payload = BuildCanonicalUpdateTrustRootPayload(nextData);
    REQUIRE(payload.HasValue());
    auto signedRoot = SignedUpdateTrustRoot::Create(nextData, Signature(ComputeSha256(Bytes(payload.Value()))));
    REQUIRE(signedRoot.HasValue());
    auto parsed = SignedUpdateTrustRoot::ParseCanonical(signedRoot.Value().CanonicalDocument());
    REQUIRE(parsed.HasValue());
    auto provider = std::make_shared<TestSignatureProvider>();
    auto rotated = old.Transition(parsed.Value(), provider, 1500U);
    REQUIRE(rotated.HasValue());
    CHECK(rotated.Value().Revision() == 2U);
    CHECK(rotated.Value().MinimumManifestSequence() == 8U);
    CHECK(!rotated.Value().Roots()->Find("com.horo.updates", "key-1").has_value());
    CHECK(rotated.Value().Roots()->Find("com.horo.updates", "key-2").has_value());

    auto nextManifest = ManifestData();
    nextManifest.sequence = 8U;
    nextManifest.minimumRootRevision = 2U;
    nextManifest.packages.front().signature.keyId = "key-2";
    auto context = Context();
    context.minimumAcceptedSequence = 8U;
    CHECK(VerifyUpdateManifest(SignedManifest(nextManifest, "key-2"), context, rotated.Value(), provider).HasValue());
    CHECK(VerifyUpdateManifest(SignedManifest(), context, rotated.Value(), provider).HasError());
    CHECK(old.Transition(parsed.Value(), nullptr, 1500U).HasError());
    CHECK(old.Transition(parsed.Value(), provider, 3001U).HasError());

    auto rollback = nextData;
    rollback.minimumManifestSequence = 6U;
    auto rollbackPayload = BuildCanonicalUpdateTrustRootPayload(rollback);
    REQUIRE(rollbackPayload.HasValue());
    auto rollbackDocument = SignedUpdateTrustRoot::Create(rollback, Signature(ComputeSha256(Bytes(rollbackPayload.Value()))));
    REQUIRE(rollbackDocument.HasValue());
    CHECK(old.Transition(rollbackDocument.Value(), provider, 1500U).HasError());
}
