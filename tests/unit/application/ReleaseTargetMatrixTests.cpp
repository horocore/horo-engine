#include "Horo/Release/ReleaseTargetMatrix.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <format>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {
    using namespace Horo;
    using namespace Horo::Release;

    [[nodiscard]] Sha256Digest Digest(const std::string_view text) {
        return ComputeSha256(std::as_bytes(std::span{text.data(), text.size()}));
    }

    [[nodiscard]] std::string Notes(const std::string_view version = "1.2.3") {
        const std::string markdown = std::format("## [{}] — 2026-09-26\n\n### Added\n- Reviewed matrix behavior.\n", version);
        return nlohmann::json{{"schemaVersion", 1},
                              {"product", "horo-editor"},
                              {"version", version},
                              {"locale", "en-US"},
                              {"date", "2026-09-26"},
                              {"sections", nlohmann::json::array(
                                               {{{"category", "Added"}, {"items", nlohmann::json::array({"Reviewed matrix behavior."})}}})},
                              {"markdown", markdown}}
                   .dump() +
               '\n';
    }

    [[nodiscard]] EffectiveReleaseProfile Profile(const DistributionPlatform platform, const DistributionPackageFormat format) {
        const auto formatCapabilities = DescribeDistributionPackageFormat(format, platform);
        REQUIRE(formatCapabilities.HasValue());
        ReleaseProfilePreset preset;
        preset.id = {"matrix-shipping"};
        preset.product = DistributionProductIdentity{DistributionProductKind::Editor, {}};
        preset.artifactClass = DistributionArtifactClass::InstallableProduct;
        preset.platform = platform;
        preset.packageFormat = format;
        preset.content = ReleaseContentPolicy{true, true, ReleaseAssetPolicy::SinglePackage, false, false};
        preset.symbols = ReleaseSymbolPolicy::Omit;
        preset.signing = formatCapabilities.Value().signing == DistributionCapability::Required ? ReleaseSigningPolicy::Required
                                                                                                : ReleaseSigningPolicy::Disabled;
        preset.notarizationRequired = false;
        preset.includeLicensesAndNotices = true;
        preset.includeReleaseNotes = true;
        preset.updateEligible = false;
        preset.patchEligible = false;
        preset.eligibleDestinations = std::vector<ReleaseDestinationId>{};
        preset.requiredCapabilities = std::vector<ReleaseCapabilityId>{};
        auto catalog = ReleaseProfileCatalog::Create({std::move(preset)});
        REQUIRE(catalog.HasValue());
        auto profile = catalog.Value().Resolve({"matrix-shipping"}, {});
        REQUIRE(profile.HasValue());
        return std::move(profile).Value();
    }

    [[nodiscard]] ReleaseMatrixCellRequest Cell(const std::string_view id, const DistributionPlatform platform,
                                                const DistributionArchitecture architecture, const DistributionPackageFormat format,
                                                const ReleaseMatrixRequirement requirement = ReleaseMatrixRequirement::Required) {
        auto version = ParseReleaseVersion("1.2.3");
        REQUIRE(version.HasValue());
        const std::filesystem::path root = std::filesystem::temp_directory_path();
        ReleasePreflightRequest request{.projectRoot = root / "horo-matrix-project",
                                        .projectId = "horo-editor",
                                        .version = {EngineProductVersion{version.Value()}, ReleaseSourceRevision{"source-123"}},
                                        .profile = Profile(platform, format),
                                        .architecture = architecture,
                                        .configuration = ReleaseBuildConfiguration::Shipping,
                                        .toolchainId = "toolchain-1",
                                        .outputRoot = root / ("horo-matrix-output-" + std::string{id}),
                                        .requiredFreeBytes = 1024,
                                        .credentials = {{42}},
                                        .signingSelected = false};
        request.signingSelected = request.profile.Signing() == ReleaseSigningPolicy::Required;
        ReleasePreflightFacts facts{.requestedProjectRoot = request.projectRoot,
                                    .requestedOutputRoot = request.outputRoot,
                                    .canonicalProjectRoot = request.projectRoot,
                                    .canonicalOutputRoot = request.outputRoot,
                                    .projectReadable = true,
                                    .outputWritable = true,
                                    .availableBytes = 2048,
                                    .hostPlatform = platform,
                                    .targetSupported = true,
                                    .toolchainAvailable = true,
                                    .currentVersion = request.version,
                                    .sourceTreeDigest = Digest("source"),
                                    .dependencyLockDigest = Digest("lock"),
                                    .profileDigest = Digest(request.profile.SerializeCanonical()),
                                    .toolchainDigest = Digest("toolchain"),
                                    .policyDigest = Digest("policy"),
                                    .availableCredentials = {{42}},
                                    .releaseNotesSnapshot = Notes()};
        return {"job-" + std::string{id}, std::string{id}, requirement, std::move(request), std::move(facts), {14, 0, 0}, "sdk-1"};
    }

    [[nodiscard]] ReleaseToolchainDescriptor Toolchain(const ReleaseMachine host, const ReleaseMachine target,
                                                       const DistributionPackageFormat format) {
        return {.id = "toolchain-1",
                .digest = Digest("toolchain"),
                .host = host,
                .target = target,
                .oldestSupportedPlatform = {10, 0, 0},
                .newestSupportedPlatform = {20, 0, 0},
                .sdk = {"sdk-1", target.platform, {15, 0, 0}, {10, 0, 0}, {20, 0, 0}, true},
                .packageFormats = {format},
                .enabled = true,
                .compileAndLinkValidated = true,
                .explicitCrossToolchain = host != target};
    }

    [[nodiscard]] bool HasIssue(const ReleaseMatrixCellPlan &cell, const ReleaseTargetIssueCode code) {
        return std::ranges::any_of(cell.issues, [code](const ReleaseTargetIssue &issue) {
            return issue.code == code;
        });
    }
}  // namespace

