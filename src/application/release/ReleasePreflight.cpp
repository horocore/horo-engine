#include "Horo/Release/ReleasePreflight.h"

#include "Horo/Release/DistributionModel.h"

#include <algorithm>
#include <format>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

namespace Horo::Release {
    namespace {
        constexpr std::size_t MaximumObservedCapabilities = 256;
        constexpr std::size_t MaximumObservedCredentials = 256;

        /** @brief Reserves the zero digest as an absent observation sentinel. */
        [[nodiscard]] bool IsEmptyDigest(const Sha256Digest &digest) noexcept {
            return std::ranges::all_of(digest.bytes, [](const std::uint8_t byte) {
                return byte == 0;
            });
        }

        /** @brief Hashes canonical profile bytes before comparing them to host observations. */
        [[nodiscard]] Sha256Digest DigestText(const std::string_view value) noexcept {
            return ComputeSha256(std::as_bytes(std::span{value.data(), value.size()}));
        }

        /** @brief Rejects ambiguous or control-bearing paths before they enter a plan summary. */
        [[nodiscard]] bool ValidAbsolutePath(const std::filesystem::path &path) {
            if (path.empty() || !path.is_absolute())
                return false;
            const std::string text = path.generic_string();
            return text.size() <= 4096 && std::ranges::all_of(text, [](const unsigned char value) {
                return value >= 0x20 && value != 0x7f;
            });
        }

        /** @brief Appends one independently actionable validation failure. */
        void AddIssue(std::vector<ReleasePreflightIssue> &issues, const ReleasePreflightIssueCode code, std::string field,
                      std::string message) {
            issues.push_back({code, std::move(field), std::move(message)});
        }

        /** @brief Formats the stable platform token used by the plan snapshot. */
        [[nodiscard]] const char *PlatformName(const DistributionPlatform platform) noexcept {
            switch (platform) {
                case DistributionPlatform::Windows:
                    return "windows";
                case DistributionPlatform::MacOS:
                    return "macos";
                case DistributionPlatform::Linux:
                    return "linux";
            }
            return "unknown";
        }

        /** @brief Formats the stable architecture token used by the plan snapshot. */
        [[nodiscard]] const char *ArchitectureName(const DistributionArchitecture architecture) noexcept {
            switch (architecture) {
                case DistributionArchitecture::X64:
                    return "x86_64";
                case DistributionArchitecture::Arm64:
                    return "arm64";
            }
            return "unknown";
        }

        /** @brief Formats the stable configuration token used by the plan snapshot. */
        [[nodiscard]] const char *ConfigurationName(const ReleaseBuildConfiguration configuration) noexcept {
            switch (configuration) {
                case ReleaseBuildConfiguration::Debug:
                    return "debug";
                case ReleaseBuildConfiguration::Development:
                    return "development";
                case ReleaseBuildConfiguration::Shipping:
                    return "shipping";
            }
            return "unknown";
        }

        /** @brief Formats the selected product version without changing its product kind. */
        [[nodiscard]] std::string VersionText(const ReleaseProductVersion &version) {
            return std::visit([](const auto &productVersion) {
                return FormatReleaseVersion(productVersion.value);
            }, version);
        }

        /** @brief Distinguishes engine and game versions in machine-readable output. */
        [[nodiscard]] const char *VersionKind(const ReleaseProductVersion &version) noexcept {
            return std::holds_alternative<EngineProductVersion>(version) ? "engine" : "game";
        }

        /** @brief Prevents game and engine product versions from crossing product profiles. */
        [[nodiscard]] bool VersionMatchesProduct(const ReleasePreflightRequest &request) noexcept {
            const bool game = request.profile.Product().kind == DistributionProductKind::GameRuntime ||
                              request.profile.Product().kind == DistributionProductKind::GameDedicatedServer;
            return game == std::holds_alternative<GameProductVersion>(request.version.productVersion);
        }

