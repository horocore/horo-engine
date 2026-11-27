#include "Horo/Application/PlatformServicesAuthoring.h"
#include "Horo/Application/PlatformServicesDefinitionCodec.h"

#include <array>
#include <nlohmann/json.hpp>

namespace Horo::Application {
    using namespace PlatformServices;

    namespace {
        /** @brief Complete validated empty service definitions for the first ledger generation. */
        struct EmptyDefinitions final {
            AchievementDefinitionRegistry achievements;
            StatDefinitionRegistry stats;
            LeaderboardDefinitionRegistry leaderboards;
            PresenceDefinitionRegistry presence;
        };

        /** @brief Validates the first semantic generation in dependency order before encoding any source document. */
        Result<EmptyDefinitions> BuildEmptyDefinitions(const PlatformStableIdRegistry &ids) {
            using Output = Result<EmptyDefinitions>;
            auto achievements = BuildAchievementDefinitionRegistry(ids, {.stableIdRegistryFingerprint = ids.Fingerprint()});
            if (achievements.HasError())
                return Output::Failure(std::move(achievements).ErrorValue());
            auto stats = BuildStatDefinitionRegistry(ids, {.stableIdRegistryFingerprint = ids.Fingerprint()});
            if (stats.HasError())
                return Output::Failure(std::move(stats).ErrorValue());
            auto leaderboards = BuildLeaderboardDefinitionRegistry(ids, stats.Value(), {.stableIdRegistryFingerprint = ids.Fingerprint()});
            if (leaderboards.HasError())
                return Output::Failure(std::move(leaderboards).ErrorValue());
            auto presence = BuildPresenceDefinitionRegistry(ids, {.stableIdRegistryFingerprint = ids.Fingerprint()});
            if (presence.HasError())
                return Output::Failure(std::move(presence).ErrorValue());
            return Output::Success(
                {std::move(achievements).Value(), std::move(stats).Value(), std::move(leaderboards).Value(), std::move(presence).Value()});
        }
    }  // namespace

    /** @copydoc PreparePlatformServicesProjectPolicyEdit */
    Result<std::string> PreparePlatformServicesProjectPolicyEdit(const std::string_view projectSource,
                                                                 const PlatformProjectConfiguration &configuration) {
        using Output = Result<std::string>;
        auto source = DecodeProjectSourceDocument(projectSource);
        if (source.HasError())
            return Output::Failure(std::move(source).ErrorValue());
        if (source.Value().metadata.projectId != configuration.ProjectId())
            return Output::Failure(MakeError(StableIdErrors::InvalidLedger));
        const auto existing = DecodeProjectPlatformServicesConfigurationCandidate(source.Value(), configuration.Profile());
        if (existing.HasError())
            return Output::Failure(existing.ErrorValue());
        auto encoded = EncodeProjectPlatformServicesConfiguration(configuration);
        if (encoded.HasError())
            return Output::Failure(std::move(encoded).ErrorValue());
        // The canonical decoder admitted the complete bounded root before this preserving projection.
        auto root = nlohmann::ordered_json::parse(projectSource);
        root["settings"]["platformServices"] = nlohmann::ordered_json::parse(encoded.Value());
        auto result = root.dump(2) + "\n";
        const auto checked = DecodeProjectSourceDocument(result);
        if (checked.HasError())
            return Output::Failure(checked.ErrorValue());
        return Output::Success(std::move(result));
    }

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
        auto empty = BuildEmptyDefinitions(ids.Value());
        if (empty.HasError())
            return Output::Failure(std::move(empty).ErrorValue());
        const auto &definitions = empty.Value();
        auto policy =
            BuildPlatformProjectConfiguration({.projectId = root.Value().metadata.projectId, .profile = PlatformServicesHostProfile::Cook},
                                              {}, {});
        if (policy.HasError())
            return Output::Failure(std::move(policy).ErrorValue());
        auto settings = EncodeProjectPlatformServicesConfiguration(policy.Value());
        if (settings.HasError())
            return Output::Failure(std::move(settings).ErrorValue());
        std::array encoded{EncodePlatformServicesLedger(ids.Value()), EncodePlatformAchievementDefinitions(definitions.achievements),
                           EncodePlatformStatDefinitions(definitions.stats), EncodePlatformLeaderboardDefinitions(definitions.leaderboards),
                           EncodePlatformPresenceDefinitions(definitions.presence)};
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