TEST_CASE("Matrix fixture freezes exact release notes and rejects a mismatched version", "[unit][application][release][matrix]") {
    const ReleaseMachine host{DistributionPlatform::Linux, DistributionArchitecture::X64};
    ReleaseMatrixCellRequest cell = Cell("notes", host.platform, host.architecture, DistributionPackageFormat::TarGzip);
    const ReleaseToolchainDescriptor toolchain = Toolchain(host, host, DistributionPackageFormat::TarGzip);
    auto matrix = PlanReleaseTargetMatrix("release-notes", host, std::span{&cell, 1U}, std::span{&toolchain, 1U});
    REQUIRE(matrix.cells.size() == 1);
    REQUIRE(matrix.cells[0].issues.empty());
    REQUIRE(matrix.cells[0].plan.has_value());
    CHECK(matrix.cells[0].plan->ReleaseNotesSnapshot() == cell.facts.releaseNotesSnapshot);
    CHECK(matrix.cells[0].plan->Identities().notes == Digest(cell.facts.releaseNotesSnapshot));

    cell.facts.releaseNotesSnapshot = Notes("1.2.4");
    matrix = PlanReleaseTargetMatrix("release-notes", host, std::span{&cell, 1U}, std::span{&toolchain, 1U});
    REQUIRE(matrix.cells.size() == 1);
    CHECK_FALSE(matrix.cells[0].plan.has_value());
    CHECK(std::ranges::any_of(matrix.cells[0].issues, [](const ReleaseTargetIssue &issue) {
        return issue.code == ReleaseTargetIssueCode::PreflightFailed && issue.field == "notes.version";
    }));
}

TEST_CASE("Native Windows, Linux and macOS targets produce independent frozen release plans", "[unit][application][release][matrix]") {
    for (const ReleaseMachine machine : {ReleaseMachine{DistributionPlatform::Windows, DistributionArchitecture::X64},
                                         ReleaseMachine{DistributionPlatform::Linux, DistributionArchitecture::X64},
                                         ReleaseMachine{DistributionPlatform::MacOS, DistributionArchitecture::Arm64}}) {
        const DistributionPackageFormat format = machine.platform == DistributionPlatform::Windows ? DistributionPackageFormat::WindowsMsi
                                                 : machine.platform == DistributionPlatform::Linux ? DistributionPackageFormat::TarGzip
                                                                                                   : DistributionPackageFormat::MacPkg;
        const ReleaseMatrixCellRequest cell = Cell("native", machine.platform, machine.architecture, format);
        const ReleaseToolchainDescriptor toolchain = Toolchain(machine, machine, format);
        const ReleaseTargetMatrixPlan matrix =
            PlanReleaseTargetMatrix("release-1", machine, std::span{&cell, 1U}, std::span{&toolchain, 1U});
        REQUIRE(matrix.issues.empty());
        REQUIRE(matrix.cells.size() == 1);
        CHECK(matrix.cells[0].issues.empty());
        REQUIRE(matrix.cells[0].plan.has_value());
        REQUIRE(matrix.cells[0].validatedTarget.has_value());
        CHECK(matrix.cells[0].plan->Request().architecture == machine.architecture);
        CHECK(matrix.cells[0].validatedTarget->packageFormat == format);
    }
}

