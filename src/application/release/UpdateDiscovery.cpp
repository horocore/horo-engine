#include "Horo/Release/UpdateDiscovery.h"

#include "Horo/Release/UpdateDiscoveryErrors.h"
#include "Horo/Release/UpdateManifestErrors.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace Horo::Release {
    namespace {
        constexpr std::uint64_t MinimumCheckInterval = 15U * 60U;
        constexpr std::uint64_t MaximumCheckInterval = 30U * 24U * 60U * 60U;

        [[nodiscard]] bool ValidChannel(const UpdateChannel &channel) {
            using enum UpdateChannelKind;
            switch (channel.kind) {
                case Stable:
                case Preview:
                case Nightly:
                    return channel.sourceId.empty();
                case Enterprise:
                case Offline:
                    return IsValidDistributionIdentity(channel.sourceId);
            }
            return false;
        }

        [[nodiscard]] ReleaseSemanticVersion Version(const ReleaseProductVersion &version) {
            return std::visit([](const auto &value) {
                return value.value;
            }, version);
        }

        [[nodiscard]] bool TargetMatches(const UpdatePackageRecord &package, const UpdateAdmissionContext &context) {
            return package.selection.artifact.platform == context.platform &&
                   package.selection.artifact.architecture == context.architecture;
        }
    }  // namespace

    /** @copydoc PlanUpdateCheck */
    Result<UpdateCheckPlan> PlanUpdateCheck(const UpdateDiscoveryPolicy &policy, const UpdateCheckContext &context) {
        if (!ValidChannel(policy.selectedChannel) || !ValidChannel(context.installedChannel) || context.now == 0U ||
            policy.intervalSeconds < MinimumCheckInterval || policy.intervalSeconds > MaximumCheckInterval ||
            (policy.automaticDownloads && !policy.automaticChecks && !policy.mandatorySecurityChecks))
            return Result<UpdateCheckPlan>::Failure(MakeError(UpdateDiscoveryErrors::InvalidPolicy));
        if (policy.selectedChannel != context.installedChannel && !context.explicitChannelChange)
            return Result<UpdateCheckPlan>::Failure(MakeError(UpdateDiscoveryErrors::ChannelChangeRequiresAction));
        const bool manual = context.trigger == UpdateCheckTrigger::Manual;
        if (!manual && context.lastSuccessfulCheck.has_value() && *context.lastSuccessfulCheck > context.now)
            return Result<UpdateCheckPlan>::Failure(MakeError(UpdateDiscoveryErrors::ClockMovedBackward));
        const bool automation = policy.automaticChecks || policy.mandatorySecurityChecks;
        const bool due = !context.lastSuccessfulCheck.has_value() || (context.now >= *context.lastSuccessfulCheck &&
                                                                      context.now - *context.lastSuccessfulCheck >= policy.intervalSeconds);
        return Result<UpdateCheckPlan>::Success({manual || (automation && due), policy.automaticDownloads, policy.telemetryConsent});
    }

    /** @copydoc UpdateSourceUnavailable */
    UpdateDiscoveryResult UpdateSourceUnavailable(Error cause) {
        return {UpdateDiscoveryStatus::SourceUnavailable, std::nullopt, std::move(cause)};
    }

    /** @copydoc AssessUpdate */
    UpdateDiscoveryResult AssessUpdate(const SignedUpdateManifest &manifest, const UpdateAdmissionContext &context,
                                       const UpdateTrustRootSnapshot &roots, std::shared_ptr<const Security::SignatureProvider> provider,
                                       const UpdatePackagePreferences &preferences, const UpdateMetadataFreshnessPolicy freshness) {
        if (auto verified = VerifyUpdateManifest(manifest, context, roots, std::move(provider), freshness); verified.HasError())
            return {UpdateDiscoveryStatus::Rejected, std::nullopt, verified.ErrorValue()};
        if (CompareReleaseVersionPrecedence(Version(manifest.Data().version), Version(context.installedVersion)) == 0)
            return {UpdateDiscoveryStatus::UpToDate, std::nullopt, std::nullopt};
        for (const DistributionPackageFormat format : preferences.formats) {
            const auto found = std::ranges::find_if(manifest.Data().packages, [&](const UpdatePackageRecord &package) {
                return package.selection.format == format && TargetMatches(package, context);
            });
            if (found != manifest.Data().packages.end())
                return {UpdateDiscoveryStatus::Available, *found, std::nullopt};
        }
        return {UpdateDiscoveryStatus::NoCompatiblePackage, std::nullopt, MakeError(UpdateManifestErrors::Incompatible)};
    }
}  // namespace Horo::Release
