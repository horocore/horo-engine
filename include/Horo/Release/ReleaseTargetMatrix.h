#pragma once

/**
 * @file ReleaseTargetMatrix.h
 * @brief Typed release target admission and complete matrix-result aggregation.
 */

#include "Horo/Release/ReleasePreflight.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::Release {
    /** @brief Maximum number of independently planned targets in one release group. */
    inline constexpr std::size_t MaximumReleaseMatrixCells = 64;

    /** @brief Exact host or target operating-system and CPU tuple. */
    struct ReleaseMachine final {
        DistributionPlatform platform{DistributionPlatform::Linux};
        DistributionArchitecture architecture{DistributionArchitecture::X64};
        bool operator==(const ReleaseMachine &) const noexcept = default;
    };

    /** @brief Three-part target platform version, ordered numerically. */
    struct ReleasePlatformVersion final {
        std::uint32_t major{};
        std::uint32_t minor{};
        std::uint32_t patch{};
        auto operator<=>(const ReleasePlatformVersion &) const noexcept = default;
    };

    /** @brief Validated SDK installation selected by a toolchain for its target OS. */
    struct ReleaseSdkDescriptor final {
        std::string id;
        DistributionPlatform platform{DistributionPlatform::Linux};
        ReleasePlatformVersion version;
        ReleasePlatformVersion oldestSupportedPlatform;
        ReleasePlatformVersion newestSupportedPlatform;
        bool available{};
    };

    /** @brief Read-only toolchain evidence captured by the host after compile/link validation. */
    struct ReleaseToolchainDescriptor final {
        std::string id;
        Sha256Digest digest;
        ReleaseMachine host;
        ReleaseMachine target;
        ReleasePlatformVersion oldestSupportedPlatform;
        ReleasePlatformVersion newestSupportedPlatform;
        ReleaseSdkDescriptor sdk;
        std::vector<DistributionPackageFormat> packageFormats;
        bool enabled{};
        bool compileAndLinkValidated{};
        bool explicitCrossToolchain{}; /**< True only for a user-configured cross-toolchain profile. */
    };

    /** @brief Immutable requirement assigned to one member of a release group. */
    enum class ReleaseMatrixRequirement : std::uint8_t {
        Required,
        Optional
    };

    /** @brief One target's requested policy and matching preflight observations. */
    struct ReleaseMatrixCellRequest final {
        std::string jobId; /**< Service-assigned identity for this independent target job. */
        std::string targetId;
        ReleaseMatrixRequirement requirement{ReleaseMatrixRequirement::Required};
        ReleasePreflightRequest request;
        ReleasePreflightFacts facts;
        ReleasePlatformVersion minimumPlatform;
        std::string sdkId;
    };

    /** @brief Stable reason a matrix cell cannot be admitted before execution. */
    enum class ReleaseTargetIssueCode : std::uint8_t {
        InvalidMatrix,
        InvalidTarget,
        ToolchainMissing,
        ToolchainAmbiguous,
        ToolchainMismatch,
        CrossToolchainRequired,
        UnsupportedHostTarget,
        UnsupportedArchitecture,
        UnsupportedPlatformVersion,
        SdkUnavailable,
        PackageFormatUnsupported,
        PreflightFailed
    };

    /** @brief One field-specific target admission failure. */
    struct ReleaseTargetIssue final {
        ReleaseTargetIssueCode code{ReleaseTargetIssueCode::InvalidTarget};
        std::string targetId;
        std::string field;
        std::string message;
    };

    /** @brief Validated target facts consumed with the frozen execution plan by one job. */
    struct ReleaseValidatedTarget final {
        ReleaseMachine machine;
        ReleasePlatformVersion minimumPlatform;
        std::string sdkId;
        ReleasePlatformVersion sdkVersion;
        ReleasePlatformVersion sdkOldestSupportedPlatform;
        ReleasePlatformVersion sdkNewestSupportedPlatform;
        DistributionPackageFormat packageFormat{DistributionPackageFormat::ZipArchive};
        DistributionPackageCapabilities packageCapabilities;
        Sha256Digest toolchainDigest;
    };

    /** @brief One independent admitted plan or its terminal validation failures. */
    struct ReleaseMatrixCellPlan final {
        std::string jobId;
        std::string targetId;
        ReleaseMatrixRequirement requirement{ReleaseMatrixRequirement::Required};
        std::optional<ReleaseExecutionPlan> plan;
        std::optional<ReleaseValidatedTarget> validatedTarget;
        std::vector<ReleaseTargetIssue> issues;
    };

    /** @brief Complete immutable group membership with independent cell plans. */
    struct ReleaseTargetMatrixPlan final {
        std::string groupId;
        std::vector<ReleaseMatrixCellPlan> cells;
        std::vector<ReleaseTargetIssue> issues;
    };

    /**
     * @brief Validates every target independently against exact host, toolchain, SDK and package evidence.
     * @param groupId Service-owned group identity; empty or invalid identities are rejected.
     * @param host Actual machine on which toolchains were observed.
     * @param cells Complete bounded target membership, including required and optional cells.
     * @param toolchains Read-only installed toolchain descriptors; duplicate matching IDs are rejected.
     * @return Complete per-cell plans and all safe admission failures; performs no build or output mutation.
     */
    [[nodiscard]] ReleaseTargetMatrixPlan PlanReleaseTargetMatrix(std::string groupId, ReleaseMachine host,
                                                                  std::span<const ReleaseMatrixCellRequest> cells,
                                                                  std::span<const ReleaseToolchainDescriptor> toolchains);

    /** @brief Exactly one terminal outcome for an admitted target job. */
    enum class ReleaseTargetTerminalState : std::uint8_t {
        Succeeded,
        Failed,
        Cancelled
    };

    /** @brief Service-owned terminal result bound to one immutable group; success requires a final-verified candidate. */
    struct ReleaseTargetTerminal final {
        std::string groupId;
        std::string jobId;
        std::string targetId;
        ReleaseTargetTerminalState state{ReleaseTargetTerminalState::Failed};
        bool candidateFinalVerified{};
    };

    /** @brief Group result derived from its complete immutable membership. */
    enum class ReleaseMatrixState : std::uint8_t {
        Incomplete,
        Failed,
        Succeeded
    };

    /** @brief Observable outcome of one cell, including terminal validation rejection. */
    enum class ReleaseMatrixMemberState : std::uint8_t {
        Pending,
        ValidationFailed,
        Succeeded,
        Failed,
        Cancelled
    };

    /** @brief Preserves each member's validation or execution result beside the group decision. */
    struct ReleaseMatrixMemberResult final {
        std::string jobId;
        std::string targetId;
        ReleaseMatrixRequirement requirement{ReleaseMatrixRequirement::Required};
        ReleaseMatrixMemberState state{ReleaseMatrixMemberState::Pending};
        std::optional<ReleaseTargetTerminal> terminal;
    };

    /** @brief Derived candidate decision and full member evidence. */
    struct ReleaseMatrixSummary final {
        ReleaseMatrixState state{ReleaseMatrixState::Incomplete};
        std::vector<ReleaseMatrixMemberResult> members;
        std::vector<ReleaseTargetIssue> issues;
    };

    /**
     * @brief Derives one truthful candidate result without dropping failed or missing members.
     * @param matrix Complete group plan returned by PlanReleaseTargetMatrix.
     * @param terminals Latest terminal outcomes keyed by exact group, job and target identity.
     * @return Failed for any required failure or invalid evidence, incomplete while required work remains, otherwise succeeded.
     */
    [[nodiscard]] ReleaseMatrixSummary SummarizeReleaseTargetMatrix(const ReleaseTargetMatrixPlan &matrix,
                                                                    std::span<const ReleaseTargetTerminal> terminals);
}  // namespace Horo::Release
