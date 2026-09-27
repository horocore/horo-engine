#include "Horo/Release/ReleasePreflight.h"

#include "Horo/Release/DistributionModel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
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

        /** @brief Validates a strict ISO calendar date without host locale or clock state. */
        [[nodiscard]] bool ValidNotesDate(const std::string_view text) noexcept {
            if (text.size() != 10 || text[4] != '-' || text[7] != '-')
                return false;
            const auto number = [](const std::string_view part) {
                int value{};
                const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
                return error == std::errc{} && end == part.data() + part.size() ? value : 0;
            };
            return std::chrono::year_month_day{std::chrono::year{number(text.substr(0, 4))},
                                               std::chrono::month{static_cast<unsigned>(number(text.substr(5, 2)))},
                                               std::chrono::day{static_cast<unsigned>(number(text.substr(8, 2)))}}
                .ok();
        }

        /** @brief Checks the bounded fields shared by every reviewed snapshot. */
        [[nodiscard]] bool ValidNotesFields(const nlohmann::json &notes) {
            if (!notes.is_object() || notes.size() != 7 || !notes.contains("schemaVersion") ||
                !notes["schemaVersion"].is_number_integer() || notes["schemaVersion"] != 1)
                return false;
            for (const std::string_view field : {"product", "version", "locale", "date", "markdown"}) {
                const std::string name{field};
                if (!notes.contains(name) || !notes[name].is_string())
                    return false;
            }
            if (!notes.contains("sections") || !notes["sections"].is_array() || notes["sections"].empty() || notes["sections"].size() > 6)
                return false;
            const std::string locale = notes["locale"].get<std::string>();
            const std::string date = notes["date"].get<std::string>();
            return locale.size() == 5 && locale[2] == '-' && std::islower(static_cast<unsigned char>(locale[0])) &&
                   std::islower(static_cast<unsigned char>(locale[1])) && std::isupper(static_cast<unsigned char>(locale[3])) &&
                   std::isupper(static_cast<unsigned char>(locale[4])) && ValidNotesDate(date) &&
                   ParseReleaseVersion(notes["version"].get<std::string>()).HasValue();
        }

        /** @brief Checks one reviewed Markdown item before reconstructing its canonical section. */
        [[nodiscard]] bool ValidNotesItem(const nlohmann::json &item) {
            if (!item.is_string())
                return false;
            const std::string value = item.get<std::string>();
            if (value.empty() || value.size() > 2048 || value.find('<') != std::string::npos || value.find('>') != std::string::npos ||
                value.find("![") != std::string::npos || std::ranges::count(value, '`') % 2 != 0 ||
                std::ranges::count(value, '[') != std::ranges::count(value, ']') ||
                std::ranges::count(value, '(') != std::ranges::count(value, ')') ||
                std::ranges::any_of(value, [](const unsigned char character) {
                return character < 0x20 || character == 0x7f;
            }))
                return false;
            for (std::size_t link = value.find("]("); link != std::string::npos; link = value.find("](", link + 2)) {
                if (!std::string_view{value}.substr(link + 2).starts_with("https://"))
                    return false;
            }
            return true;
        }

        /** @brief Reconstructs canonical Markdown so sections and distribution text cannot diverge. */
        [[nodiscard]] bool ValidNotesSections(const nlohmann::json &notes) {
            static constexpr std::array<std::string_view, 6> categories{"Added", "Changed", "Deprecated", "Removed", "Fixed", "Security"};
            std::array<bool, categories.size()> seen{};
            std::size_t itemCount{};
            std::string markdown = std::format("## [{}] — {}\n", notes["version"].get<std::string>(), notes["date"].get<std::string>());
            for (const auto &section : notes["sections"]) {
                if (!section.is_object() || section.size() != 2 || !section.contains("category") || !section["category"].is_string() ||
                    !section.contains("items") || !section["items"].is_array() || section["items"].empty())
                    return false;
                const std::string category = section["category"].get<std::string>();
                const auto found = std::ranges::find(categories, std::string_view{category});
                if (found == categories.end())
                    return false;
                const auto index = static_cast<std::size_t>(found - categories.begin());
                if (seen[index])
                    return false;
                seen[index] = true;
                markdown += std::format("\n### {}\n", category);
                for (const auto &item : section["items"]) {
                    if (!ValidNotesItem(item))
                        return false;
                    ++itemCount;
                    if (itemCount > 64)
                        return false;
                    markdown += std::format("- {}\n", item.get<std::string>());
                }
            }
            return notes["markdown"] == markdown;
        }

        /** @brief Ensures the reviewed snapshot has only bounded typed fields. */
        [[nodiscard]] bool ValidNotesShape(const nlohmann::json &notes) {
            return ValidNotesFields(notes) && ValidNotesSections(notes);
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

        /** @brief Finds a component-aligned ancestor, including the path itself. */
        [[nodiscard]] bool ContainsPath(const std::filesystem::path &ancestor, const std::filesystem::path &path) {
            const auto normalizedAncestor = ancestor.lexically_normal();
            const auto normalizedPath = path.lexically_normal();
            auto ancestorPart = normalizedAncestor.begin();
            auto pathPart = normalizedPath.begin();
            while (ancestorPart != normalizedAncestor.end() && pathPart != normalizedPath.end()) {
                if (*ancestorPart != *pathPart)
                    return false;
                ++ancestorPart;
                ++pathPart;
            }
            return ancestorPart == normalizedAncestor.end();
        }

        /** @brief Appends one independently actionable validation failure. */
        void AddIssue(std::vector<ReleasePreflightIssue> &issues, const ReleasePreflightIssueCode code, std::string field,
                      std::string message) {
            issues.emplace_back(code, std::move(field), std::move(message));
        }

        [[nodiscard]] std::string VersionText(const ReleaseProductVersion &version);

        /** @brief Validates the host-captured notes without reaching back into source files. */
        void ValidateNotes(const ReleasePreflightRequest &request, const ReleasePreflightFacts &facts,
                           std::vector<ReleasePreflightIssue> &issues) {
            using enum ReleasePreflightIssueCode;
            const std::string &bytes = facts.releaseNotesSnapshot;
            if (bytes.empty()) {
                AddIssue(issues, NotesMissing, "notes", "Reviewed release notes snapshot is missing.");
                return;
            }
            if (bytes.size() > MaximumReleaseNotesSnapshotBytes) {
                AddIssue(issues, NotesOversized, "notes", "Reviewed release notes snapshot exceeds 32768 bytes.");
                return;
            }
            const auto notes = nlohmann::json::parse(bytes, nullptr, false);
            if (notes.is_discarded() || !ValidNotesShape(notes)) {
                AddIssue(issues, NotesMalformed, "notes", "Reviewed release notes snapshot is malformed or inconsistent.");
                return;
            }
            if (const std::string version = notes["version"].get<std::string>(); version != VersionText(request.version.productVersion))
                AddIssue(issues, NotesVersionMismatch, "notes.version", "Release notes must match the exact candidate SemVer.");
            if (notes["product"] != request.projectId)
                AddIssue(issues, NotesProductMismatch, "notes.product", "Release notes product differs from the candidate.");
        }

        /** @brief Formats the stable platform token used by the plan snapshot. */
        [[nodiscard]] const char *PlatformName(const DistributionPlatform platform) noexcept {
            using enum DistributionPlatform;
            switch (platform) {
                case Windows:
                    return "windows";
                case MacOS:
                    return "macos";
                case Linux:
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
            using enum ReleaseBuildConfiguration;
            switch (configuration) {
                case Debug:
                    return "debug";
                case Development:
                    return "development";
                case Shipping:
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
            const auto validateSemantic = [](const auto &product) {
                const ReleaseSemanticVersion &semantic = product.value;
                if (semantic.prerelease.size() > MaximumReleaseVersionBytes || semantic.buildMetadata.size() > MaximumReleaseVersionBytes)
                    return false;
                const std::string text = FormatReleaseVersion(semantic);
                const auto parsed = ParseReleaseVersion(text);
                return parsed.HasValue() && parsed.Value() == semantic;
            };
            if (const bool validSemantic = std::visit(validateSemantic, version.productVersion); !validSemantic)
                return false;
            const ReleaseVersionClaim requested{ReleaseVersionClaimSource::Requested,
                                                std::visit([](const auto &product) -> ReleaseVersionClaimValue {
                return product;
            }, version.productVersion)};
            return ValidateReleaseVersionAuthority(std::span{&requested, 1U}, version.sourceRevision).HasValue();
        }

        /** @brief Checks the bounded credential list without hiding duplicate or invalid handles. */
        void ValidateCredentialHandles(const std::vector<ReleaseCredentialHandle> &credentials,
                                       std::vector<ReleasePreflightIssue> &issues) {
            using enum ReleasePreflightIssueCode;
            for (std::size_t index = 0; index < std::min(credentials.size(), MaximumReleaseCredentialHandles); ++index) {
                if (credentials[index].value == 0)
                    AddIssue(issues, InvalidRequest, "credentials", "A credential handle is invalid.");
                if (std::ranges::find(credentials.begin(), credentials.begin() + static_cast<std::ptrdiff_t>(index), credentials[index]) !=
                    credentials.begin() + static_cast<std::ptrdiff_t>(index))
                    AddIssue(issues, InvalidRequest, "credentials", "Credential handles must be unique.");
            }
        }

        /** @brief Collects every independent malformed request field before reading facts. */
        void ValidateRequest(const ReleasePreflightRequest &request, std::vector<ReleasePreflightIssue> &issues) {
            using enum ReleasePreflightIssueCode;
            if (!ValidAbsolutePath(request.projectRoot) || !IsValidDistributionIdentity(request.projectId))
                AddIssue(issues, InvalidRequest, "project", "Project root and identity must be absolute and valid.");
            if (!ValidAbsolutePath(request.outputRoot) || request.requiredFreeBytes == 0)
                AddIssue(issues, InvalidRequest, "output", "Output root and required space must be specified.");
            if (!IsValidDistributionIdentity(request.toolchainId))
                AddIssue(issues, InvalidRequest, "toolchain", "Toolchain identity is invalid.");
            if ((request.architecture != DistributionArchitecture::X64 && request.architecture != DistributionArchitecture::Arm64) ||
                (request.configuration != ReleaseBuildConfiguration::Debug &&
                 request.configuration != ReleaseBuildConfiguration::Development &&
                 request.configuration != ReleaseBuildConfiguration::Shipping))
                AddIssue(issues, InvalidRequest, "target", "Target architecture or configuration is invalid.");
            if (!VersionMatchesProduct(request) || !ValidVersionAuthority(request.version))
                AddIssue(issues, InvalidRequest, "version", "Product version or source revision is invalid.");
            if (request.credentials.size() > MaximumReleaseCredentialHandles)
                AddIssue(issues, InvalidRequest, "credentials", "Too many credential handles were selected.");
            if ((request.profile.Signing() == ReleaseSigningPolicy::Disabled && request.signingSelected) ||
                (request.profile.Signing() == ReleaseSigningPolicy::Required && !request.signingSelected))
                AddIssue(issues, InvalidRequest, "signing", "Signing selection conflicts with the release profile.");
            if (request.signingSelected && request.credentials.empty())
                AddIssue(issues, CredentialUnavailable, "credentials", "A signing credential handle is required.");
            if (request.publicationDestination) {
                const auto eligible = request.profile.EligibleDestinations();
                if (!IsValidDistributionIdentity(request.publicationDestination->value) ||
                    std::ranges::find(eligible, *request.publicationDestination) == eligible.end())
                    AddIssue(issues, InvalidRequest, "publication", "Publication destination is not allowed by the profile.");
            }
            ValidateCredentialHandles(request.credentials, issues);
        }

        /** @brief Checks bounded host capabilities and credential handles against the selected profile. */
        void ValidateObservedAccess(const ReleasePreflightRequest &request, const ReleasePreflightFacts &facts,
                                    std::vector<ReleasePreflightIssue> &issues) {
            using enum ReleasePreflightIssueCode;
            if (facts.availableCapabilities.size() > MaximumObservedCapabilities)
                AddIssue(issues, InvalidRequest, "capabilities", "Too many host capabilities were observed.");
            if (facts.availableCredentials.size() > MaximumObservedCredentials)
                AddIssue(issues, InvalidRequest, "credentials", "Too many credential handles were observed.");
            const auto capabilities =
                std::span{facts.availableCapabilities}.first(std::min(facts.availableCapabilities.size(), MaximumObservedCapabilities));
            const auto credentials =
                std::span{facts.availableCredentials}.first(std::min(facts.availableCredentials.size(), MaximumObservedCredentials));
            for (const ReleaseCapabilityId &required : request.profile.RequiredCapabilities()) {
                if (std::ranges::find(capabilities, required) == capabilities.end())
                    AddIssue(issues, CapabilityUnavailable, "capabilities",
                             "Required release capability is unavailable: " + required.value);
            }
            for (const ReleaseCredentialHandle &handle :
                 std::span{request.credentials}.first(std::min(request.credentials.size(), MaximumReleaseCredentialHandles))) {
                if (std::ranges::find(credentials, handle) == credentials.end())
                    AddIssue(issues, CredentialUnavailable, "credentials", "A selected credential handle is unavailable.");
            }
        }

        /** @brief Compares trusted read-only host observations with the requested target. */
        void ValidateFacts(const ReleasePreflightRequest &request, const ReleasePreflightFacts &facts,
                           std::vector<ReleasePreflightIssue> &issues) {
            using enum ReleasePreflightIssueCode;
            if (facts.requestedProjectRoot.lexically_normal() != request.projectRoot.lexically_normal())
                AddIssue(issues, ProjectUnavailable, "project", "Project observations do not match the requested source root.");
            if (facts.requestedOutputRoot.lexically_normal() != request.outputRoot.lexically_normal())
                AddIssue(issues, OutputUnavailable, "output", "Output observations do not match the requested output root.");
            if (!facts.projectReadable || !ValidAbsolutePath(facts.canonicalProjectRoot))
                AddIssue(issues, ProjectUnavailable, "project", "Project source is not readable.");
            if (facts.currentVersion != request.version)
                AddIssue(issues, SourceChanged, "version", "Observed version or source revision differs from the request.");
            if (facts.profileDigest != DigestText(request.profile.SerializeCanonical()))
                AddIssue(issues, ProfileChanged, "profile", "Observed release profile differs from the selected profile.");
            if (!facts.toolchainAvailable || IsEmptyDigest(facts.toolchainDigest))
                AddIssue(issues, ToolchainUnavailable, "toolchain", "Selected toolchain is unavailable.");
            if (!facts.targetSupported)
                AddIssue(issues, TargetUnsupported, "target", "Selected target is unsupported by this host.");
            if (facts.hostPlatform != DistributionPlatform::Windows && facts.hostPlatform != DistributionPlatform::MacOS &&
                facts.hostPlatform != DistributionPlatform::Linux)
                AddIssue(issues, TargetUnsupported, "host", "Host platform is unknown.");
            if (facts.hostPlatform != request.profile.Platform() && !facts.crossCompilerAvailable)
                AddIssue(issues, CrossCompilerUnavailable, "target", "Cross-compilation support is unavailable.");
            if (facts.outputExists)
                AddIssue(issues, OutputCollision, "output", "The output path already exists.");
            if (!facts.outputWritable || !ValidAbsolutePath(facts.canonicalOutputRoot) ||
                (ValidAbsolutePath(facts.canonicalProjectRoot) && ValidAbsolutePath(facts.canonicalOutputRoot) &&
                 (ContainsPath(facts.canonicalProjectRoot, facts.canonicalOutputRoot) ||
                  ContainsPath(facts.canonicalOutputRoot, facts.canonicalProjectRoot))) ||
                !facts.availableBytes.has_value())
                AddIssue(issues, OutputUnavailable, "output", "Output root is unavailable or unsafe.");
            if (facts.availableBytes.has_value() && *facts.availableBytes < request.requiredFreeBytes)
                AddIssue(issues, InsufficientSpace, "output", "Output root does not have enough free space.");
            if (IsEmptyDigest(facts.sourceTreeDigest))
                AddIssue(issues, InvalidRequest, "sourceTree", "Source-tree identity is missing.");
            if (IsEmptyDigest(facts.dependencyLockDigest))
                AddIssue(issues, InvalidRequest, "dependencyLock", "Dependency-lock identity is missing.");
            if (IsEmptyDigest(facts.policyDigest))
                AddIssue(issues, InvalidRequest, "policy", "Release-policy identity is missing.");
            ValidateNotes(request, facts, issues);
            ValidateObservedAccess(request, facts, issues);
        }
    }  // namespace

    /** @copydoc ReleaseExecutionPlan::ReleaseExecutionPlan */
    ReleaseExecutionPlan::ReleaseExecutionPlan(ReleasePreflightRequest request, std::filesystem::path projectRoot,
                                               std::filesystem::path outputRoot, ReleaseFrozenIdentities identities,
                                               std::string releaseNotesSnapshot)
        : request_(std::move(request)), projectRoot_(std::move(projectRoot)), outputRoot_(std::move(outputRoot)),
          identities_(std::move(identities)), releaseNotesSnapshot_(std::move(releaseNotesSnapshot)) {}

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

    /** @copydoc ReleaseExecutionPlan::ReleaseNotesSnapshot */
    const std::string &ReleaseExecutionPlan::ReleaseNotesSnapshot() const noexcept {
        return releaseNotesSnapshot_;
    }

    /** @copydoc ReleaseExecutionPlan::Summary */
    std::string ReleaseExecutionPlan::Summary() const {
        return std::format("{} {} for {}/{} ({})\nProject: {}\nSource revision: {}\nSource tree: {}\nDependency lock: {}\n"
                           "Profile: {} ({})\nToolchain: {} ({})\nPolicy: {}\nNotes: {}\nOutput: {}\nSigning: {}\nPublication: {}\n"
                           "Credentials: {} opaque handle(s)",
                           request_.projectId, VersionText(request_.version.productVersion), PlatformName(request_.profile.Platform()),
                           ArchitectureName(request_.architecture), ConfigurationName(request_.configuration), projectRoot_.string(),
                           request_.version.sourceRevision.value, FormatSha256(identities_.sourceTree),
                           FormatSha256(identities_.dependencyLock), request_.profile.Id().value, FormatSha256(identities_.profile),
                           request_.toolchainId, FormatSha256(identities_.toolchain), FormatSha256(identities_.policy),
                           FormatSha256(identities_.notes), outputRoot_.string(), request_.signingSelected ? "selected" : "disabled",
                           request_.publicationDestination ? request_.publicationDestination->value : "local candidate",
                           request_.credentials.size());
    }

    /** @copydoc ReleaseExecutionPlan::SerializeCanonical */
    std::string ReleaseExecutionPlan::SerializeCanonical() const {
        nlohmann::json credentials = nlohmann::json::array();
        for (const ReleaseCredentialHandle &handle : request_.credentials)
            credentials.push_back(handle.value);
        nlohmann::json publicationDestination = nullptr;
        if (request_.publicationDestination)
            publicationDestination = request_.publicationDestination->value;
        const nlohmann::json snapshot{{"schemaVersion", 1},
                                      {"project", {{"id", request_.projectId}, {"root", projectRoot_.generic_string()}}},
                                      {"version",
                                       {{"kind", VersionKind(request_.version.productVersion)},
                                        {"value", VersionText(request_.version.productVersion)},
                                        {"sourceRevision", request_.version.sourceRevision.value}}},
                                      {"profile", nlohmann::json::parse(request_.profile.SerializeCanonical())},
                                      {"releaseNotes", {{"digest", FormatSha256(identities_.notes)}, {"bytes", releaseNotesSnapshot_}}},
                                      {"target",
                                       {{"platform", PlatformName(request_.profile.Platform())},
                                        {"architecture", ArchitectureName(request_.architecture)},
                                        {"configuration", ConfigurationName(request_.configuration)}}},
                                      {"toolchainId", request_.toolchainId},
                                      {"outputRoot", outputRoot_.generic_string()},
                                      {"requiredFreeBytes", request_.requiredFreeBytes},
                                      {"credentialHandles", std::move(credentials)},
                                      {"signingSelected", request_.signingSelected},
                                      {"publicationDestination", std::move(publicationDestination)},
                                      {"reproducible", request_.reproducible},
                                      {"identities",
                                       {{"sourceTree", FormatSha256(identities_.sourceTree)},
                                        {"dependencyLock", FormatSha256(identities_.dependencyLock)},
                                        {"profile", FormatSha256(identities_.profile)},
                                        {"toolchain", FormatSha256(identities_.toolchain)},
                                        {"policy", FormatSha256(identities_.policy)},
                                        {"notes", FormatSha256(identities_.notes)}}}};
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
                                                                        facts.profileDigest, facts.toolchainDigest, facts.policyDigest,
                                                                        DigestText(facts.releaseNotesSnapshot)},
                                                facts.releaseNotesSnapshot};
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
        if (DigestText(current.releaseNotesSnapshot) != plan.Identities().notes)
            AddIssue(issues, ReleasePreflightIssueCode::NotesChanged, "notes",
                     "Reviewed release notes differ from the frozen candidate snapshot.");
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
