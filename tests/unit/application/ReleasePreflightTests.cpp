#include "Horo/Release/ReleaseErrors.h"
#include "Horo/Release/ReleasePipelineExecutor.h"
#include "Horo/Release/ReleasePreflight.h"
#include "Horo/Release/ReleaseService.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <format>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <thread>

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

TEST_CASE("Release notes reject forged structure and excessive item counts", "[unit][application][release][preflight]") {
    const ReleasePreflightRequest request = Request();
    ReleasePreflightFacts facts = Facts(request);
    const nlohmann::json valid = nlohmann::json::parse(Notes());

    auto checkMalformed = [&](const nlohmann::json &notes) {
        facts.releaseNotesSnapshot = notes.dump();
        const ReleasePreflightOutcome outcome = PreflightRelease(request, facts);
        CHECK_FALSE(outcome.plan.has_value());
        CHECK(HasIssue(outcome, ReleasePreflightIssueCode::NotesMalformed));
    };

    auto malformed = valid;
    malformed["locale"] = "bad-locale";
    checkMalformed(malformed);
    malformed = valid;
    malformed["date"] = "2026-02-30";
    checkMalformed(malformed);
    malformed = valid;
    malformed["sections"][0]["category"] = "Unknown";
    checkMalformed(malformed);
    malformed = valid;
    malformed["sections"].push_back(malformed["sections"][0]);
    checkMalformed(malformed);
    malformed = valid;
    malformed["sections"][0]["items"] = nlohmann::json::array();
    checkMalformed(malformed);
    malformed = valid;
    malformed["sections"][0]["items"] = nlohmann::json::array();
    for (int index = 0; index < 65; ++index)
        malformed["sections"][0]["items"].push_back("Reviewed item.");
    checkMalformed(malformed);
}

namespace {
    class FixedReleaseFacts final : public IReleasePreflightFactsProvider {
    public:
        explicit FixedReleaseFacts(ReleasePreflightFacts facts) : facts_(std::move(facts)) {}

        [[nodiscard]] Result<ReleasePreflightFacts> Capture(const ReleaseExecutionPlan &) override {
            ++captures;
            ReleasePreflightFacts current = facts_;
            if (driftAt > 0 && captures >= driftAt)
                current.sourceTreeDigest = Digest("changed-source");
            return Result<ReleasePreflightFacts>::Success(std::move(current));
        }

        int captures{};
        int driftAt{};

    private:
        ReleasePreflightFacts facts_;
    };

    class RecordingReleaseStages final : public IReleasePipelineStages {
    public:
        std::optional<ReleaseStage> failAt;
        std::optional<ReleaseStage> throwAt;
        std::optional<ReleaseStage> cancelAt;
        const CancellationSource *cancellationSource{};
        std::vector<ReleaseStage> called;
        bool receivedSignedBytes{};
        bool publishedVerifiedCandidate{};
        bool reportObservations{};

        [[nodiscard]] Result<void> Validate(const ReleaseStageContext &context) override {
            return Run(context);
        }

        [[nodiscard]] Result<ReleaseConfiguredTarget> Configure(const ReleaseStageContext &context) override {
            return Run(context, ReleaseConfiguredTarget{"configured", Digest("configuration")});
        }

        [[nodiscard]] Result<ReleaseBuiltPayload> Build(const ReleaseStageContext &context,
                                                        const ReleaseConfiguredTarget &configured) override {
            CHECK(configured.root == "configured");
            return Run(context, ReleaseBuiltPayload{"built", Digest("built")});
        }

        [[nodiscard]] Result<ReleaseCookedPayload> Cook(const ReleaseStageContext &context, const ReleaseConfiguredTarget &configured,
                                                        const ReleaseBuiltPayload &built) override {
            CHECK(configured.root == "configured");
            CHECK(built.root == "built");
            return Run(context, ReleaseCookedPayload{"cooked", Digest("cooked")});
        }

        [[nodiscard]] Result<ReleaseStagedPayload> Package(const ReleaseStageContext &context, const ReleaseBuiltPayload &built,
                                                           const ReleaseCookedPayload &cooked) override {
            CHECK(built.root == "built");
            CHECK(cooked.root == "cooked");
            return Run(context, ReleaseStagedPayload{"staged", Digest("unsigned-bytes")});
        }

        [[nodiscard]] Result<void> PreSignVerify(const ReleaseStageContext &context, const ReleaseStagedPayload &staged) override {
            CHECK(staged.bytesDigest == Digest("unsigned-bytes"));
            return Run(context);
        }

