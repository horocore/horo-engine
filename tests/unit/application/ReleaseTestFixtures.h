#pragma once

#include "Horo/Release/ReleaseErrors.h"
#include "Horo/Release/ReleasePipelineExecutor.h"
#include "Horo/Release/ReleasePreflight.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <format>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace ReleaseTestFixtures {
    using namespace Horo;
    using namespace Horo::Release;

    [[nodiscard]] inline Sha256Digest Digest(const std::string_view text) {
        return ComputeSha256(std::as_bytes(std::span{text.data(), text.size()}));
    }

    [[nodiscard]] inline std::string Notes(const std::string_view version = "0.4.2", const std::string_view product = "horo-editor") {
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

    [[nodiscard]] inline EffectiveReleaseProfile Profile(const ReleaseSigningPolicy signing = ReleaseSigningPolicy::Disabled,
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

    [[nodiscard]] inline ReleasePreflightRequest Request() {
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

    [[nodiscard]] inline ReleasePreflightFacts Facts(const ReleasePreflightRequest &request) {
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

    [[nodiscard]] inline bool HasIssue(const ReleasePreflightOutcome &outcome, const ReleasePreflightIssueCode code) {
        return std::ranges::any_of(outcome.issues, [code](const ReleasePreflightIssue &issue) {
            return issue.code == code;
        });
    }

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

}  // namespace ReleaseTestFixtures