TEST_CASE("Explicit compatible macOS to Windows cross-toolchain is admitted", "[unit][application][release][matrix]") {
    const ReleaseMachine host{DistributionPlatform::MacOS, DistributionArchitecture::Arm64};
    const ReleaseMachine target{DistributionPlatform::Windows, DistributionArchitecture::X64};
    ReleaseMatrixCellRequest cell = Cell("windows", target.platform, target.architecture, DistributionPackageFormat::WindowsMsi);
    cell.facts.hostPlatform = host.platform;
    cell.facts.crossCompilerAvailable = true;
    const ReleaseToolchainDescriptor toolchain = Toolchain(host, target, DistributionPackageFormat::WindowsMsi);
    const auto matrix = PlanReleaseTargetMatrix("release-1", host, std::span{&cell, 1U}, std::span{&toolchain, 1U});
    REQUIRE(matrix.cells.size() == 1);
    CHECK(matrix.cells[0].issues.empty());
    CHECK(matrix.cells[0].plan.has_value());
}

TEST_CASE("Windows macOS and Linux cells retain distinct jobs and complete terminal outcomes", "[unit][application][release][matrix]") {
    const ReleaseMachine host{DistributionPlatform::MacOS, DistributionArchitecture::Arm64};
    std::vector<ReleaseMatrixCellRequest> cells;
    cells.push_back(Cell("windows", DistributionPlatform::Windows, DistributionArchitecture::X64, DistributionPackageFormat::ZipArchive));
    cells.push_back(Cell("linux", DistributionPlatform::Linux, DistributionArchitecture::X64, DistributionPackageFormat::TarGzip));
    cells.push_back(Cell("macos", DistributionPlatform::MacOS, DistributionArchitecture::Arm64, DistributionPackageFormat::MacPkg));
    std::vector<ReleaseToolchainDescriptor> toolchains;
    for (std::size_t index = 0; index < cells.size(); ++index) {
        ReleaseMatrixCellRequest &cell = cells[index];
        cell.facts.hostPlatform = host.platform;
        cell.facts.crossCompilerAvailable = index != 2;
        ReleaseToolchainDescriptor toolchain =
            Toolchain(host, {cell.request.profile.Platform(), cell.request.architecture}, cell.request.profile.PackageFormat());
        toolchain.id = "toolchain-" + cell.targetId;
        toolchain.digest = Digest(toolchain.id);
        cell.request.toolchainId = toolchain.id;
        cell.facts.toolchainDigest = toolchain.digest;
        toolchains.push_back(std::move(toolchain));
    }

    const ReleaseTargetMatrixPlan matrix = PlanReleaseTargetMatrix("release-1", host, cells, toolchains);
    REQUIRE(matrix.issues.empty());
    REQUIRE(matrix.cells.size() == 3);
    for (const ReleaseMatrixCellPlan &cell : matrix.cells) {
        INFO(cell.targetId);
        CHECK(cell.issues.empty());
        CHECK(cell.plan.has_value());
        CHECK(cell.validatedTarget.has_value());
    }
    const std::vector<ReleaseTargetTerminal> terminals{{"release-1", "job-windows", "windows", ReleaseTargetTerminalState::Succeeded, true},
                                                       {"release-1", "job-linux", "linux", ReleaseTargetTerminalState::Failed, false},
                                                       {"release-1", "job-macos", "macos", ReleaseTargetTerminalState::Succeeded, true}};
    const ReleaseMatrixSummary failed = SummarizeReleaseTargetMatrix(matrix, terminals);
    CHECK(failed.state == ReleaseMatrixState::Failed);
    REQUIRE(failed.members.size() == 3);
    CHECK(failed.members[0].jobId == "job-windows");
    CHECK(failed.members[1].state == ReleaseMatrixMemberState::Failed);
    CHECK(failed.members[2].jobId == "job-macos");

    auto wrongJob = terminals;
    wrongJob[1].jobId = "another-job";
    const ReleaseMatrixSummary mismatched = SummarizeReleaseTargetMatrix(matrix, wrongJob);
    CHECK(mismatched.state == ReleaseMatrixState::Failed);
    CHECK(mismatched.members[1].state == ReleaseMatrixMemberState::Pending);
    CHECK_FALSE(mismatched.issues.empty());
}