        [[nodiscard]] Result<ReleaseSignedPayload> Sign(const ReleaseStageContext &context,
                                                        const ReleasePreSignVerifiedPayload &verified) override {
            CHECK(verified.staged.bytesDigest == Digest("unsigned-bytes"));
            return Run(context, ReleaseSignedPayload{"signed", Digest("signed-bytes")});
        }

        [[nodiscard]] Result<Sha256Digest> FinalizeMetadata(const ReleaseStageContext &context, const ReleaseCandidateId candidate,
                                                            const ReleaseFinalBytes &bytes) override {
            CHECK(candidate == ReleaseCandidateId{7});
            receivedSignedBytes = std::holds_alternative<ReleaseSignedPayload>(bytes) &&
                                  std::get<ReleaseSignedPayload>(bytes).bytesDigest == Digest("signed-bytes");
            return Run(context, Digest("final-manifest"));
        }

        [[nodiscard]] Result<void> FinalVerify(const ReleaseStageContext &context, const ReleaseFinalizedCandidate &candidate) override {
            CHECK(candidate.metadata.candidate == ReleaseCandidateId{7});
            CHECK(candidate.metadata.manifestDigest == Digest("final-manifest"));
            CHECK(std::get<ReleaseSignedPayload>(candidate.bytes).bytesDigest == Digest("signed-bytes"));
            return Run(context);
        }

        [[nodiscard]] Result<void> Publish(const ReleaseStageContext &context, const ReleaseFinalVerifiedCandidate &candidate) override {
            publishedVerifiedCandidate = candidate.finalized.metadata.candidate == ReleaseCandidateId{7};
            return Run(context);
        }

    private:
        [[nodiscard]] Result<void> Run(const ReleaseStageContext &context) {
            CHECK(context.job == ReleaseJobId{1});
            CHECK(context.target == ReleaseTargetId{2});
            CHECK(context.operation == 3);
            CHECK(context.attempt.value == called.size() + 1);
            called.push_back(context.stage);
            if (throwAt == context.stage)
                throw std::runtime_error{"stage failed"};
            if (reportObservations) {
                CHECK(context.ReportProgress(1, 2).HasValue());
                CHECK(context.ReportDiagnostic(ErrorCode{"release.test.stage"}, ErrorSeverity::Info, "Stage entered").HasValue());
            }
            if (cancelAt == context.stage && cancellationSource)
                cancellationSource->RequestCancellation();
            if (failAt == context.stage)
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            return Result<void>::Success();
        }

        template <typename T> [[nodiscard]] Result<T> Run(const ReleaseStageContext &context, T output) {
            const auto entered = Run(context);
            if (entered.HasError())
                return Result<T>::Failure(entered.ErrorValue());
            return Result<T>::Success(std::move(output));
        }
    };
}  // namespace

TEST_CASE("Release executor passes exact signed bytes into final metadata and publication", "[unit][application][release][executor]") {
    ReleasePreflightRequest request = Request();
    request.profile = Profile(ReleaseSigningPolicy::Required, true);
    request.signingSelected = true;
    request.publicationDestination = ReleaseDestinationId{"github-releases"};
    const ReleasePreflightFacts facts = Facts(request);
    const auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    RecordingReleaseStages stages;
    stages.reportObservations = true;
    ReleaseJobTracker tracker{{1}, {2}, 3, {true, true}};

    const auto result = ReleasePipelineExecutor{}.Execute(tracker, {7}, *outcome.plan, current, stages, {});
    CHECK(result.state == ReleaseJobState::Succeeded);
    REQUIRE(result.terminal.has_value());
    CHECK(std::holds_alternative<ReleaseSucceeded>(*result.terminal));
    CHECK(stages.receivedSignedBytes);
    CHECK(stages.publishedVerifiedCandidate);
    CHECK(current.captures == 10);
    CHECK(stages.called.size() == 10);
    CHECK(result.candidate->state == ReleaseCandidateState::FinalVerified);
    REQUIRE(result.progress.has_value());
    CHECK(result.progress->stage == ReleaseStage::Publishing);
    CHECK(result.progress->completed == 1);
    REQUIRE(result.recentDiagnostics.size() == 10);
    const auto lastDiagnostic = tracker.Diagnostic(result.recentDiagnostics.back());
    REQUIRE(lastDiagnostic.has_value());
    CHECK(lastDiagnostic->job == ReleaseJobId{1});
    CHECK(lastDiagnostic->target == ReleaseTargetId{2});
    CHECK(lastDiagnostic->operation == 3);
    CHECK(lastDiagnostic->stage == ReleaseStage::Publishing);
    CHECK(lastDiagnostic->attempt == ReleaseStageAttemptId{10});
}

