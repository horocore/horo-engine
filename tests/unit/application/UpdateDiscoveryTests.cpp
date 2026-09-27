#include "Horo/Release/UpdateDiscovery.h"
#include "Horo/Release/UpdateDiscoveryErrors.h"
#include "Horo/Release/UpdateManifestErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <span>
#include <string_view>

using namespace Horo;
using namespace Horo::Release;

namespace {
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

    struct Fixture final {
        SignedUpdateManifest manifest;
        UpdateTrustRootSnapshot roots;
        UpdateAdmissionContext context;
    };

    [[nodiscard]] SignedUpdateManifest MakeManifest(const DistributionArtifactIdentity &artifact,
                                                    const DistributionPackageSelection &selected) {
        constexpr std::string_view ArtifactBytes = "package";
        const auto digest = ComputeSha256(std::as_bytes(std::span{ArtifactBytes.data(), ArtifactBytes.size()}));
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
        data.packages.push_back({selected, "https://example.test/editor.tar.gz", ArtifactBytes.size(), digest, Signature(digest)});
        auto payload = BuildCanonicalUpdatePayload(data);
        REQUIRE(payload.HasValue());
        auto metadataDigest = ComputeSha256(std::as_bytes(std::span{payload.Value()}));
        auto manifest = SignedUpdateManifest::Create(std::move(data), Signature(metadataDigest));
        REQUIRE(manifest.HasValue());
        return std::move(manifest).Value();
    }

    [[nodiscard]] UpdateTrustRootSnapshot MakeRoots(const DistributionProductIdentity &product) {
        UpdateTrustRootData root;
        root.product = product;
        root.revision = 1U;
        root.minimumManifestSequence = 7U;
        root.expiresAt = 3000U;
        std::vector<std::byte> publicKey(65U, std::byte{1});
        publicKey.front() = std::byte{0x04};
        root.keys.push_back({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = std::move(publicKey)});
        auto roots = UpdateTrustRootSnapshot::Bootstrap(std::move(root));
        REQUIRE(roots.HasValue());
        return std::move(roots).Value();
    }

    [[nodiscard]] Fixture MakeFixture() {
        const auto version = ParseReleaseVersion("0.4.2");
        const auto installed = ParseReleaseVersion("0.4.1");
        REQUIRE(version.HasValue());
        REQUIRE(installed.HasValue());
        DistributionArtifactIdentity artifact;
        artifact.product = {DistributionProductKind::Editor, {}};
        artifact.version = EngineProductVersion{version.Value()};
        artifact.platform = DistributionPlatform::Linux;
        artifact.architecture = DistributionArchitecture::X64;
        artifact.build = {"build-42"};
        artifact.package = {"editor-linux"};
        artifact.installation = DistributionInstallationId{"horo-editor"};
        auto selected = ValidateDistributionPackageSelection(artifact, DistributionPackageFormat::TarGzip);
        REQUIRE(selected.HasValue());
        UpdateAdmissionContext context{.installedProduct = artifact.product,
                                       .installedVersion = EngineProductVersion{installed.Value()},
                                       .channel = "stable",
                                       .platform = DistributionPlatform::Linux,
                                       .architecture = DistributionArchitecture::X64,
                                       .updaterVersion = 1U,
                                       .minimumAcceptedSequence = 7U,
                                       .now = 1500U};
        return {MakeManifest(artifact, selected.Value()), MakeRoots(artifact.product), std::move(context)};
    }
}  // namespace