TEST_CASE("Unsupported host, architecture and implicit cross-toolchains fail before execution", "[unit][application][release][matrix]") {
    const ReleaseMachine host{DistributionPlatform::Linux, DistributionArchitecture::X64};
    const ReleaseMachine mac{DistributionPlatform::MacOS, DistributionArchitecture::Arm64};
    ReleaseMatrixCellRequest cell = Cell("mac", mac.platform, mac.architecture, DistributionPackageFormat::MacPkg);
    cell.facts.hostPlatform = host.platform;
    cell.facts.crossCompilerAvailable = true;
    ReleaseToolchainDescriptor toolchain = Toolchain(host, mac, DistributionPackageFormat::MacPkg);
    toolchain.explicitCrossToolchain = false;
    const auto matrix = PlanReleaseTargetMatrix("release-1", host, std::span{&cell, 1U}, std::span{&toolchain, 1U});
    REQUIRE(matrix.cells.size() == 1);
    CHECK_FALSE(matrix.cells[0].plan.has_value());
    CHECK(HasIssue(matrix.cells[0], ReleaseTargetIssueCode::UnsupportedHostTarget));
    CHECK(HasIssue(matrix.cells[0], ReleaseTargetIssueCode::CrossToolchainRequired));

    const ReleaseMachine windowsArm{DistributionPlatform::Windows, DistributionArchitecture::Arm64};
    ReleaseMatrixCellRequest unsupported =
        Cell("win-arm", windowsArm.platform, windowsArm.architecture, DistributionPackageFormat::WindowsMsi);
    const ReleaseToolchainDescriptor armToolchain = Toolchain(host, windowsArm, DistributionPackageFormat::WindowsMsi);
    const auto armMatrix = PlanReleaseTargetMatrix("release-2", host, std::span{&unsupported, 1U}, std::span{&armToolchain, 1U});
    CHECK(HasIssue(armMatrix.cells[0], ReleaseTargetIssueCode::UnsupportedArchitecture));
}

TEST_CASE("Missing SDK, minimum platform and package capability are reported independently", "[unit][application][release][matrix]") {
    const ReleaseMachine host{DistributionPlatform::Linux, DistributionArchitecture::X64};
    ReleaseMatrixCellRequest cell = Cell("linux", host.platform, host.architecture, DistributionPackageFormat::LinuxDeb);
    cell.minimumPlatform = {21, 0, 0};
    ReleaseToolchainDescriptor toolchain = Toolchain(host, host, DistributionPackageFormat::TarGzip);
    toolchain.sdk.available = false;
    const auto matrix = PlanReleaseTargetMatrix("release-1", host, std::span{&cell, 1U}, std::span{&toolchain, 1U});
    REQUIRE(matrix.cells.size() == 1);
    CHECK_FALSE(matrix.cells[0].plan.has_value());
    CHECK(HasIssue(matrix.cells[0], ReleaseTargetIssueCode::UnsupportedPlatformVersion));
    CHECK(HasIssue(matrix.cells[0], ReleaseTargetIssueCode::SdkUnavailable));
    CHECK(HasIssue(matrix.cells[0], ReleaseTargetIssueCode::PackageFormatUnsupported));
}