TEST_CASE("Release executor stops at every failed stage and commits one terminal", "[unit][application][release][executor]") {
    ReleasePreflightRequest request = Request();
    request.profile = Profile(ReleaseSigningPolicy::Required, true);
    request.signingSelected = true;
    request.publicationDestination = ReleaseDestinationId{"github-releases"};
    const ReleasePreflightFacts facts = Facts(request);
    const auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    for (std::size_t index = 0; index < ReleaseStageCount; ++index) {
        INFO("Stage index: " << index);
        FixedReleaseFacts current{facts};
        RecordingReleaseStages stages;
        stages.failAt = static_cast<ReleaseStage>(index);
        ReleaseJobTracker tracker{{1}, {2}, 3, {true, true}};
        const auto result = ReleasePipelineExecutor{}.Execute(tracker, {7}, *outcome.plan, current, stages, {});
        CHECK(result.state == ReleaseJobState::Failed);
        REQUIRE(result.terminal.has_value());
        REQUIRE(std::holds_alternative<ReleaseFailed>(*result.terminal));
        CHECK(std::get<ReleaseFailed>(*result.terminal).stage == stages.failAt);
        CHECK(result.stages[index].state == ReleaseStageState::Failed);
        CHECK(stages.called.size() == index + 1);
        CHECK(current.captures == static_cast<int>(index + 1));
        CHECK(result.revision == index * 2 + 3);
    }
}

TEST_CASE("Release executor acknowledges cancellation after the active worker returns", "[unit][application][release][executor]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    const auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    CancellationSource cancellation;
    RecordingReleaseStages stages;
    stages.cancelAt = ReleaseStage::Building;
    stages.cancellationSource = &cancellation;
    ReleaseJobTracker tracker{{1}, {2}, 3, {false, false}};

    const auto result = ReleasePipelineExecutor{}.Execute(tracker, {7}, *outcome.plan, current, stages, cancellation.Token());
    CHECK(result.state == ReleaseJobState::Cancelled);
    REQUIRE(result.terminal.has_value());
    CHECK(std::holds_alternative<ReleaseCancelled>(*result.terminal));
    CHECK(result.stages[static_cast<std::size_t>(ReleaseStage::Building)].state == ReleaseStageState::Cancelled);
    CHECK(stages.called.size() == 3);
    CHECK_FALSE(tracker.RequestCancel().HasError());
    CHECK(tracker.Snapshot().revision == result.revision);
}

TEST_CASE("Release executor rejects source drift before invoking the next worker", "[unit][application][release][executor]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    const auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    current.driftAt = 2;
    RecordingReleaseStages stages;
    ReleaseJobTracker tracker{{1}, {2}, 3, {false, false}};

    const auto result = ReleasePipelineExecutor{}.Execute(tracker, {7}, *outcome.plan, current, stages, {});
    CHECK(result.state == ReleaseJobState::Failed);
    REQUIRE(result.terminal.has_value());
    const auto &failure = std::get<ReleaseFailed>(*result.terminal);
    CHECK(failure.stage == ReleaseStage::Configuring);
    CHECK(failure.cause.code.Value() == ReleaseErrors::PipelineInputChanged.code.Value());
    CHECK(stages.called.size() == 1);
}

TEST_CASE("Release executor converts a worker exception into one stage failure", "[unit][application][release][executor]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    const auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    RecordingReleaseStages stages;
    stages.throwAt = ReleaseStage::Cooking;
    ReleaseJobTracker tracker{{1}, {2}, 3, {false, false}};

    const auto result = ReleasePipelineExecutor{}.Execute(tracker, {7}, *outcome.plan, current, stages, {});
    CHECK(result.state == ReleaseJobState::Failed);
    REQUIRE(result.terminal.has_value());
    const auto &failure = std::get<ReleaseFailed>(*result.terminal);
    CHECK(failure.stage == ReleaseStage::Cooking);
    CHECK(failure.cause.code.Value() == ReleaseErrors::PipelineStageException.code.Value());
    CHECK(stages.called.size() == 4);
}