TEST_CASE("Update check policy keeps startup checks scheduled and requires explicit channel changes", "[release][update]") {
    UpdateDiscoveryPolicy policy;
    UpdateCheckContext context;
    context.now = 10'000U;
    auto planned = PlanUpdateCheck(policy, context);
    REQUIRE(planned.HasValue());
    CHECK(planned.Value().schedule);
    CHECK_FALSE(planned.Value().allowAutomaticDownload);
    CHECK_FALSE(planned.Value().allowTelemetry);
    context.lastSuccessfulCheck = 9'000U;
    CHECK_FALSE(PlanUpdateCheck(policy, context).Value().schedule);
    context.trigger = UpdateCheckTrigger::Manual;
    CHECK(PlanUpdateCheck(policy, context).Value().schedule);
    context.trigger = UpdateCheckTrigger::Startup;
    policy.selectedChannel.kind = UpdateChannelKind::Nightly;
    CHECK(PlanUpdateCheck(policy, context).HasError());
    context.explicitChannelChange = true;
    CHECK(PlanUpdateCheck(policy, context).HasValue());
    policy.selectedChannel = {UpdateChannelKind::Offline, "corp"};
    CHECK(PlanUpdateCheck(policy, context).HasValue());
    policy.selectedChannel.sourceId = "bad/path";
    CHECK(PlanUpdateCheck(policy, context).HasError());
    policy.selectedChannel = {};
    context.explicitChannelChange = false;
    policy.automaticChecks = false;
    policy.mandatorySecurityChecks = false;
    CHECK_FALSE(PlanUpdateCheck(policy, context).Value().schedule);
    policy.mandatorySecurityChecks = true;
    context.lastSuccessfulCheck.reset();
    CHECK(PlanUpdateCheck(policy, context).Value().schedule);
}

TEST_CASE("Discovery exposes only signed target-compatible packages", "[release][update]") {
    auto fixture = MakeFixture();
    auto provider = std::make_shared<AcceptingProvider>();
    UpdatePackagePreferences preferences{{DistributionPackageFormat::TarGzip}};
    auto found = AssessUpdate(fixture.manifest, fixture.context, fixture.roots, provider, preferences);
    CHECK(found.status == UpdateDiscoveryStatus::Available);
    REQUIRE(found.package.has_value());
    CHECK(found.package->selection.artifact.package.value == "editor-linux");
    preferences.formats = {DistributionPackageFormat::LinuxDeb};
    CHECK(AssessUpdate(fixture.manifest, fixture.context, fixture.roots, provider, preferences).status ==
          UpdateDiscoveryStatus::NoCompatiblePackage);
    preferences.formats = {DistributionPackageFormat::TarGzip};
    fixture.context.channel = "preview";
    CHECK(AssessUpdate(fixture.manifest, fixture.context, fixture.roots, provider, preferences).status == UpdateDiscoveryStatus::Rejected);
    fixture.context.channel = "stable";
    auto unavailable = UpdateSourceUnavailable(MakeError(UpdateManifestErrors::Stale));
    CHECK(unavailable.status == UpdateDiscoveryStatus::SourceUnavailable);
    CHECK_FALSE(unavailable.package.has_value());
    CHECK(unavailable.failure.has_value());
}

TEST_CASE("Discovery does not offer unchanged or unverifiable versions", "[release][update]") {
    auto fixture = MakeFixture();
    const auto installed = ParseReleaseVersion("0.4.2");
    REQUIRE(installed.HasValue());
    fixture.context.installedVersion = EngineProductVersion{installed.Value()};
    const auto provider = std::make_shared<AcceptingProvider>();
    const UpdatePackagePreferences preferences{{DistributionPackageFormat::TarGzip}};
    CHECK(AssessUpdate(fixture.manifest, fixture.context, fixture.roots, provider, preferences).status == UpdateDiscoveryStatus::UpToDate);
    auto rejected = AssessUpdate(fixture.manifest, fixture.context, fixture.roots, nullptr, preferences);
    CHECK(rejected.status == UpdateDiscoveryStatus::Rejected);
    CHECK_FALSE(rejected.package.has_value());
    CHECK(rejected.failure.has_value());
}

TEST_CASE("Automatic update checks reject contradictory policy and backward clocks", "[release][update]") {
    UpdateDiscoveryPolicy policy;
    UpdateCheckContext context;
    context.now = 10'000U;
    context.lastSuccessfulCheck = 12'000U;
    CHECK(PlanUpdateCheck(policy, context).HasError());
    context.trigger = UpdateCheckTrigger::Manual;
    CHECK(PlanUpdateCheck(policy, context).Value().schedule);
    context.lastSuccessfulCheck.reset();
    policy.intervalSeconds = 1U;
    CHECK(PlanUpdateCheck(policy, context).HasError());
    policy.intervalSeconds = 6U * 60U * 60U;
    policy.automaticChecks = false;
    policy.automaticDownloads = true;
    CHECK(PlanUpdateCheck(policy, context).HasError());
    policy.automaticDownloads = false;
    policy.selectedChannel = {UpdateChannelKind::Enterprise, "managed"};
    context.explicitChannelChange = true;
    CHECK(PlanUpdateCheck(policy, context).HasValue());
}