TEST_CASE("Mixed matrix results preserve every member and block failed required targets", "[unit][application][release][matrix]") {
    const ReleaseMachine host{DistributionPlatform::Linux, DistributionArchitecture::X64};
    std::vector<ReleaseMatrixCellRequest> cells;
    cells.push_back(Cell("required-ok", host.platform, host.architecture, DistributionPackageFormat::TarGzip));
    cells.push_back(Cell("required-fail", host.platform, host.architecture, DistributionPackageFormat::TarGzip));
    cells.push_back(
        Cell("optional-fail", host.platform, host.architecture, DistributionPackageFormat::TarGzip, ReleaseMatrixRequirement::Optional));
    cells[1].request.configuration = ReleaseBuildConfiguration::Development;
    cells[2].request.configuration = ReleaseBuildConfiguration::Debug;
    const ReleaseToolchainDescriptor toolchain = Toolchain(host, host, DistributionPackageFormat::TarGzip);
    const auto matrix = PlanReleaseTargetMatrix("release-1", host, cells, std::span{&toolchain, 1U});
    REQUIRE(matrix.cells.size() == 3);
    REQUIRE(std::ranges::all_of(matrix.cells, [](const ReleaseMatrixCellPlan &cell) {
        return cell.plan.has_value();
    }));

    const std::vector<ReleaseTargetTerminal> partial{
        {"release-1", "job-required-ok", "required-ok", ReleaseTargetTerminalState::Succeeded, true}};
    CHECK(SummarizeReleaseTargetMatrix(matrix, partial).state == ReleaseMatrixState::Incomplete);
    const std::vector<ReleaseTargetTerminal> mixed{{"release-1", "job-required-ok", "required-ok", ReleaseTargetTerminalState::Succeeded,
                                                    true},
                                                   {"release-1", "job-required-fail", "required-fail", ReleaseTargetTerminalState::Failed,
                                                    false},
                                                   {"release-1", "job-optional-fail", "optional-fail",
                                                    ReleaseTargetTerminalState::Cancelled, false}};
    const auto failed = SummarizeReleaseTargetMatrix(matrix, mixed);
    CHECK(failed.state == ReleaseMatrixState::Failed);
    REQUIRE(failed.members.size() == 3);
    CHECK(failed.members[1].terminal->state == ReleaseTargetTerminalState::Failed);
    CHECK(failed.members[2].terminal->state == ReleaseTargetTerminalState::Cancelled);
    CHECK(failed.members[1].state == ReleaseMatrixMemberState::Failed);
    CHECK(failed.members[2].state == ReleaseMatrixMemberState::Cancelled);

    auto succeeded = mixed;
    succeeded[1] = {"release-1", "job-required-fail", "required-fail", ReleaseTargetTerminalState::Succeeded, true};
    CHECK(SummarizeReleaseTargetMatrix(matrix, succeeded).state == ReleaseMatrixState::Succeeded);
    succeeded[1].candidateFinalVerified = false;
    CHECK(SummarizeReleaseTargetMatrix(matrix, succeeded).state == ReleaseMatrixState::Failed);
}

TEST_CASE("Duplicate cells and terminal results cannot produce a successful group", "[unit][application][release][matrix]") {
    const ReleaseMachine host{DistributionPlatform::Linux, DistributionArchitecture::X64};
    std::vector<ReleaseMatrixCellRequest> cells;
    cells.push_back(Cell("same", host.platform, host.architecture, DistributionPackageFormat::TarGzip));
    cells.push_back(Cell("same", host.platform, host.architecture, DistributionPackageFormat::TarGzip));
    const ReleaseToolchainDescriptor toolchain = Toolchain(host, host, DistributionPackageFormat::TarGzip);
    const auto matrix = PlanReleaseTargetMatrix("release-1", host, cells, std::span{&toolchain, 1U});
    CHECK(HasIssue(matrix.cells[1], ReleaseTargetIssueCode::InvalidTarget));
    CHECK(SummarizeReleaseTargetMatrix(matrix, {}).state == ReleaseMatrixState::Failed);

    const ReleaseMatrixCellRequest single = Cell("one", host.platform, host.architecture, DistributionPackageFormat::TarGzip);
    const auto valid = PlanReleaseTargetMatrix("release-2", host, std::span{&single, 1U}, std::span{&toolchain, 1U});
    const std::vector<ReleaseTargetTerminal> duplicate{{"release-2", "job-one", "one", ReleaseTargetTerminalState::Succeeded, true},
                                                       {"release-2", "job-one", "one", ReleaseTargetTerminalState::Succeeded, true}};
    CHECK(SummarizeReleaseTargetMatrix(valid, duplicate).state == ReleaseMatrixState::Failed);
}