        /** @brief Revalidates manually constructed typed versions against canonical authority rules. */
        [[nodiscard]] bool ValidVersionAuthority(const ReleaseVersionAuthority &version) {
            const bool validSemantic = std::visit([](const auto &product) {
                const ReleaseSemanticVersion &semantic = product.value;
                if (semantic.prerelease.size() > MaximumReleaseVersionBytes || semantic.buildMetadata.size() > MaximumReleaseVersionBytes)
                    return false;
                const std::string text = FormatReleaseVersion(semantic);
                const auto parsed = ParseReleaseVersion(text);
                return parsed.HasValue() && parsed.Value() == semantic;
            }, version.productVersion);
            if (!validSemantic)
                return false;
            const ReleaseVersionClaim requested{ReleaseVersionClaimSource::Requested,
                                                std::visit([](const auto &product) -> ReleaseVersionClaimValue {
                return product;
            }, version.productVersion)};
            return ValidateReleaseVersionAuthority(std::span{&requested, 1U}, version.sourceRevision).HasValue();
        }

        /** @brief Collects every independent malformed request field before reading facts. */
        void ValidateRequest(const ReleasePreflightRequest &request, std::vector<ReleasePreflightIssue> &issues) {
            if (!ValidAbsolutePath(request.projectRoot) || !IsValidDistributionIdentity(request.projectId))
                AddIssue(issues, ReleasePreflightIssueCode::InvalidRequest, "project",
                         "Project root and identity must be absolute and valid.");
            if (!ValidAbsolutePath(request.outputRoot) || request.requiredFreeBytes == 0)
                AddIssue(issues, ReleasePreflightIssueCode::InvalidRequest, "output", "Output root and required space must be specified.");
            if (!IsValidDistributionIdentity(request.toolchainId))
                AddIssue(issues, ReleasePreflightIssueCode::InvalidRequest, "toolchain", "Toolchain identity is invalid.");
            if ((request.architecture != DistributionArchitecture::X64 && request.architecture != DistributionArchitecture::Arm64) ||
                (request.configuration != ReleaseBuildConfiguration::Debug &&
                 request.configuration != ReleaseBuildConfiguration::Development &&
                 request.configuration != ReleaseBuildConfiguration::Shipping))
                AddIssue(issues, ReleasePreflightIssueCode::InvalidRequest, "target", "Target architecture or configuration is invalid.");
            if (!VersionMatchesProduct(request) || !ValidVersionAuthority(request.version))
                AddIssue(issues, ReleasePreflightIssueCode::InvalidRequest, "version", "Product version or source revision is invalid.");
            if (request.credentials.size() > MaximumReleaseCredentialHandles)
                AddIssue(issues, ReleasePreflightIssueCode::InvalidRequest, "credentials", "Too many credential handles were selected.");
            if (request.profile.Signing() == ReleaseSigningPolicy::Required && request.credentials.empty())
                AddIssue(issues, ReleasePreflightIssueCode::CredentialUnavailable, "credentials",
                         "A signing credential handle is required.");
            for (std::size_t index = 0; index < std::min(request.credentials.size(), MaximumReleaseCredentialHandles); ++index) {
                if (request.credentials[index].value == 0)
                    AddIssue(issues, ReleasePreflightIssueCode::InvalidRequest, "credentials", "A credential handle is invalid.");
                if (std::ranges::find(request.credentials.begin(), request.credentials.begin() + static_cast<std::ptrdiff_t>(index),
                                      request.credentials[index]) != request.credentials.begin() + static_cast<std::ptrdiff_t>(index))
                    AddIssue(issues, ReleasePreflightIssueCode::InvalidRequest, "credentials", "Credential handles must be unique.");
            }
        }