namespace {
    class ServiceStages final : public IReleasePipelineStages {
    public:
        ServiceStages(const bool block, std::shared_ptr<std::atomic<bool>> entered) : block_(block), entered_(std::move(entered)) {}

        [[nodiscard]] Result<void> Validate(const ReleaseStageContext &context) override {
            auto progress = context.ReportProgress(1, 2);
            if (progress.HasError())
                return progress;
            auto diagnostic = context.ReportDiagnostic(ErrorCode{"release.test.service"}, ErrorSeverity::Info, "Validation started");
            if (diagnostic.HasError())
                return Result<void>::Failure(diagnostic.ErrorValue());
            entered_->store(true);
            while (block_ && !context.cancellation.IsCancellationRequested())
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            if (context.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineProcessCancelled));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<ReleaseConfiguredTarget> Configure(const ReleaseStageContext &) override {
            return Result<ReleaseConfiguredTarget>::Success({"configured", Digest("configuration")});
        }

        [[nodiscard]] Result<ReleaseBuiltPayload> Build(const ReleaseStageContext &, const ReleaseConfiguredTarget &) override {
            return Result<ReleaseBuiltPayload>::Success({"built", Digest("built")});
        }

        [[nodiscard]] Result<ReleaseCookedPayload> Cook(const ReleaseStageContext &, const ReleaseConfiguredTarget &,
                                                        const ReleaseBuiltPayload &) override {
            return Result<ReleaseCookedPayload>::Success({"cooked", Digest("cooked")});
        }

        [[nodiscard]] Result<ReleaseStagedPayload> Package(const ReleaseStageContext &, const ReleaseBuiltPayload &,
                                                           const ReleaseCookedPayload &) override {
            return Result<ReleaseStagedPayload>::Success({"staged", Digest("staged")});
        }

        [[nodiscard]] Result<void> PreSignVerify(const ReleaseStageContext &, const ReleaseStagedPayload &) override {
            return Result<void>::Success();
        }

        [[nodiscard]] Result<ReleaseSignedPayload> Sign(const ReleaseStageContext &, const ReleasePreSignVerifiedPayload &) override {
            return Result<ReleaseSignedPayload>::Success({"signed", Digest("signed")});
        }

        [[nodiscard]] Result<Sha256Digest> FinalizeMetadata(const ReleaseStageContext &, ReleaseCandidateId,
                                                            const ReleaseFinalBytes &) override {
            return Result<Sha256Digest>::Success(Digest("manifest"));
        }

        [[nodiscard]] Result<void> FinalVerify(const ReleaseStageContext &, const ReleaseFinalizedCandidate &) override {
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> Publish(const ReleaseStageContext &, const ReleaseFinalVerifiedCandidate &) override {
            return Result<void>::Success();
        }

    private:
        bool block_{};
        std::shared_ptr<std::atomic<bool>> entered_;
    };

    class ServiceWorkerFactory final : public IReleaseWorkerFactory {
    public:
        bool block{};
        bool throwOnCreate{};
        std::shared_ptr<std::atomic<bool>> entered = std::make_shared<std::atomic<bool>>(false);

        [[nodiscard]] Result<std::unique_ptr<IReleasePipelineStages>> Create(const ReleaseExecutionPlan &) override {
            if (throwOnCreate)
                throw std::runtime_error{"factory failure"};
            return Result<std::unique_ptr<IReleasePipelineStages>>::Success(std::make_unique<ServiceStages>(block, entered));
        }
    };

    [[nodiscard]] std::optional<ReleaseJobSnapshot> WaitForTerminal(ReleaseService &service, const ReleaseJobId job) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            auto snapshot = service.Query(job);
            if (snapshot && snapshot->terminal)
                return snapshot;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return service.Query(job);
    }
}  // namespace

TEST_CASE("Release service retains a completed job after its submitting scope ends", "[unit][application][release][service]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    FixedReleaseFacts current{facts};
    ServiceWorkerFactory factory;
    OperationStore operations{4, 4};
    ReleaseService service{operations, current, factory};

    ReleaseSubmission submitted;
    {
        auto outcome = PreflightRelease(request, facts);
        REQUIRE(outcome.plan.has_value());
        auto result = service.Submit(std::move(*outcome.plan));
        REQUIRE(result.HasValue());
        submitted = result.Value();
    }
    const auto snapshot = WaitForTerminal(service, submitted.job);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->terminal.has_value());
    CHECK(snapshot->state == ReleaseJobState::Succeeded);
    CHECK(snapshot->target == submitted.target);
    CHECK(snapshot->operation == submitted.operation);
    CHECK(service.List().size() == 1);
    const auto projected = operations.SnapshotIfChanged(0);
    REQUIRE(projected.has_value());
    REQUIRE(projected->operations.size() == 1);
    CHECK(projected->operations.front().state == OperationState::Succeeded);
    service.Shutdown();
}

