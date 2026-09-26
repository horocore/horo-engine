#include "Horo/Release/ReleasePreflight.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <format>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {
    using namespace Horo;
    using namespace Horo::Release;

    [[nodiscard]] Sha256Digest Digest(const std::string_view text) {
        return ComputeSha256(std::as_bytes(std::span{text.data(), text.size()}));
    }

    [[nodiscard]] std::string Notes(const std::string_view version = "0.4.2", const std::string_view product = "horo-editor") {
        const std::string markdown = std::format("## [{}] — 2026-09-26\n\n### Added\n- Reviewed candidate behavior.\n", version);
        return nlohmann::json{{"schemaVersion", 1},
                              {"product", product},
                              {"version", version},
                              {"locale", "en-US"},
                              {"date", "2026-09-26"},
                              {"sections", nlohmann::json::array({{{"category", "Added"},
                                                                   {"items", nlohmann::json::array({"Reviewed candidate behavior."})}}})},
                              {"markdown", markdown}}
                   .dump() +
               '\n';
    }

    [[nodiscard]] EffectiveReleaseProfile Profile(const ReleaseSigningPolicy signing = ReleaseSigningPolicy::Disabled,
                                                  const bool allowPublication = false) {
        ReleaseProfilePreset preset;
        preset.id = {"shipping"};
        preset.product = DistributionProductIdentity{DistributionProductKind::Editor, {}};
        preset.artifactClass = DistributionArtifactClass::InstallableProduct;
        preset.platform = DistributionPlatform::Linux;
        preset.packageFormat =
            signing == ReleaseSigningPolicy::Required ? DistributionPackageFormat::LinuxAppImage : DistributionPackageFormat::TarGzip;
        preset.content = ReleaseContentPolicy{true, true, ReleaseAssetPolicy::SinglePackage, false, false};
        preset.symbols = ReleaseSymbolPolicy::Omit;
        preset.signing = signing;
        preset.notarizationRequired = false;
        preset.includeLicensesAndNotices = true;
        preset.includeReleaseNotes = true;
        preset.updateEligible = false;
        preset.patchEligible = false;
        preset.eligibleDestinations =
            allowPublication ? std::vector<ReleaseDestinationId>{{"github-releases"}} : std::vector<ReleaseDestinationId>{};
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
                .availableCredentials = {{42}},
                .releaseNotesSnapshot = Notes()};
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
    CHECK(outcome.plan->Identities().notes == Digest(facts.releaseNotesSnapshot));
    CHECK(outcome.plan->ReleaseNotesSnapshot() == facts.releaseNotesSnapshot);
    CHECK(outcome.plan->Summary().find("Credentials: 1 opaque handle(s)") != std::string::npos);
    CHECK(outcome.plan->Summary().find("Credentials: 42") == std::string::npos);
    CHECK(outcome.plan->Summary().find(FormatSha256(facts.sourceTreeDigest)) != std::string::npos);
    CHECK(outcome.plan->Summary().find(FormatSha256(facts.dependencyLockDigest)) != std::string::npos);

    const nlohmann::json snapshot = nlohmann::json::parse(outcome.plan->SerializeCanonical());
    CHECK(snapshot.at("version").at("value") == "0.4.2");
    CHECK(snapshot.at("target").at("configuration") == "shipping");
    CHECK(snapshot.at("credentialHandles") == nlohmann::json::array({42}));
    CHECK(snapshot.at("signingSelected") == false);
    CHECK(snapshot.at("publicationDestination").is_null());
    CHECK(snapshot.at("identities").at("dependencyLock") == FormatSha256(facts.dependencyLockDigest));
    CHECK(snapshot.at("releaseNotes").at("digest") == FormatSha256(Digest(facts.releaseNotesSnapshot)));
    CHECK(snapshot.at("releaseNotes").at("bytes") == facts.releaseNotesSnapshot);
    CHECK(nlohmann::json::parse(snapshot.at("releaseNotes").at("bytes").get<std::string>()).at("markdown") ==
          "## [0.4.2] — 2026-09-26\n\n### Added\n- Reviewed candidate behavior.\n");
    CHECK(outcome.plan->SerializeCanonical() == outcome.plan->SerializeCanonical());
    CHECK(ValidateReleaseInputFreeze(*outcome.plan, facts).empty());
}