        /** @brief Compares trusted read-only host observations with the requested target. */
        void ValidateFacts(const ReleasePreflightRequest &request, const ReleasePreflightFacts &facts,
                           std::vector<ReleasePreflightIssue> &issues) {
            if (facts.requestedProjectRoot.lexically_normal() != request.projectRoot.lexically_normal())
                AddIssue(issues, ReleasePreflightIssueCode::ProjectUnavailable, "project",
                         "Project observations do not match the requested source root.");
            if (facts.requestedOutputRoot.lexically_normal() != request.outputRoot.lexically_normal())
                AddIssue(issues, ReleasePreflightIssueCode::OutputUnavailable, "output",
                         "Output observations do not match the requested output root.");
            if (!facts.projectReadable || !ValidAbsolutePath(facts.canonicalProjectRoot))
                AddIssue(issues, ReleasePreflightIssueCode::ProjectUnavailable, "project", "Project source is not readable.");
            if (facts.currentVersion != request.version)
                AddIssue(issues, ReleasePreflightIssueCode::SourceChanged, "version",
                         "Observed version or source revision differs from the request.");
            if (facts.profileDigest != DigestText(request.profile.SerializeCanonical()))
                AddIssue(issues, ReleasePreflightIssueCode::ProfileChanged, "profile",
                         "Observed release profile differs from the selected profile.");
            if (!facts.toolchainAvailable || IsEmptyDigest(facts.toolchainDigest))
                AddIssue(issues, ReleasePreflightIssueCode::ToolchainUnavailable, "toolchain", "Selected toolchain is unavailable.");
            if (!facts.targetSupported)
                AddIssue(issues, ReleasePreflightIssueCode::TargetUnsupported, "target", "Selected target is unsupported by this host.");
            if (facts.hostPlatform != DistributionPlatform::Windows && facts.hostPlatform != DistributionPlatform::MacOS &&
                facts.hostPlatform != DistributionPlatform::Linux)
                AddIssue(issues, ReleasePreflightIssueCode::TargetUnsupported, "host", "Host platform is unknown.");
            if (facts.hostPlatform != request.profile.Platform() && !facts.crossCompilerAvailable)
                AddIssue(issues, ReleasePreflightIssueCode::CrossCompilerUnavailable, "target",
                         "Cross-compilation support is unavailable.");
            if (facts.outputExists)
                AddIssue(issues, ReleasePreflightIssueCode::OutputCollision, "output", "The output path already exists.");
            if (!facts.outputWritable || !ValidAbsolutePath(facts.canonicalOutputRoot) ||
                facts.canonicalOutputRoot == facts.canonicalProjectRoot || !facts.availableBytes.has_value())
                AddIssue(issues, ReleasePreflightIssueCode::OutputUnavailable, "output", "Output root is unavailable or unsafe.");
            if (facts.availableBytes.has_value() && *facts.availableBytes < request.requiredFreeBytes)
                AddIssue(issues, ReleasePreflightIssueCode::InsufficientSpace, "output", "Output root does not have enough free space.");
            if (IsEmptyDigest(facts.sourceTreeDigest))
                AddIssue(issues, ReleasePreflightIssueCode::InvalidRequest, "sourceTree", "Source-tree identity is missing.");
            if (IsEmptyDigest(facts.dependencyLockDigest))
                AddIssue(issues, ReleasePreflightIssueCode::InvalidRequest, "dependencyLock", "Dependency-lock identity is missing.");
            if (IsEmptyDigest(facts.policyDigest))
                AddIssue(issues, ReleasePreflightIssueCode::InvalidRequest, "policy", "Release-policy identity is missing.");
            if (facts.availableCapabilities.size() > MaximumObservedCapabilities)
                AddIssue(issues, ReleasePreflightIssueCode::InvalidRequest, "capabilities", "Too many host capabilities were observed.");
            if (facts.availableCredentials.size() > MaximumObservedCredentials)
                AddIssue(issues, ReleasePreflightIssueCode::InvalidRequest, "credentials", "Too many credential handles were observed.");
            const auto capabilities =
                std::span{facts.availableCapabilities}.first(std::min(facts.availableCapabilities.size(), MaximumObservedCapabilities));
            const auto credentials =
                std::span{facts.availableCredentials}.first(std::min(facts.availableCredentials.size(), MaximumObservedCredentials));
            for (const ReleaseCapabilityId &required : request.profile.RequiredCapabilities()) {
                if (std::ranges::find(capabilities, required) == capabilities.end())
                    AddIssue(issues, ReleasePreflightIssueCode::CapabilityUnavailable, "capabilities",
                             "Required release capability is unavailable: " + required.value);
            }
            for (const ReleaseCredentialHandle &handle :
                 std::span{request.credentials}.first(std::min(request.credentials.size(), MaximumReleaseCredentialHandles))) {
                if (std::ranges::find(credentials, handle) == credentials.end())
                    AddIssue(issues, ReleasePreflightIssueCode::CredentialUnavailable, "credentials",
                             "A selected credential handle is unavailable.");
            }
        }
    }  // namespace