TEST_CASE("Release service bounds admission and cancels an active worker", "[unit][application][release][service]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    ServiceWorkerFactory factory;
    factory.block = true;
    OperationStore operations{4, 4};
    ReleaseServiceConfig config;
    config.activeCapacity = 1;
    ReleaseService service{operations, current, factory, config};

    const auto first = service.Submit(*outcome.plan);
    REQUIRE(first.HasValue());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!factory.entered->load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    REQUIRE(factory.entered->load());
    const auto running = service.Query(first.Value().job);
    REQUIRE(running.has_value());
    REQUIRE(running->progress.has_value());
    CHECK(running->progress->stage == ReleaseStage::Validating);
    REQUIRE(running->recentDiagnostics.size() == 1);
    const auto diagnostic = service.Diagnostic(first.Value().job, running->recentDiagnostics.front());
    REQUIRE(diagnostic.has_value());
    CHECK(diagnostic->operation == first.Value().operation);
    CHECK(service.Submit(*outcome.plan).HasError());
    REQUIRE(service.RequestCancel(first.Value().job).HasValue());
    const auto cancelled = WaitForTerminal(service, first.Value().job);
    REQUIRE(cancelled.has_value());
    CHECK(cancelled->state == ReleaseJobState::Cancelled);
    REQUIRE(cancelled->terminal.has_value());
    CHECK(std::holds_alternative<ReleaseCancelled>(*cancelled->terminal));
    CHECK(service.List().size() == 1);
    auto replacement = service.Submit(*outcome.plan);
    REQUIRE(replacement.HasValue());
    REQUIRE(operations.RequestCancel(replacement.Value().operation));
    const auto replaced = WaitForTerminal(service, replacement.Value().job);
    REQUIRE(replaced.has_value());
    CHECK(replaced->state == ReleaseJobState::Cancelled);
    service.Shutdown();
}

TEST_CASE("Release service terminalizes an unexpected worker-factory exception", "[unit][application][release][service]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    ServiceWorkerFactory factory;
    factory.throwOnCreate = true;
    OperationStore operations{4, 4};
    ReleaseService service{operations, current, factory};

    const auto submitted = service.Submit(*outcome.plan);
    REQUIRE(submitted.HasValue());
    const auto failed = WaitForTerminal(service, submitted.Value().job);
    REQUIRE(failed.has_value());
    CHECK(failed->state == ReleaseJobState::Failed);
    REQUIRE(failed->terminal.has_value());
    CHECK(std::holds_alternative<ReleaseFailed>(*failed->terminal));
    service.Shutdown();
}

TEST_CASE("Release service assigns distinct candidate identities and bounds recent history", "[unit][application][release][service]") {
    const ReleasePreflightRequest request = Request();
    const ReleasePreflightFacts facts = Facts(request);
    auto outcome = PreflightRelease(request, facts);
    REQUIRE(outcome.plan.has_value());
    FixedReleaseFacts current{facts};
    ServiceWorkerFactory factory;
    OperationStore operations{4, 4};
    ReleaseServiceConfig config;
    config.recentCapacity = 1;
    ReleaseService service{operations, current, factory, config};

    const auto first = service.Submit(*outcome.plan);
    REQUIRE(first.HasValue());
    const auto firstTerminal = WaitForTerminal(service, first.Value().job);
    REQUIRE(firstTerminal.has_value());
    REQUIRE(firstTerminal->candidate.has_value());

    const auto second = service.Submit(*outcome.plan);
    REQUIRE(second.HasValue());
    const auto secondTerminal = WaitForTerminal(service, second.Value().job);
    REQUIRE(secondTerminal.has_value());
    REQUIRE(secondTerminal->candidate.has_value());
    CHECK(firstTerminal->candidate->id != secondTerminal->candidate->id);
    CHECK_FALSE(service.Query(first.Value().job).has_value());
    CHECK(service.Query(second.Value().job).has_value());
    CHECK(service.List().size() == 1);
    service.Shutdown();
}
