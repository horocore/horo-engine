#include "Horo/Application/PlatformServicesAuthoring.h"
#include "Horo/Application/PlatformServicesDefinitionCodec.h"

#include <array>
#include <nlohmann/json.hpp>

namespace Horo::Application {
    using namespace PlatformServices;

    /** @copydoc PreparePlatformServicesInitialDocuments */
    Result<PlatformServicesInitialDocuments> PreparePlatformServicesInitialDocuments(const std::string_view projectSource,
                                                                                     const PlatformServicesIdSalt &salt) {
        using Output = Result<PlatformServicesInitialDocuments>;
        auto root = DecodeProjectSourceDocument(projectSource);
        if (root.HasError())
            return Output::Failure(std::move(root).ErrorValue());
        if (root.Value().platformServicesIdSaltSource || root.Value().platformServicesSettingsSource)
            return Output::Failure(
                MakeError(StableIdErrors::InvalidLedger, "Initial platform documents cannot replace an existing root salt or policy."));
        auto ids = BuildPlatformStableIdRegistry({.projectId = root.Value().metadata.projectId, .salt = salt});
        if (ids.HasError())
            return Output::Failure(std::move(ids).ErrorValue());
        auto achievements = BuildAchievementDefinitionRegistry(ids.Value(), {.stableIdRegistryFingerprint = ids.Value().Fingerprint()});
        if (achievements.HasError())
            return Output::Failure(std::move(achievements).ErrorValue());
        auto stats = BuildStatDefinitionRegistry(ids.Value(), {.stableIdRegistryFingerprint = ids.Value().Fingerprint()});
        if (stats.HasError())
            return Output::Failure(std::move(stats).ErrorValue());
        auto leaderboards =
            BuildLeaderboardDefinitionRegistry(ids.Value(), stats.Value(), {.stableIdRegistryFingerprint = ids.Value().Fingerprint()});
        if (leaderboards.HasError())
            return Output::Failure(std::move(leaderboards).ErrorValue());
        auto presence = BuildPresenceDefinitionRegistry(ids.Value(), {.stableIdRegistryFingerprint = ids.Value().Fingerprint()});
        if (presence.HasError())
            return Output::Failure(std::move(presence).ErrorValue());
        auto policy =
            BuildPlatformProjectConfiguration({.projectId = root.Value().metadata.projectId, .profile = PlatformServicesHostProfile::Cook},
                                              {}, {});
        if (policy.HasError())
            return Output::Failure(std::move(policy).ErrorValue());
        auto settings = EncodeProjectPlatformServicesConfiguration(policy.Value());
        if (settings.HasError())
            return Output::Failure(std::move(settings).ErrorValue());
        std::array encoded{EncodePlatformServicesLedger(ids.Value()), EncodePlatformAchievementDefinitions(achievements.Value()),
                           EncodePlatformStatDefinitions(stats.Value()), EncodePlatformLeaderboardDefinitions(leaderboards.Value()),
                           EncodePlatformPresenceDefinitions(presence.Value())};
        for (auto &document : encoded)
            if (document.HasError())
                return Output::Failure(std::move(document).ErrorValue());
        // The strict decoder already admitted this bounded root. Preserve every unrelated authored value.
        auto metadata = nlohmann::ordered_json::parse(projectSource);
        metadata["platformServicesIdSalt"] = FormatPlatformServicesIdSalt(salt);
        metadata["settings"]["platformServices"] = nlohmann::ordered_json::parse(settings.Value());
        const auto projectText = metadata.dump(2) + "\n";
        auto verified = DecodeProjectSourceDocument(projectText);
        if (verified.HasError())
            return Output::Failure(std::move(verified).ErrorValue());
        return Output::Success({.projectSource = projectText,
                                .ledgerSource = std::move(encoded[0]).Value(),
                                .achievementSource = std::move(encoded[1]).Value(),
                                .statSource = std::move(encoded[2]).Value(),
                                .leaderboardSource = std::move(encoded[3]).Value(),
                                .presenceSource = std::move(encoded[4]).Value()});
    }
}  // namespace Horo::Application