    /** @copydoc ReleaseExecutionPlan::ReleaseExecutionPlan */
    ReleaseExecutionPlan::ReleaseExecutionPlan(ReleasePreflightRequest request, std::filesystem::path projectRoot,
                                               std::filesystem::path outputRoot, ReleaseFrozenIdentities identities)
        : request_(std::move(request)), projectRoot_(std::move(projectRoot)), outputRoot_(std::move(outputRoot)),
          identities_(std::move(identities)) {}

    /** @copydoc ReleaseExecutionPlan::Request */
    const ReleasePreflightRequest &ReleaseExecutionPlan::Request() const noexcept {
        return request_;
    }

    /** @copydoc ReleaseExecutionPlan::ProjectRoot */
    const std::filesystem::path &ReleaseExecutionPlan::ProjectRoot() const noexcept {
        return projectRoot_;
    }

    /** @copydoc ReleaseExecutionPlan::OutputRoot */
    const std::filesystem::path &ReleaseExecutionPlan::OutputRoot() const noexcept {
        return outputRoot_;
    }

    /** @copydoc ReleaseExecutionPlan::Identities */
    const ReleaseFrozenIdentities &ReleaseExecutionPlan::Identities() const noexcept {
        return identities_;
    }

    /** @copydoc ReleaseExecutionPlan::Summary */
    std::string ReleaseExecutionPlan::Summary() const {
        return std::format("{} {} for {}/{} ({})\nProject: {}\nSource revision: {}\nSource tree: {}\nDependency lock: {}\n"
                           "Profile: {} ({})\nToolchain: {} ({})\nPolicy: {}\nOutput: {}\nCredentials: {} opaque handle(s)",
                           request_.projectId, VersionText(request_.version.productVersion), PlatformName(request_.profile.Platform()),
                           ArchitectureName(request_.architecture), ConfigurationName(request_.configuration), projectRoot_.string(),
                           request_.version.sourceRevision.value, FormatSha256(identities_.sourceTree),
                           FormatSha256(identities_.dependencyLock), request_.profile.Id().value, FormatSha256(identities_.profile),
                           request_.toolchainId, FormatSha256(identities_.toolchain), FormatSha256(identities_.policy),
                           outputRoot_.string(), request_.credentials.size());
    }

    /** @copydoc ReleaseExecutionPlan::SerializeCanonical */
    std::string ReleaseExecutionPlan::SerializeCanonical() const {
        nlohmann::json credentials = nlohmann::json::array();
        for (const ReleaseCredentialHandle &handle : request_.credentials)
            credentials.push_back(handle.value);
        const nlohmann::json snapshot{{"schemaVersion", 1},
                                      {"project", {{"id", request_.projectId}, {"root", projectRoot_.generic_string()}}},
                                      {"version",
                                       {{"kind", VersionKind(request_.version.productVersion)},
                                        {"value", VersionText(request_.version.productVersion)},
                                        {"sourceRevision", request_.version.sourceRevision.value}}},
                                      {"profile", nlohmann::json::parse(request_.profile.SerializeCanonical())},
                                      {"target",
                                       {{"platform", PlatformName(request_.profile.Platform())},
                                        {"architecture", ArchitectureName(request_.architecture)},
                                        {"configuration", ConfigurationName(request_.configuration)}}},
                                      {"toolchainId", request_.toolchainId},
                                      {"outputRoot", outputRoot_.generic_string()},
                                      {"requiredFreeBytes", request_.requiredFreeBytes},
                                      {"credentialHandles", std::move(credentials)},
                                      {"reproducible", request_.reproducible},
                                      {"identities",
                                       {{"sourceTree", FormatSha256(identities_.sourceTree)},
                                        {"dependencyLock", FormatSha256(identities_.dependencyLock)},
                                        {"profile", FormatSha256(identities_.profile)},
                                        {"toolchain", FormatSha256(identities_.toolchain)},
                                        {"policy", FormatSha256(identities_.policy)}}}};
        return snapshot.dump() + '\n';
    }