TEST_CASE("Invalid, foreign and rejected terminal evidence cannot produce a successful group", "[unit][application][release][matrix]") {
    const ReleaseMachine host{DistributionPlatform::Linux, DistributionArchitecture::X64};
    const ReleaseMatrixCellRequest cell = Cell("one", host.platform, host.architecture, DistributionPackageFormat::TarGzip);
    const ReleaseToolchainDescriptor toolchain = Toolchain(host, host, DistributionPackageFormat::TarGzip);
    const ReleaseTargetMatrixPlan valid = PlanReleaseTargetMatrix("release-1", host, std::span{&cell, 1U}, std::span{&toolchain, 1U});
    REQUIRE(valid.cells.size() == 1);
    REQUIRE(valid.cells[0].plan.has_value());

    const ReleaseTargetTerminal invalid{"release-1", "job-one", "one", static_cast<ReleaseTargetTerminalState>(255), false};
    const ReleaseMatrixSummary invalidSummary = SummarizeReleaseTargetMatrix(valid, std::span{&invalid, 1U});
    CHECK(invalidSummary.state == ReleaseMatrixState::Failed);
    REQUIRE(invalidSummary.members.size() == 1);
    CHECK(invalidSummary.members[0].terminal.has_value());
    CHECK(invalidSummary.members[0].state == ReleaseMatrixMemberState::Pending);

    const std::vector<ReleaseTargetTerminal> foreign{{"release-1", "job-one", "one", ReleaseTargetTerminalState::Succeeded, true},
                                                     {"release-1", "job-foreign", "one", ReleaseTargetTerminalState::Succeeded, true}};
    CHECK(SummarizeReleaseTargetMatrix(valid, foreign).state == ReleaseMatrixState::Failed);

    ReleaseMatrixCellRequest rejectedCell = cell;
    rejectedCell.sdkId = "missing-sdk";
    const ReleaseTargetMatrixPlan rejected =
        PlanReleaseTargetMatrix("release-2", host, std::span{&rejectedCell, 1U}, std::span{&toolchain, 1U});
    REQUIRE_FALSE(rejected.cells[0].plan.has_value());
    const ReleaseTargetTerminal claimed{"release-2", "job-one", "one", ReleaseTargetTerminalState::Succeeded, true};
    const ReleaseMatrixSummary rejectedSummary = SummarizeReleaseTargetMatrix(rejected, std::span{&claimed, 1U});
    CHECK(rejectedSummary.state == ReleaseMatrixState::Failed);
    CHECK(rejectedSummary.members[0].state == ReleaseMatrixMemberState::ValidationFailed);
}

TEST_CASE("A terminal from another release group cannot satisfy the same job and target labels", "[unit][application][release][matrix]") {
    const ReleaseMachine host{DistributionPlatform::Linux, DistributionArchitecture::X64};
    const ReleaseMatrixCellRequest cell = Cell("one", host.platform, host.architecture, DistributionPackageFormat::TarGzip);
    const ReleaseToolchainDescriptor toolchain = Toolchain(host, host, DistributionPackageFormat::TarGzip);
    const ReleaseTargetMatrixPlan groupA = PlanReleaseTargetMatrix("release-a", host, std::span{&cell, 1U}, std::span{&toolchain, 1U});
    const ReleaseTargetMatrixPlan groupB = PlanReleaseTargetMatrix("release-b", host, std::span{&cell, 1U}, std::span{&toolchain, 1U});
    REQUIRE(groupA.cells[0].plan.has_value());
    REQUIRE(groupB.cells[0].plan.has_value());

    const ReleaseTargetTerminal terminalB{"release-b", "job-one", "one", ReleaseTargetTerminalState::Succeeded, true};
    CHECK(SummarizeReleaseTargetMatrix(groupB, std::span{&terminalB, 1U}).state == ReleaseMatrixState::Succeeded);
    const ReleaseMatrixSummary wrongGroup = SummarizeReleaseTargetMatrix(groupA, std::span{&terminalB, 1U});
    CHECK(wrongGroup.state == ReleaseMatrixState::Failed);
    REQUIRE(wrongGroup.members.size() == 1);
    CHECK(wrongGroup.members[0].state == ReleaseMatrixMemberState::Pending);
    CHECK_FALSE(wrongGroup.members[0].terminal.has_value());
    CHECK_FALSE(wrongGroup.issues.empty());

    const ReleaseTargetTerminal terminalA{"release-a", "job-one", "one", ReleaseTargetTerminalState::Succeeded, true};
    const std::vector<ReleaseTargetTerminal> mixed{terminalA, terminalB};
    CHECK(SummarizeReleaseTargetMatrix(groupA, mixed).state == ReleaseMatrixState::Failed);
}
