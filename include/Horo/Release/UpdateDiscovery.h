#pragma once

/**
 * @file UpdateDiscovery.h
 * @brief Update check policy and verified package selection without transport side effects.
 */

#include "Horo/Release/UpdateManifest.h"
#include "Horo/Release/UpdateTrustRoot.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::Release {
    /** @brief Stable channel families; enterprise and offline sources carry an explicit source ID. */
    enum class UpdateChannelKind : std::uint8_t {
        Stable,
        Preview,
        Nightly,
        Enterprise,
        Offline
    };

    /** @brief One selected update channel and optional managed source identity. */
    struct UpdateChannel final {
        UpdateChannelKind kind{UpdateChannelKind::Stable};
        std::string sourceId;
        bool operator==(const UpdateChannel &) const noexcept = default;
    };

    /** @brief Persistent user/administrator policy for checks and downloads. */
    struct UpdateDiscoveryPolicy final {
        UpdateChannel selectedChannel;
        std::uint64_t intervalSeconds{6U * 60U * 60U};
        bool automaticChecks{true};
        bool automaticDownloads{};
        bool mandatorySecurityChecks{};
        bool telemetryConsent{};
    };

    /** @brief Why a host asks to schedule one nonblocking discovery job. */
    enum class UpdateCheckTrigger : std::uint8_t {
        Startup,
        Scheduled,
        Manual
    };

    /** @brief Exact host facts used for a side-effect-free scheduling decision. */
    struct UpdateCheckContext final {
        UpdateChannel installedChannel;
        std::uint64_t now{}; /**< Trusted host Unix seconds. */
        std::optional<std::uint64_t> lastSuccessfulCheck;
        UpdateCheckTrigger trigger{UpdateCheckTrigger::Startup};
        bool explicitChannelChange{};
    };

    /** @brief Pure plan; the host queues work off the startup thread when schedule is true. */
    struct UpdateCheckPlan final {
        bool schedule{};
        bool allowAutomaticDownload{};
        bool allowTelemetry{};
    };

    /**
     * @brief Validates channel and check policy without network, disk or job-system work.
     * @param policy Selected channel and automation policy.
     * @param context Current installed channel, check history and trigger.
     * @return Scheduling plan or a typed policy error.
     */
    [[nodiscard]] Result<UpdateCheckPlan> PlanUpdateCheck(const UpdateDiscoveryPolicy &policy, const UpdateCheckContext &context);

    /** @brief Read-only discovery outcome; an available package is always signed and target compatible. */
    enum class UpdateDiscoveryStatus : std::uint8_t {
        Available,
        UpToDate,
        NoCompatiblePackage,
        SourceUnavailable,
        Rejected
    };

    /** @brief Exact preference order among host-supported package formats. */
    struct UpdatePackagePreferences final {
        std::vector<DistributionPackageFormat> formats;
    };

    /** @brief User-visible result that never mutates the installed product. */
    struct UpdateDiscoveryResult final {
        UpdateDiscoveryStatus status{UpdateDiscoveryStatus::Rejected};
        std::optional<UpdatePackageRecord> package;
        std::optional<Error> failure;
    };

    /**
     * @brief Projects a failed metadata fetch without changing the current installation.
     * @param cause Preserved source error for actionable diagnostics.
     * @return Explicit unavailable result with no package.
     */
    [[nodiscard]] UpdateDiscoveryResult UpdateSourceUnavailable(Error cause);

    /**
     * @brief Authenticates a fetched manifest and selects one exact supported target package.
     * @param manifest Complete canonical signed metadata from a source adapter.
     * @param context Installed product, channel, target and monotonic state.
     * @param roots Installed versioned trust roots.
     * @param provider Signature provider; null fails closed.
     * @param preferences Ordered host-supported package formats.
     * @param freshness Explicit bounded offline manifest-expiry policy; default requires fresh metadata.
     * @return Availability without any download or installation side effect.
     */
    [[nodiscard]] UpdateDiscoveryResult AssessUpdate(const SignedUpdateManifest &manifest, const UpdateAdmissionContext &context,
                                                     const UpdateTrustRootSnapshot &roots,
                                                     std::shared_ptr<const Security::SignatureProvider> provider,
                                                     const UpdatePackagePreferences &preferences,
                                                     UpdateMetadataFreshnessPolicy freshness = {});
}  // namespace Horo::Release
