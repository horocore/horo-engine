#include "Horo/Release/ReleasePreflight.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {
    using namespace Horo;
    using namespace Horo::Release;

    [[nodiscard]] Sha256Digest Digest(const std::string_view text) {
        return ComputeSha256(std::as_bytes(std::span{text.data(), text.size()}));
    }

    [[nodiscard]] EffectiveReleaseProfile Profile() {
        ReleaseProfilePreset preset;
        preset.id = {"shipping"};
        preset.product = DistributionProductIdentity{DistributionProductKind::Editor, {}};
        preset.artifactClass = DistributionArtifactClass::InstallableProduct;
        preset.platform = DistributionPlatform::Linux;
        preset.packageFormat = DistributionPackageFormat::TarGzip;
        preset.content = ReleaseContentPolicy{true, true, ReleaseAssetPolicy::SinglePackage, false, false};
        preset.symbols = ReleaseSymbolPolicy::Omit;
        preset.signing = ReleaseSigningPolicy::Disabled;
        preset.notarizationRequired = false;
        preset.includeLicensesAndNotices = true;
        preset.includeReleaseNotes = true;
        preset.updateEligible = false;
        preset.patchEligible = false;
        preset.eligibleDestinations = std::vector<ReleaseDestinationId>{};
        preset.requiredCapabilities = std::vector<ReleaseCapabilityId>{{"release.packaging"}};
        auto catalog = ReleaseProfileCatalog::Create({std::move(preset)});
        REQUIRE(catalog.HasValue());
        const ReleaseCapabilityId capability{"release.packaging"};
        auto profile = catalog.Value().Resolve({"shipping"}, std::span{&capability, 1U});
        REQUIRE(profile.HasValue());
        return std::move(profile).Value();
    }

    [[nodiscard]] ReleasePreflightRequest Request() {
        auto version = ParseReleaseVersion("0.4.2");
        REQUIRE(version.HasValue());
        return {.projectRoot = std::filesystem::temp_directory_path() / "horo-release-source",
                .projectId = "horo-editor",
                .version = {EngineProductVersion{version.Value()}, ReleaseSourceRevision{"commit-123"}},
                .profile = Profile(),
                .architecture = DistributionArchitecture::X64,
                .configuration = ReleaseBuildConfiguration::Shipping,
                .toolchainId = "clang-20",
                .outputRoot = std::filesystem::temp_directory_path() / "horo-release-output",
                .requiredFreeBytes = 1024,
                .credentials = {{42}},
                .reproducible = true};
    }

    [[nodiscard]] ReleasePreflightFacts Facts(const ReleasePreflightRequest &request) {
        return {.requestedProjectRoot = request.projectRoot,
                .requestedOutputRoot = request.outputRoot,
                .canonicalProjectRoot = request.projectRoot,
                .canonicalOutputRoot = request.outputRoot,
                .projectReadable = true,
                .outputWritable = true,
                .outputExists = false,
                .availableBytes = 2048,
                .hostPlatform = DistributionPlatform::Linux,
                .targetSupported = true,
                .crossCompilerAvailable = false,
                .toolchainAvailable = true,
                .currentVersion = request.version,
                .sourceTreeDigest = Digest("source-tree"),
                .dependencyLockDigest = Digest("dependency-lock"),
                .profileDigest = Digest(request.profile.SerializeCanonical()),
                .toolchainDigest = Digest("toolchain"),
                .policyDigest = Digest("policy"),
                .availableCapabilities = {{"release.packaging"}},
                .availableCredentials = {{42}}};
    }

    [[nodiscard]] bool HasIssue(const ReleasePreflightOutcome &outcome, const ReleasePreflightIssueCode code) {
        return std::ranges::any_of(outcome.issues, [code](const ReleasePreflightIssue &issue) {
            return issue.code == code;
        });
    }
}  // namespace

TEST_CASE("Release preflight captures exact validated inputs without exposing credential values",
          "[unit][application][release][preflight]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    const ReleasePreflightOutcome outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.issues.empty());
    REQUIRE(outcome.plan.has_value());
    CHECK(outcome.plan->Request().projectId == "horo-editor");
    CHECK(outcome.plan->Request().projectRoot == request.projectRoot);
    CHECK(outcome.plan->Request().outputRoot == request.outputRoot);
    CHECK(outcome.plan->Identities().sourceTree == facts.sourceTreeDigest);
    CHECK(outcome.plan->Summary().find("42") == std::string::npos);
    CHECK(outcome.plan->Summary().find(FormatSha256(facts.sourceTreeDigest)) != std::string::npos);
    CHECK(outcome.plan->Summary().find(FormatSha256(facts.dependencyLockDigest)) != std::string::npos);

    const nlohmann::json snapshot = nlohmann::json::parse(outcome.plan->SerializeCanonical());
    CHECK(snapshot.at("version").at("value") == "0.4.2");
    CHECK(snapshot.at("target").at("configuration") == "shipping");
    CHECK(snapshot.at("credentialHandles") == nlohmann::json::array({42}));
    CHECK(snapshot.at("identities").at("dependencyLock") == FormatSha256(facts.dependencyLockDigest));
    CHECK(outcome.plan->SerializeCanonical() == outcome.plan->SerializeCanonical());
    CHECK(ValidateReleaseInputFreeze(*outcome.plan, facts).empty());
}

