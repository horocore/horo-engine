#include "Horo/Release/ReleaseTargetMatrix.h"

#include "Horo/Release/DistributionModel.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace Horo::Release {
    namespace {
        /** @brief Appends a stable, field-specific admission failure. */
        void AddIssue(std::vector<ReleaseTargetIssue> &issues, const ReleaseTargetIssueCode code, const std::string_view targetId,
                      std::string field, std::string message) {
            issues.emplace_back(code, std::string{targetId}, std::move(field), std::move(message));
        }

        /** @brief Checks the closed platform identity range before any enum-based lookup. */
        [[nodiscard]] bool ValidMachine(const ReleaseMachine machine) noexcept {
            return machine.platform >= DistributionPlatform::Windows && machine.platform <= DistributionPlatform::Linux &&
                   machine.architecture >= DistributionArchitecture::X64 && machine.architecture <= DistributionArchitecture::Arm64;
        }

        /** @brief Applies the currently qualified desktop architecture matrix. */
        [[nodiscard]] bool SupportedTarget(const ReleaseMachine target) noexcept {
            return ValidMachine(target) &&
                   (target.platform == DistributionPlatform::MacOS || target.architecture == DistributionArchitecture::X64);
        }

        /** @brief Treats an all-zero SHA-256 value as absent observation. */
        [[nodiscard]] bool ValidDigest(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](const std::uint8_t byte) {
                return byte != 0;
            });
        }

        /** @brief Validates the descriptor matching one cell without trusting broad preflight booleans. */
        [[nodiscard]] const ReleaseToolchainDescriptor *ValidateToolchain(const ReleaseMatrixCellRequest &cell, const ReleaseMachine host,
                                                                          const std::span<const ReleaseToolchainDescriptor> toolchains,
                                                                          std::vector<ReleaseTargetIssue> &issues) {
            using enum ReleaseTargetIssueCode;
            const ReleaseMachine target{cell.request.profile.Platform(), cell.request.architecture};
            if (target.platform == DistributionPlatform::MacOS && host.platform != DistributionPlatform::MacOS)
                AddIssue(issues, UnsupportedHostTarget, cell.targetId, "host", "macOS targets require a macOS host.");
            if (!SupportedTarget(target))
                AddIssue(issues, UnsupportedArchitecture, cell.targetId, "architecture", "Target architecture is not qualified.");

            const ReleaseToolchainDescriptor *selected = nullptr;
            for (const ReleaseToolchainDescriptor &toolchain : toolchains) {
                if (toolchain.id != cell.request.toolchainId)
                    continue;
                if (selected) {
                    AddIssue(issues, ToolchainAmbiguous, cell.targetId, "toolchain", "More than one toolchain has the selected identity.");
                    return nullptr;
                }
                selected = &toolchain;
            }
            if (!selected) {
                AddIssue(issues, ToolchainMissing, cell.targetId, "toolchain", "Selected toolchain is not configured.");
                return nullptr;
            }
            if (!selected->enabled || !selected->compileAndLinkValidated || !ValidDigest(selected->digest))
                AddIssue(issues, ToolchainMismatch, cell.targetId, "toolchain", "Toolchain is disabled or lacks compile/link validation.");
            if (selected->digest != cell.facts.toolchainDigest || selected->host != host || selected->target != target ||
                cell.facts.hostPlatform != host.platform)
                AddIssue(issues, ToolchainMismatch, cell.targetId, "toolchain",
                         "Toolchain observations do not match host, target or identity.");
            if (host != target && !selected->explicitCrossToolchain)
                AddIssue(issues, CrossToolchainRequired, cell.targetId, "toolchain",
                         "Cross-compilation requires an explicit compatible profile.");
            if (host == target && selected->explicitCrossToolchain)
                AddIssue(issues, ToolchainMismatch, cell.targetId, "toolchain", "Native target selected a cross-toolchain profile.");

            if (cell.minimumPlatform.major == 0 || selected->oldestSupportedPlatform.major == 0 ||
                selected->newestSupportedPlatform.major == 0 || selected->oldestSupportedPlatform > selected->newestSupportedPlatform ||
                cell.minimumPlatform < selected->oldestSupportedPlatform || cell.minimumPlatform > selected->newestSupportedPlatform)
                AddIssue(issues, UnsupportedPlatformVersion, cell.targetId, "minimumPlatform",
                         "Minimum platform version is outside the validated toolchain range.");
            if (!IsValidDistributionIdentity(cell.sdkId) || !selected->sdk.available || selected->sdk.id != cell.sdkId ||
                selected->sdk.platform != target.platform || selected->sdk.version.major == 0 ||
                selected->sdk.oldestSupportedPlatform.major == 0 || selected->sdk.newestSupportedPlatform.major == 0 ||
                selected->sdk.oldestSupportedPlatform > selected->sdk.newestSupportedPlatform ||
                cell.minimumPlatform < selected->sdk.oldestSupportedPlatform ||
                cell.minimumPlatform > selected->sdk.newestSupportedPlatform)
                AddIssue(issues, SdkUnavailable, cell.targetId, "sdk", "Matching SDK does not support the minimum platform.");

            if (const DistributionPackageFormat format = cell.request.profile.PackageFormat();
                std::ranges::find(selected->packageFormats, format) == selected->packageFormats.end() ||
                ValidateDistributionProductPackageFormat(cell.request.profile.Product(), cell.request.profile.ArtifactClass(),
                                                         target.platform, format)
                    .HasError())
                AddIssue(issues, PackageFormatUnsupported, cell.targetId, "packageFormat",
                         "Selected package format is unsupported by the target or toolchain.");
            return selected;
        }

        /** @brief Rejects duplicate identities, equivalent target tuples, and colliding output roots. */
        void ValidateCellIdentity(const ReleaseMatrixCellRequest &cell, const std::span<const ReleaseMatrixCellRequest> previousRequests,
                                  const std::vector<ReleaseMatrixCellPlan> &previousPlans, std::vector<ReleaseTargetIssue> &issues) {
            using enum ReleaseTargetIssueCode;
            if (!IsValidDistributionIdentity(cell.jobId))
                AddIssue(issues, InvalidTarget, cell.targetId, "jobId", "Job identity is invalid.");
            if (!IsValidDistributionIdentity(cell.targetId))
                AddIssue(issues, InvalidTarget, cell.targetId, "targetId", "Target identity is invalid.");
            if (cell.requirement != ReleaseMatrixRequirement::Required && cell.requirement != ReleaseMatrixRequirement::Optional)
                AddIssue(issues, InvalidTarget, cell.targetId, "requirement", "Target requirement is invalid.");
            if (std::ranges::any_of(previousPlans, [&cell](const ReleaseMatrixCellPlan &previous) {
                return previous.targetId == cell.targetId;
            }))
                AddIssue(issues, InvalidTarget, cell.targetId, "targetId", "Target identity is duplicated in the group.");
            if (std::ranges::any_of(previousPlans, [&cell](const ReleaseMatrixCellPlan &previous) {
                return previous.jobId == cell.jobId;
            }))
                AddIssue(issues, InvalidTarget, cell.targetId, "jobId", "Job identity is duplicated in the group.");
            for (const ReleaseMatrixCellRequest &previous : previousRequests) {
                if (previous.request.projectId == cell.request.projectId && previous.request.profile.Id() == cell.request.profile.Id() &&
                    previous.request.profile.Platform() == cell.request.profile.Platform() &&
                    previous.request.architecture == cell.request.architecture &&
                    previous.request.configuration == cell.request.configuration &&
                    previous.request.toolchainId == cell.request.toolchainId)
                    AddIssue(issues, InvalidTarget, cell.targetId, "target", "Target tuple is duplicated in the group.");
                if (previous.facts.canonicalOutputRoot.lexically_normal() == cell.facts.canonicalOutputRoot.lexically_normal())
                    AddIssue(issues, InvalidTarget, cell.targetId, "output", "Target output root is duplicated in the group.");
            }
        }

        /** @brief Admits one cell only after identity, toolchain, and preflight evidence agree. */
        [[nodiscard]] ReleaseMatrixCellPlan PlanCell(const ReleaseMatrixCellRequest &cell, const ReleaseMachine host,
                                                     const std::span<const ReleaseMatrixCellRequest> previousRequests,
                                                     const std::vector<ReleaseMatrixCellPlan> &previousPlans,
                                                     const std::span<const ReleaseToolchainDescriptor> toolchains) {
            ReleaseMatrixCellPlan planned{cell.jobId, cell.targetId, cell.requirement, std::nullopt, std::nullopt, {}};
            ValidateCellIdentity(cell, previousRequests, previousPlans, planned.issues);
            const ReleaseToolchainDescriptor *selected = ValidateToolchain(cell, host, toolchains, planned.issues);
            ReleasePreflightOutcome preflight = PreflightRelease(cell.request, cell.facts);
            for (const ReleasePreflightIssue &issue : preflight.issues)
                AddIssue(planned.issues, ReleaseTargetIssueCode::PreflightFailed, cell.targetId, issue.field, issue.message);
            if (!planned.issues.empty() || !selected)
                return planned;

            planned.plan = std::move(preflight.plan);
            const auto capabilities =
                ValidateDistributionProductPackageFormat(cell.request.profile.Product(), cell.request.profile.ArtifactClass(),
                                                         cell.request.profile.Platform(), cell.request.profile.PackageFormat());
            planned.validatedTarget = ReleaseValidatedTarget{{cell.request.profile.Platform(), cell.request.architecture},
                                                             cell.minimumPlatform,
                                                             selected->sdk.id,
                                                             selected->sdk.version,
                                                             selected->sdk.oldestSupportedPlatform,
                                                             selected->sdk.newestSupportedPlatform,
                                                             cell.request.profile.PackageFormat(),
                                                             capabilities.Value(),
                                                             selected->digest};
            return planned;
        }

        /** @brief Finds the exact group/job/target terminal and rejects duplicate evidence. */
        [[nodiscard]] const ReleaseTargetTerminal *FindTerminal(const std::string_view groupId, const ReleaseMatrixCellPlan &cell,
                                                                const std::span<const ReleaseTargetTerminal> terminals,
                                                                std::vector<ReleaseTargetIssue> &issues, bool &invalidEvidence) {
            const ReleaseTargetTerminal *matched = nullptr;
            for (const ReleaseTargetTerminal &terminal : terminals) {
                if (terminal.groupId != groupId || terminal.jobId != cell.jobId || terminal.targetId != cell.targetId)
                    continue;
                if (matched) {
                    AddIssue(issues, ReleaseTargetIssueCode::InvalidMatrix, cell.targetId, "terminal",
                             "Target has more than one terminal result.");
                    invalidEvidence = true;
                    break;
                }
                matched = &terminal;
            }
            return matched;
        }

        /** @brief Maps and validates one admitted job's terminal evidence. */
        void ApplyTerminal(const ReleaseMatrixCellPlan &cell, const ReleaseTargetTerminal &terminal, ReleaseMatrixMemberResult &member,
                           std::vector<ReleaseTargetIssue> &issues, bool &invalidEvidence) {
            member.terminal = terminal;
            if (cell.plan) {
                switch (terminal.state) {
                    case ReleaseTargetTerminalState::Succeeded:
                        member.state = ReleaseMatrixMemberState::Succeeded;
                        break;
                    case ReleaseTargetTerminalState::Failed:
                        member.state = ReleaseMatrixMemberState::Failed;
                        break;
                    case ReleaseTargetTerminalState::Cancelled:
                        member.state = ReleaseMatrixMemberState::Cancelled;
                        break;
                }
            }
            if (terminal.state != ReleaseTargetTerminalState::Succeeded && terminal.state != ReleaseTargetTerminalState::Failed &&
                terminal.state != ReleaseTargetTerminalState::Cancelled) {
                AddIssue(issues, ReleaseTargetIssueCode::InvalidMatrix, cell.targetId, "terminal", "Target has an invalid terminal state.");
                invalidEvidence = true;
            }
            if (!cell.plan) {
                AddIssue(issues, ReleaseTargetIssueCode::InvalidMatrix, cell.targetId, "terminal",
                         "Rejected target cannot have an execution result.");
                invalidEvidence = true;
            }
            if (terminal.state == ReleaseTargetTerminalState::Succeeded && !terminal.candidateFinalVerified) {
                AddIssue(issues, ReleaseTargetIssueCode::InvalidMatrix, cell.targetId, "candidate",
                         "Successful target lacks a final-verified candidate.");
                invalidEvidence = true;
            }
            if (terminal.state != ReleaseTargetTerminalState::Succeeded && terminal.candidateFinalVerified) {
                AddIssue(issues, ReleaseTargetIssueCode::InvalidMatrix, cell.targetId, "candidate",
                         "Non-successful target cannot claim a successful candidate result.");
                invalidEvidence = true;
            }
        }
    }  // namespace

    /** @copydoc PlanReleaseTargetMatrix */
    ReleaseTargetMatrixPlan PlanReleaseTargetMatrix(std::string groupId, const ReleaseMachine host,
                                                    const std::span<const ReleaseMatrixCellRequest> cells,
                                                    const std::span<const ReleaseToolchainDescriptor> toolchains) {
        using enum ReleaseTargetIssueCode;
        ReleaseTargetMatrixPlan matrix{std::move(groupId), {}, {}};
        if (!IsValidDistributionIdentity(matrix.groupId) || cells.empty() || cells.size() > MaximumReleaseMatrixCells)
            AddIssue(matrix.issues, InvalidMatrix, {}, "group", "Group identity or member count is invalid.");
        if (!ValidMachine(host))
            AddIssue(matrix.issues, InvalidMatrix, {}, "host", "Host machine identity is invalid.");
        if (toolchains.size() > MaximumReleaseMatrixCells)
            AddIssue(matrix.issues, InvalidMatrix, {}, "toolchains", "Too many toolchains were supplied.");
        if (!matrix.issues.empty())
            return matrix;

        matrix.cells.reserve(cells.size());
        for (std::size_t index = 0; index < cells.size(); ++index) {
            matrix.cells.push_back(PlanCell(cells[index], host, cells.first(index), matrix.cells, toolchains));
        }
        return matrix;
    }

    /** @copydoc SummarizeReleaseTargetMatrix */
    ReleaseMatrixSummary SummarizeReleaseTargetMatrix(const ReleaseTargetMatrixPlan &matrix,
                                                      const std::span<const ReleaseTargetTerminal> terminals) {
        ReleaseMatrixSummary summary;
        summary.issues = matrix.issues;
        bool requiredFailed = !summary.issues.empty();
        bool pending = false;
        if (!IsValidDistributionIdentity(matrix.groupId) || matrix.cells.empty() || matrix.cells.size() > MaximumReleaseMatrixCells) {
            AddIssue(summary.issues, ReleaseTargetIssueCode::InvalidMatrix, {}, "group", "Group membership is invalid.");
            requiredFailed = true;
        }
        summary.members.reserve(matrix.cells.size());

        for (const ReleaseMatrixCellPlan &cell : matrix.cells) {
            ReleaseMatrixMemberResult member{cell.jobId, cell.targetId, cell.requirement,
                                             cell.plan ? ReleaseMatrixMemberState::Pending : ReleaseMatrixMemberState::ValidationFailed,
                                             std::nullopt};
            if (cell.plan.has_value() != cell.validatedTarget.has_value()) {
                AddIssue(summary.issues, ReleaseTargetIssueCode::InvalidMatrix, cell.targetId, "plan",
                         "Admitted target plan and validation evidence disagree.");
                requiredFailed = true;
            }
            const ReleaseTargetTerminal *matched = FindTerminal(matrix.groupId, cell, terminals, summary.issues, requiredFailed);
            if (matched)
                ApplyTerminal(cell, *matched, member, summary.issues, requiredFailed);
            if (cell.requirement == ReleaseMatrixRequirement::Required) {
                requiredFailed |= !cell.plan || (matched && matched->state != ReleaseTargetTerminalState::Succeeded);
            }
            pending |= cell.plan.has_value() && !matched;
            summary.members.push_back(std::move(member));
        }
        for (const ReleaseTargetTerminal &terminal : terminals) {
            if (terminal.groupId != matrix.groupId || std::ranges::none_of(matrix.cells, [&terminal](const ReleaseMatrixCellPlan &cell) {
                return cell.jobId == terminal.jobId && cell.targetId == terminal.targetId;
            })) {
                AddIssue(summary.issues, ReleaseTargetIssueCode::InvalidMatrix, terminal.targetId, "terminal",
                         "Terminal result does not belong to this group.");
                requiredFailed = true;
            }
        }
        if (requiredFailed)
            summary.state = ReleaseMatrixState::Failed;
        else if (pending)
            summary.state = ReleaseMatrixState::Incomplete;
        else
            summary.state = ReleaseMatrixState::Succeeded;
        return summary;
    }
}  // namespace Horo::Release
