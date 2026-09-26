#pragma once

/**
 * @file ReleasePreflight.h
 * @brief Side-effect-free validation and immutable capture of release execution inputs.
 */

#include "Horo/Foundation/Sha256.h"
#include "Horo/Release/ReleaseProfile.h"
#include "Horo/Release/ReleaseVersion.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::Release {
    /** @brief Maximum credential handles accepted by one release plan. */
    inline constexpr std::size_t MaximumReleaseCredentialHandles = 64;

    /** @brief Build configuration selected by a release request. */
    enum class ReleaseBuildConfiguration : std::uint8_t {
        Debug,
        Development,
        Shipping
    };

    /** @brief Opaque identity resolved by a credential provider only when a stage needs it. */
    struct ReleaseCredentialHandle final {
        std::uint64_t value{};
        bool operator==(const ReleaseCredentialHandle &) const noexcept = default;
    };

    /** @brief Intent supplied to preflight; its values are copied into a successful plan. */
    struct ReleasePreflightRequest final {
        std::filesystem::path projectRoot;
        std::string projectId;
        ReleaseVersionAuthority version;
        EffectiveReleaseProfile profile;
        DistributionArchitecture architecture{DistributionArchitecture::X64};
        ReleaseBuildConfiguration configuration{ReleaseBuildConfiguration::Shipping};
        std::string toolchainId;
        std::filesystem::path outputRoot;
        std::uintmax_t requiredFreeBytes{};
        std::vector<ReleaseCredentialHandle> credentials;
        bool reproducible{};
    };

    /** @brief Read-only facts captured by the host for the exact request being validated. */
    struct ReleasePreflightFacts final {
        std::filesystem::path requestedProjectRoot; /**< Input path whose project facts were captured. */
        std::filesystem::path requestedOutputRoot;  /**< Input path whose output facts were captured. */
        std::filesystem::path canonicalProjectRoot;
        std::filesystem::path canonicalOutputRoot;
        bool projectReadable{};
        bool outputWritable{};
        bool outputExists{};
        std::optional<std::uintmax_t> availableBytes;
        DistributionPlatform hostPlatform{DistributionPlatform::Linux};
        bool targetSupported{};
        bool crossCompilerAvailable{};
        bool toolchainAvailable{};
        ReleaseVersionAuthority currentVersion;
        Sha256Digest sourceTreeDigest;
        Sha256Digest dependencyLockDigest;
        Sha256Digest profileDigest;
        Sha256Digest toolchainDigest;
        Sha256Digest policyDigest;
        std::vector<ReleaseCapabilityId> availableCapabilities;
        std::vector<ReleaseCredentialHandle> availableCredentials;
    };

    /** @brief Stable category of one independent preflight failure. */
    enum class ReleasePreflightIssueCode : std::uint8_t {
        InvalidRequest,
        ProjectUnavailable,
        SourceChanged,
        ProfileChanged,
        ToolchainUnavailable,
        TargetUnsupported,
        CrossCompilerUnavailable,
        OutputCollision,
        OutputUnavailable,
        InsufficientSpace,
        CredentialUnavailable,
        CapabilityUnavailable,
        InputChanged
    };

    /** @brief One field-specific failure retained alongside other safe independent failures. */
    struct ReleasePreflightIssue final {
        ReleasePreflightIssueCode code{ReleasePreflightIssueCode::InvalidRequest};
        std::string field;
        std::string message;
    };

    /** @brief Exact source, dependency, profile, toolchain and policy bytes frozen by preflight. */
    struct ReleaseFrozenIdentities final {
        Sha256Digest sourceTree;
        Sha256Digest dependencyLock;
        Sha256Digest profile;
        Sha256Digest toolchain;
        Sha256Digest policy;
        bool operator==(const ReleaseFrozenIdentities &) const noexcept = default;
    };

    struct ReleasePreflightOutcome;

    /** @brief Validated immutable value handed to later release stages. */
    class ReleaseExecutionPlan final {
    public:
        /** @brief Returns copied request values. @return Borrowed immutable release inputs. */
        [[nodiscard]] const ReleasePreflightRequest &Request() const noexcept;
        /** @brief Returns canonical read-only path identities. @return Borrowed canonical project and output roots. */
        [[nodiscard]] const std::filesystem::path &ProjectRoot() const noexcept;
        /** @brief Returns canonical output root. @return Borrowed canonical output root. */
        [[nodiscard]] const std::filesystem::path &OutputRoot() const noexcept;
        /** @brief Returns frozen input digests. @return Source, lock, profile, toolchain and policy identities. */
        [[nodiscard]] const ReleaseFrozenIdentities &Identities() const noexcept;
        /** @brief Formats a safe summary without credential identities. @return Human-readable release plan. */
        [[nodiscard]] std::string Summary() const;
        /** @brief Serializes the exact plan with opaque credential handles, never credential values. @return Canonical JSON. */
        [[nodiscard]] std::string SerializeCanonical() const;

    private:
        friend struct ReleasePreflightOutcome;
        friend ReleasePreflightOutcome PreflightRelease(const ReleasePreflightRequest &, const ReleasePreflightFacts &);
        /** @brief Constructs a plan only after complete preflight success. */
        ReleaseExecutionPlan(ReleasePreflightRequest request, std::filesystem::path projectRoot, std::filesystem::path outputRoot,
                             ReleaseFrozenIdentities identities);

        ReleasePreflightRequest request_;
        std::filesystem::path projectRoot_;
        std::filesystem::path outputRoot_;
        ReleaseFrozenIdentities identities_;
    };

    /** @brief Aggregated independent failures or one ready-to-execute immutable plan. */
    struct ReleasePreflightOutcome final {
        std::vector<ReleasePreflightIssue> issues;
        std::optional<ReleaseExecutionPlan> plan;
    };

    /**
     * @brief Validates read-only host facts and freezes one exact release target without mutating output.
     * @param request User or automation release intent.
     * @param facts Read-only observations captured for that intent by the host.
     * @return All safe independent issues, or exactly one immutable plan.
     */
    [[nodiscard]] ReleasePreflightOutcome PreflightRelease(const ReleasePreflightRequest &request, const ReleasePreflightFacts &facts);

    /**
     * @brief Rejects source, lock, profile, toolchain or policy drift before a later stage consumes a plan.
     * @param plan Frozen plan originally accepted by preflight.
     * @param current Fresh read-only host observations for the same request.
     * @return Every changed input identity; empty only when all frozen identities still match.
     */
    [[nodiscard]] std::vector<ReleasePreflightIssue> ValidateReleaseInputFreeze(const ReleaseExecutionPlan &plan,
                                                                                const ReleasePreflightFacts &current);
}  // namespace Horo::Release