TEST_CASE("Release plan retains requested paths beside resolved execution roots", "[unit][application][release][preflight]") {
    const ReleasePreflightRequest request = Request();
    ReleasePreflightFacts facts = Facts(request);
    facts.canonicalProjectRoot = std::filesystem::temp_directory_path() / "resolved-release-source";
    facts.canonicalOutputRoot = std::filesystem::temp_directory_path() / "resolved-release-output";

    const ReleasePreflightOutcome outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    CHECK(outcome.plan->Request().projectRoot == request.projectRoot);
    CHECK(outcome.plan->Request().outputRoot == request.outputRoot);
    CHECK(outcome.plan->ProjectRoot() == facts.canonicalProjectRoot);
    CHECK(outcome.plan->OutputRoot() == facts.canonicalOutputRoot);
    CHECK(ValidateReleaseInputFreeze(*outcome.plan, facts).empty());
}

TEST_CASE("Release preflight aggregates independent failures without creating output", "[unit][application][release][preflight]") {
    ReleasePreflightRequest request = Request();
    request.outputRoot =
        std::filesystem::temp_directory_path() /
        ("horo-release-preflight-no-output-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE_FALSE(std::filesystem::exists(request.outputRoot));
    ReleasePreflightFacts facts = Facts(request);
    facts.projectReadable = false;
    facts.toolchainAvailable = false;
    facts.targetSupported = false;
    facts.outputExists = true;
    facts.outputWritable = false;
    facts.availableBytes = 1;
    facts.availableCredentials.clear();
    facts.availableCapabilities.clear();

    const ReleasePreflightOutcome outcome = PreflightRelease(request, facts);
    REQUIRE_FALSE(outcome.plan.has_value());
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::ProjectUnavailable));
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::ToolchainUnavailable));
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::TargetUnsupported));
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::OutputCollision));
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::OutputUnavailable));
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::InsufficientSpace));
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::CredentialUnavailable));
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::CapabilityUnavailable));
    CHECK_FALSE(std::filesystem::exists(request.outputRoot));
}

TEST_CASE("Release plan rejects changed source and dependency identities before execution", "[unit][application][release][preflight]") {
    const ReleasePreflightRequest request = Request();
    ReleasePreflightFacts facts = Facts(request);
    const ReleasePreflightOutcome outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());

    facts.sourceTreeDigest = Digest("new-source");
    facts.dependencyLockDigest = Digest("new-lock");
    const std::vector<ReleasePreflightIssue> changes = ValidateReleaseInputFreeze(*outcome.plan, facts);
    REQUIRE(changes.size() == 2);
    CHECK(changes[0].field == "source");
    CHECK(changes[1].field == "dependencyLock");
}

TEST_CASE("Release preflight reports source profile and cross-compile conflicts together", "[unit][application][release][preflight]") {
    const ReleasePreflightRequest request = Request();
    ReleasePreflightFacts facts = Facts(request);
    facts.currentVersion.sourceRevision.value = "different-commit";
    facts.profileDigest = Digest("different-profile");
    facts.hostPlatform = DistributionPlatform::Windows;

    const ReleasePreflightOutcome outcome = PreflightRelease(request, facts);
    REQUIRE_FALSE(outcome.plan.has_value());
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::SourceChanged));
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::ProfileChanged));
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::CrossCompilerUnavailable));
}

TEST_CASE("Release preflight rejects observations captured for another request", "[unit][application][release][preflight]") {
    const ReleasePreflightRequest request = Request();
    ReleasePreflightFacts facts = Facts(request);
    facts.requestedProjectRoot = "/tmp/another-project";
    facts.requestedOutputRoot = "/tmp/another-output";

    const ReleasePreflightOutcome outcome = PreflightRelease(request, facts);
    REQUIRE_FALSE(outcome.plan.has_value());
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::ProjectUnavailable));
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::OutputUnavailable));

    const ReleasePreflightFacts originalFacts = Facts(request);
    const ReleasePreflightOutcome accepted = PreflightRelease(request, originalFacts);
    REQUIRE(accepted.plan.has_value());
    const auto drift = ValidateReleaseInputFreeze(*accepted.plan, facts);
    CHECK(std::ranges::any_of(drift, [](const ReleasePreflightIssue &issue) {
        return issue.field == "projectRequest";
    }));
    CHECK(std::ranges::any_of(drift, [](const ReleasePreflightIssue &issue) {
        return issue.field == "outputRequest";
    }));
}

TEST_CASE("Release preflight rejects malformed identity and credential requests", "[unit][application][release][preflight]") {
    ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    request.projectId.clear();
    request.projectRoot = "/tmp/invalid\nproject";
    request.toolchainId.clear();
    std::get<EngineProductVersion>(request.version.productVersion).value.prerelease = "invalid..prerelease";
    request.credentials = {{0}, {0}};

    const ReleasePreflightOutcome outcome = PreflightRelease(request, facts);
    REQUIRE_FALSE(outcome.plan.has_value());
    CHECK(HasIssue(outcome, ReleasePreflightIssueCode::InvalidRequest));
    CHECK(outcome.issues.size() >= 3);
}

TEST_CASE("Release plan detects policy and required capability drift", "[unit][application][release][preflight]") {
    const ReleasePreflightRequest request = Request();
    ReleasePreflightFacts facts = Facts(request);
    const ReleasePreflightOutcome outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());

    facts.policyDigest = Digest("changed-policy");
    facts.availableCapabilities.clear();
    facts.outputWritable = false;
    facts.availableCredentials.clear();
    const std::vector<ReleasePreflightIssue> changes = ValidateReleaseInputFreeze(*outcome.plan, facts);
    REQUIRE(changes.size() == 4);
    CHECK(changes[0].field == "outputAccess");
    CHECK(changes[1].field == "policy");
    CHECK(changes[2].field == "capabilities");
    CHECK(changes[3].field == "credentials");
}
