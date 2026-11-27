#pragma once

/** @file PlatformServicesAuthoringFixture.h @brief Shared typed source/semantic fixtures for ledger, policy and definition codec
 * regressions. */
#include "Horo/Application/PlatformServicesAuthoring.h"
#include "Horo/Application/PlatformServicesDefinitionCodec.h"
#include "Horo/PlatformServices/PlatformProviderManifestCook.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace Horo::Tests::PlatformAuthoring {
    using namespace Application;
    using namespace PlatformServices;

    /** @brief Builds a portable fixture with live identity and retained history of every kind. */
    inline PlatformStableIdRegistryCandidate Candidate() {
        PlatformStableIdRegistryCandidate candidate{.projectId = "project.portable"};
        candidate.salt.bytes.front() = std::byte{1};
        for (const auto kind : {PlatformServiceIdKind::Achievement, PlatformServiceIdKind::Leaderboard, PlatformServiceIdKind::Stat,
                                PlatformServiceIdKind::PresenceStatus}) {
            for (const bool tombstone : {false, true}) {
                const std::string key = tombstone ? "retired.key" : "active.key";
                candidate.entries.push_back({.kind = kind,
                                             .canonicalKey = key,
                                             .storedId = DerivePlatformServiceStableId(candidate.salt, kind, key).Value(),
                                             .aliases = {key + ".z", key + ".a"},
                                             .state = tombstone ? PlatformStableIdState::Tombstoned : PlatformStableIdState::Active,
                                             .removalProvenance = tombstone ? "removed:reviewed-history" : ""});
            }
        }
        return candidate;
    }

    /** @brief Encodes a fixture only after requiring the typed result. */
    inline std::string Encode(const PlatformStableIdRegistry &registry) {
        auto encoded = EncodePlatformServicesLedger(registry);
        REQUIRE(encoded.HasValue());
        return std::move(encoded).Value();
    }

    /** @brief Requires the exact stable machine-readable failure identity. */
    inline void CheckError(const auto &result, const ErrorCodeDescriptor &descriptor) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
    }

    /** @brief Requires an initialized policy before producing persisted project bytes. */
    inline std::string EncodePolicy(const PlatformProjectConfiguration &configuration) {
        auto encoded = EncodeProjectPlatformServicesConfiguration(configuration);
        REQUIRE(encoded.HasValue());
        return std::move(encoded).Value();
    }

    /** @brief Constructs a real project root using the registered current contract and explicit Null policy. */
    inline nlohmann::json Root() {
        const auto release = CurrentEngineReleaseVersion();
        const auto *decision = BuiltInReleaseCompatibilityRegistry().Find(release);
        REQUIRE(decision != nullptr);
        const auto salt = Candidate().salt;
        const auto configuration = BuildPlatformProjectConfiguration({.projectId = "project.portable"}, {}, {});
        REQUIRE(configuration.HasValue());
        return {{"horoVersion", FormatHoroVersion(release.value)},
                {"persistentContract", FormatPersistentContractHash(decision->persistentContract)},
                {"projectId", "project.portable"},
                {"name", "Platform authoring"},
                {"projectVersion", "1.0.0"},
                {"createdAt", "2026-11-11T00:00:00Z"},
                {"platformServicesIdSalt", FormatPlatformServicesIdSalt(salt)},
                {"settings",
                 {{"renderBackend", "null"}, {"platformServices", nlohmann::json::parse(EncodePolicy(configuration.Value()))}}}};
    }

    /** @brief Complete semantic source fixture used by codec and cook regressions. */
    struct Definitions {
        AchievementDefinitionRegistry achievements;
        StatDefinitionRegistry stats;
        LeaderboardDefinitionRegistry leaderboards;
        PresenceDefinitionRegistry presence;
    };

    /** @brief Builds semantic fixtures once through their existing owning validators. */
    inline Definitions Define(const PlatformStableIdRegistry &ids) {
        const auto stat = ids.ResolveStat("active.key").Value();
        const auto stats = BuildStatDefinitionRegistry(ids, {.stableIdRegistryFingerprint = ids.Fingerprint(),
                                                             .definitions = {{.id = stat,
                                                                              .authority = ProgressionAuthorityMode::LocalProduct,
                                                                              .valueKind = ProgressionValueKind::SignedInteger64,
                                                                              .range = {0, 100},
                                                                              .mutation = StatMutationPolicy::SetMaximum,
                                                                              .localizationKey = "stat.active"}}});
        REQUIRE(stats.HasValue());
        const auto achievements =
            BuildAchievementDefinitionRegistry(ids, {.stableIdRegistryFingerprint = ids.Fingerprint(),
                                                     .definitions = {{.id = ids.ResolveAchievement("active.key").Value(),
                                                                      .authority = ProgressionAuthorityMode::LocalProduct,
                                                                      .progress = {.kind = AchievementProgressKind::UnlockOnce, .total = 1},
                                                                      .presentation = {.titleLocalizationKey = "achievement.active.title",
                                                                                       .descriptionLocalizationKey =
                                                                                           "achievement.active.description"}}}});
        REQUIRE(achievements.HasValue());
        const auto leaderboards = BuildLeaderboardDefinitionRegistry(ids, stats.Value(),
                                                                     {.stableIdRegistryFingerprint = ids.Fingerprint(),
                                                                      .definitions = {{.id = ids.ResolveLeaderboard("active.key").Value(),
                                                                                       .authority = ProgressionAuthorityMode::LocalProduct,
                                                                                       .valueKind = ProgressionValueKind::SignedInteger64,
                                                                                       .range = {0, 100},
                                                                                       .ordering = LeaderboardOrdering::HighestFirst,
                                                                                       .sourceStat = stat,
                                                                                       .localizationKey = "leaderboard.active"}}});
        REQUIRE(leaderboards.HasValue());
        const auto presence = BuildPresenceDefinitionRegistry(ids, {.stableIdRegistryFingerprint = ids.Fingerprint(),
                                                                    .definitions = {{.id = ids.ResolvePresenceStatus("active.key").Value(),
                                                                                     .detailPolicy = PresenceDetailPolicy::Forbidden,
                                                                                     .maximumDetailUtf8Bytes = 0,
                                                                                     .localizationKey = "presence.active"}}});
        REQUIRE(presence.HasValue());
        return {achievements.Value(), stats.Value(), leaderboards.Value(), presence.Value()};
    }

    /** @brief Reloads all semantic source documents before cooking, retaining exact semantic fingerprints. */
    inline PlatformProviderManifestCookOutput Cook(const PlatformStableIdRegistry &ids, const PlatformProjectConfiguration &configuration) {
        const auto original = Define(ids);
        const auto achievementBytes = EncodePlatformAchievementDefinitions(original.achievements);
        const auto statBytes = EncodePlatformStatDefinitions(original.stats);
        const auto leaderboardBytes = EncodePlatformLeaderboardDefinitions(original.leaderboards);
        const auto presenceBytes = EncodePlatformPresenceDefinitions(original.presence);
        REQUIRE(achievementBytes.HasValue());
        REQUIRE(statBytes.HasValue());
        REQUIRE(leaderboardBytes.HasValue());
        REQUIRE(presenceBytes.HasValue());
        const auto achievements = DecodePlatformAchievementDefinitions(achievementBytes.Value(), ids);
        const auto stats = DecodePlatformStatDefinitions(statBytes.Value(), ids);
        REQUIRE(achievements.HasValue());
        REQUIRE(stats.HasValue());
        const auto leaderboards = DecodePlatformLeaderboardDefinitions(leaderboardBytes.Value(), ids, stats.Value());
        const auto presence = DecodePlatformPresenceDefinitions(presenceBytes.Value(), ids);
        REQUIRE(leaderboards.HasValue());
        REQUIRE(presence.HasValue());
        CHECK(achievements.Value().Fingerprint() == original.achievements.Fingerprint());
        CHECK(stats.Value().Fingerprint() == original.stats.Fingerprint());
        CHECK(leaderboards.Value().Fingerprint() == original.leaderboards.Fingerprint());
        CHECK(presence.Value().Fingerprint() == original.presence.Fingerprint());
        CHECK(EncodePlatformAchievementDefinitions(achievements.Value()).Value() == achievementBytes.Value());
        CHECK(EncodePlatformStatDefinitions(stats.Value()).Value() == statBytes.Value());
        CHECK(EncodePlatformLeaderboardDefinitions(leaderboards.Value()).Value() == leaderboardBytes.Value());
        CHECK(EncodePlatformPresenceDefinitions(presence.Value()).Value() == presenceBytes.Value());
        auto cooked = CookPlatformProviderManifest({.stableIds = ids,
                                                    .achievements = achievements.Value(),
                                                    .leaderboards = leaderboards.Value(),
                                                    .stats = stats.Value(),
                                                    .presence = presence.Value(),
                                                    .configuration = configuration});
        REQUIRE(cooked.HasValue());
        return std::move(cooked).Value();
    }
}  // namespace Horo::Tests::PlatformAuthoring