    /** @copydoc PreflightRelease */
    ReleasePreflightOutcome PreflightRelease(const ReleasePreflightRequest &request, const ReleasePreflightFacts &facts) {
        ReleasePreflightOutcome outcome;
        ValidateRequest(request, outcome.issues);
        ValidateFacts(request, facts, outcome.issues);
        if (outcome.issues.empty()) {
            outcome.plan = ReleaseExecutionPlan{request, facts.canonicalProjectRoot, facts.canonicalOutputRoot,
                                                ReleaseFrozenIdentities{facts.sourceTreeDigest, facts.dependencyLockDigest,
                                                                        facts.profileDigest, facts.toolchainDigest, facts.policyDigest}};
        }
        return outcome;
    }

    /** @copydoc ValidateReleaseInputFreeze */
    std::vector<ReleasePreflightIssue> ValidateReleaseInputFreeze(const ReleaseExecutionPlan &plan, const ReleasePreflightFacts &current) {
        std::vector<ReleasePreflightIssue> issues;
        const auto check = [&issues](const bool changed, const std::string_view field) {
            if (changed)
                AddIssue(issues, ReleasePreflightIssueCode::InputChanged, std::string{field}, "Frozen release input changed.");
        };
        check(current.canonicalProjectRoot != plan.ProjectRoot(), "project");
        check(current.requestedProjectRoot.lexically_normal() != plan.Request().projectRoot.lexically_normal(), "projectRequest");
        check(!current.projectReadable, "projectAccess");
        check(current.canonicalOutputRoot != plan.OutputRoot(), "output");
        check(current.requestedOutputRoot.lexically_normal() != plan.Request().outputRoot.lexically_normal(), "outputRequest");
        check(!current.outputWritable, "outputAccess");
        check(current.currentVersion != plan.Request().version, "version");
        check(current.sourceTreeDigest != plan.Identities().sourceTree, "source");
        check(current.dependencyLockDigest != plan.Identities().dependencyLock, "dependencyLock");
        check(current.profileDigest != plan.Identities().profile, "profile");
        check(current.toolchainDigest != plan.Identities().toolchain, "toolchain");
        check(!current.toolchainAvailable, "toolchainAccess");
        check(!current.targetSupported || (current.hostPlatform != plan.Request().profile.Platform() && !current.crossCompilerAvailable),
              "targetSupport");
        check(current.policyDigest != plan.Identities().policy, "policy");
        const auto capabilities =
            std::span{current.availableCapabilities}.first(std::min(current.availableCapabilities.size(), MaximumObservedCapabilities));
        const auto credentials =
            std::span{current.availableCredentials}.first(std::min(current.availableCredentials.size(), MaximumObservedCredentials));
        check(current.availableCapabilities.size() > MaximumObservedCapabilities, "capabilities");
        check(current.availableCredentials.size() > MaximumObservedCredentials, "credentials");
        for (const ReleaseCapabilityId &required : plan.Request().profile.RequiredCapabilities())
            check(std::ranges::find(capabilities, required) == capabilities.end(), "capabilities");
        for (const ReleaseCredentialHandle &handle : plan.Request().credentials)
            check(std::ranges::find(credentials, handle) == credentials.end(), "credentials");
        return issues;
    }
}  // namespace Horo::Release