TEST_CASE("Release preflight freezes signing and publication choices before submission", "[unit][application][release][preflight]") {
    ReleasePreflightRequest request = Request();
    request.profile = Profile(ReleaseSigningPolicy::Required, true);
    request.signingSelected = true;
    request.publicationDestination = ReleaseDestinationId{"github-releases"};
    const ReleasePreflightFacts facts = Facts(request);
    const ReleasePreflightOutcome accepted = PreflightRelease(request, facts);
    REQUIRE(accepted.plan.has_value());
    CHECK(accepted.plan->Request().signingSelected);
    CHECK(accepted.plan->Request().publicationDestination == request.publicationDestination);
    CHECK(accepted.plan->Summary().find("Publication: github-releases") != std::string::npos);
    const nlohmann::json snapshot = nlohmann::json::parse(accepted.plan->SerializeCanonical());
    CHECK(snapshot.at("signingSelected") == true);
    CHECK(snapshot.at("publicationDestination") == "github-releases");

    request.signingSelected = false;
    CHECK(HasIssue(PreflightRelease(request, facts), ReleasePreflightIssueCode::InvalidRequest));
    request.signingSelected = true;
    request.publicationDestination = ReleaseDestinationId{"unlisted"};
    CHECK(HasIssue(PreflightRelease(request, facts), ReleasePreflightIssueCode::InvalidRequest));
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

TEST_CASE("Release output cannot contain or be contained by the project source", "[unit][application][release][preflight]") {
    const ReleasePreflightRequest request = Request();
    for (const std::filesystem::path &unsafe : {request.projectRoot, request.projectRoot / "releases", request.projectRoot.parent_path()}) {
        ReleasePreflightFacts facts = Facts(request);
        facts.canonicalOutputRoot = unsafe;
        const ReleasePreflightOutcome outcome = PreflightRelease(request, facts);
        CHECK_FALSE(outcome.plan.has_value());
        CHECK(HasIssue(outcome, ReleasePreflightIssueCode::OutputUnavailable));
    }

    ReleasePreflightFacts sibling = Facts(request);
    sibling.canonicalOutputRoot = request.projectRoot.parent_path() / (request.projectRoot.filename().string() + "-releases");
    const ReleasePreflightOutcome accepted = PreflightRelease(request, sibling);
    CHECK(accepted.issues.empty());
    CHECK(accepted.plan.has_value());
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

TEST_CASE("Release notes failures are field-specific and cannot create a candidate", "[unit][application][release][preflight]") {
    const ReleasePreflightRequest request = Request();
    ReleasePreflightFacts facts = Facts(request);
    facts.releaseNotesSnapshot.clear();
    CHECK(HasIssue(PreflightRelease(request, facts), ReleasePreflightIssueCode::NotesMissing));
    facts.releaseNotesSnapshot = "{bad json";
    CHECK(HasIssue(PreflightRelease(request, facts), ReleasePreflightIssueCode::NotesMalformed));
    facts.releaseNotesSnapshot.assign(MaximumReleaseNotesSnapshotBytes + 1, 'x');
    CHECK(HasIssue(PreflightRelease(request, facts), ReleasePreflightIssueCode::NotesOversized));
    facts.releaseNotesSnapshot = Notes("0.4.2+another");
    CHECK(HasIssue(PreflightRelease(request, facts), ReleasePreflightIssueCode::NotesVersionMismatch));
    facts.releaseNotesSnapshot = Notes("0.4.2", "another-product");
    CHECK(HasIssue(PreflightRelease(request, facts), ReleasePreflightIssueCode::NotesProductMismatch));
    facts.releaseNotesSnapshot = Notes();
    auto malformed = nlohmann::json::parse(facts.releaseNotesSnapshot);
    malformed["markdown"] = "different text";
    facts.releaseNotesSnapshot = malformed.dump();
    CHECK(HasIssue(PreflightRelease(request, facts), ReleasePreflightIssueCode::NotesMalformed));
    for (const std::string_view unsafe : {"a < b", "a > b", "[bad](javascript:evil)"}) {
        malformed = nlohmann::json::parse(Notes());
        malformed["sections"][0]["items"][0] = unsafe;
        malformed["markdown"] = std::format("## [0.4.2] — 2026-09-26\n\n### Added\n- {}\n", unsafe);
        facts.releaseNotesSnapshot = malformed.dump();
        CHECK(HasIssue(PreflightRelease(request, facts), ReleasePreflightIssueCode::NotesMalformed));
    }
}

TEST_CASE("Release candidate retains reviewed notes and rejects later source changes", "[unit][application][release][preflight]") {
    const ReleasePreflightRequest request = Request();
    ReleasePreflightFacts facts = Facts(request);
    const ReleasePreflightOutcome accepted = PreflightRelease(request, facts);
    REQUIRE(accepted.plan.has_value());
    const std::string frozen = accepted.plan->ReleaseNotesSnapshot();
    facts.releaseNotesSnapshot = Notes("0.4.2", "horo-editor");
    auto changed = nlohmann::json::parse(facts.releaseNotesSnapshot);
    changed["sections"][0]["items"][0] = "Edited after candidate creation.";
    changed["markdown"] = "## [0.4.2] — 2026-09-26\n\n### Added\n- Edited after candidate creation.\n";
    facts.releaseNotesSnapshot = changed.dump() + '\n';
    CHECK(accepted.plan->ReleaseNotesSnapshot() == frozen);
    const auto issues = ValidateReleaseInputFreeze(*accepted.plan, facts);
    CHECK(std::ranges::any_of(issues, [](const ReleasePreflightIssue &issue) {
        return issue.code == ReleasePreflightIssueCode::NotesChanged && issue.field == "notes";
    }));
}
