#include "Horo/PlatformServices/PlatformProviderManifestCook.h"
#include "PlatformDefinitionTestAssertions.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <stop_token>
#include <string>
#include <utility>

namespace Horo::PlatformServices {
    using TestAssertions::CheckError;
    using TestAssertions::CheckFieldError;

    namespace {
        [[nodiscard]] PlatformServicesIdSalt Salt() {
            PlatformServicesIdSalt salt;
            salt.bytes.front() = std::byte{1};
            return salt;
        }

        [[nodiscard]] PlatformStableIdDeclaration Entry(const PlatformServiceIdKind kind, std::string key,
                                                        const PlatformStableIdState state = PlatformStableIdState::Active) {
            const auto derived = DerivePlatformServiceStableId(Salt(), kind, key);
            REQUIRE(derived.HasValue());
            return {.kind = kind,
                    .canonicalKey = std::move(key),
                    .storedId = derived.Value(),
                    .state = state,
                    .removalProvenance = state == PlatformStableIdState::Tombstoned ? "retired" : ""};
        }

        template <typename T> [[nodiscard]] T Unwrap(Result<T> result) {
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        [[nodiscard]] PlatformProjectConfiguration BuildConfiguration(const bool exact, std::string projectId = "project.cook") {
            PlatformProjectConfigurationCandidate candidate{.projectId = std::move(projectId),
                                                            .profile = PlatformServicesHostProfile::Cook};
            if (!exact)
                return Unwrap(BuildPlatformProjectConfiguration(candidate, {}, {}));
            candidate.provider = {.mode = PlatformProviderSelectionMode::ExactProvider, .providerKey = "platform.provider.mock"};
            candidate.services.fill(PlatformServiceRequirement::Optional);
            candidate.services[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = PlatformServiceRequirement::Required;
            PlatformProviderModuleContribution contribution{
                .module = {"horo.platform.mock"},
                .providerKey = "platform.provider.mock",
                .provider = {41},
                .interfaceVersion = {PlatformServicesBackendInterfaceMajor, PlatformServicesBackendInterfaceMinor},
                .allowedProfiles = PlatformServicesHostProfileMask::Cook,
            };
            contribution.supportedServices.fill(true);
            const ModuleId trusted = contribution.module;
            return Unwrap(BuildPlatformProjectConfiguration(candidate, std::span{&contribution, 1}, std::span{&trusted, 1}));
        }

        struct Fixture final {
            PlatformStableIdDeclaration achievement{Entry(PlatformServiceIdKind::Achievement, "campaign.win")};
            PlatformStableIdDeclaration leaderboard{Entry(PlatformServiceIdKind::Leaderboard, "ranked.score")};
            PlatformStableIdDeclaration stat{Entry(PlatformServiceIdKind::Stat, "stats.score")};
            PlatformStableIdDeclaration presence{Entry(PlatformServiceIdKind::PresenceStatus, "presence.online")};
            PlatformStableIdDeclaration retired{
                Entry(PlatformServiceIdKind::Achievement, "campaign.retired", PlatformStableIdState::Tombstoned)};
            PlatformStableIdRegistry ids{Unwrap(BuildPlatformStableIdRegistry(
                {.projectId = "project.cook", .salt = Salt(), .entries = {presence, stat, leaderboard, retired, achievement}}))};
            StatDefinitionRegistry stats{
                Unwrap(BuildStatDefinitionRegistry(ids, {.stableIdRegistryFingerprint = ids.Fingerprint(),
                                                         .definitions = {{.id = StatId{stat.storedId.value},
                                                                          .authority = ProgressionAuthorityMode::LocalProduct,
                                                                          .valueKind = ProgressionValueKind::SignedInteger64,
                                                                          .range = {-10, 100},
                                                                          .mutation = StatMutationPolicy::SetMaximum,
                                                                          .localizationKey = "stats.score"}}}))};
            AchievementDefinitionRegistry achievements{Unwrap(
                BuildAchievementDefinitionRegistry(ids,
                                                   {.stableIdRegistryFingerprint = ids.Fingerprint(),
                                                    .definitions = {{.id = AchievementId{achievement.storedId.value},
                                                                     .authority = ProgressionAuthorityMode::LocalProduct,
                                                                     .progress = {.kind = AchievementProgressKind::UnlockOnce, .total = 1},
                                                                     .presentation = {.titleLocalizationKey = "achievement.win.title",
                                                                                      .descriptionLocalizationKey =
                                                                                          "achievement.win.description"}}}}))};
            LeaderboardDefinitionRegistry leaderboards{
                Unwrap(BuildLeaderboardDefinitionRegistry(ids, stats,
                                                          {.stableIdRegistryFingerprint = ids.Fingerprint(),
                                                           .definitions = {{.id = LeaderboardId{leaderboard.storedId.value},
                                                                            .authority = ProgressionAuthorityMode::AuthorityServer,
                                                                            .valueKind = ProgressionValueKind::SignedInteger64,
                                                                            .range = {0, 100},
                                                                            .ordering = LeaderboardOrdering::HighestFirst,
                                                                            .sourceStat = StatId{stat.storedId.value},
                                                                            .localizationKey = "ranked.score"}}}))};
            PresenceDefinitionRegistry statuses{
                Unwrap(BuildPresenceDefinitionRegistry(ids, {.stableIdRegistryFingerprint = ids.Fingerprint(),
                                                             .definitions = {{.id = PresenceStatusId{presence.storedId.value},
                                                                              .detailPolicy = PresenceDetailPolicy::Optional,
                                                                              .maximumDetailUtf8Bytes = 32,
                                                                              .localizationKey = "presence.online"}}}))};
            PlatformProjectConfiguration config{BuildConfiguration(true)};
            std::vector<PlatformCookMapping> mappings;

            Fixture() {
                const std::array entries{achievement, leaderboard, stat, presence};
                std::uint8_t seed = 1;
                for (const auto &entry : entries) {
                    Sha256Digest digest;
                    digest.bytes.front() = seed++;
                    mappings.push_back({.evidence = {.provider = PlatformProviderId{41},
                                                     .kind = entry.kind,
                                                     .id = entry.storedId,
                                                     .providerValueDigest = digest,
                                                     .registryFingerprint = ids.Fingerprint(),
                                                     .mappingRevision = 7},
                                        .source = "mappings.json:" + entry.canonicalKey});
                }
            }

            [[nodiscard]] PlatformProviderManifestCookInput Input(std::stop_token cancellation = {}) const {
                return {.stableIds = ids,
                        .achievements = achievements,
                        .leaderboards = leaderboards,
                        .stats = stats,
                        .presence = statuses,
                        .configuration = config,
                        .mappingPolicy = {.requiredKinds = {true, false, false, false}},
                        .mappingRevision = 7,
                        .mappings = mappings,
                        .cancellation = cancellation};
            }
        };
    }  // namespace

    TEST_CASE("Paired cook encoding is the existing preimage and rejects malformed streams", "[platform-services][manifest-cook]") {
        Fixture fixture;
        const auto cooked = CookPlatformProviderManifest(fixture.Input());
        REQUIRE(cooked.HasValue());
        auto encoded = EncodePlatformProviderManifestCookGeneration(cooked.Value());
        REQUIRE(encoded.HasValue());
        auto bytes = std::move(encoded).Value();
        CHECK(ComputeSha256(bytes) == cooked.Value().generationFingerprint);
        const auto decoded = DecodePlatformProviderManifestCookGeneration(bytes, cooked.Value().generationFingerprint);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().neutralBytes == cooked.Value().neutralBytes);
        CHECK(decoded.Value().mappingBytes == cooked.Value().mappingBytes);
        const auto wrongFingerprint = DecodePlatformProviderManifestCookGeneration(bytes, Sha256Digest{});
        REQUIRE(wrongFingerprint.HasError());
        CHECK(wrongFingerprint.ErrorValue().code.Value() == PlatformProviderManifestCookErrors::StaleGeneration.code.Value());
        SECTION("Trailing bytes cannot be admitted even under their own hash") {
            bytes.push_back(std::byte{});
        }
        SECTION("Truncated second stream") {
            bytes.pop_back();
        }
        SECTION("Unknown framing domain") {
            bytes[4] ^= std::byte{1};
        }
        const auto malformed = DecodePlatformProviderManifestCookGeneration(bytes, ComputeSha256(bytes));
        REQUIRE(malformed.HasError());
        CHECK(malformed.ErrorValue().code.Value() == PlatformProviderManifestCookErrors::InvalidInput.code.Value());
        auto changed = cooked.Value();
        changed.mappingBytes.back() ^= std::byte{1};
        CHECK(EncodePlatformProviderManifestCookGeneration(changed).HasError());
    }

    TEST_CASE("Paired cook framing rejects empty output and the finite encoded-byte ceiling", "[platform-services][manifest-cook]") {
        CHECK(EncodePlatformProviderManifestCookGeneration({}).HasError());
        const std::vector<std::byte> oversized(PlatformProviderManifestCookGenerationMaximumBytes + 1U);
        const auto decoded = DecodePlatformProviderManifestCookGeneration(oversized, {});
        REQUIRE(decoded.HasError());
        CHECK(decoded.ErrorValue().code.Value() == PlatformProviderManifestCookErrors::InvalidInput.code.Value());
    }

    TEST_CASE("Host cancellation reaches the shared cook without a watcher thread", "[platform-services][manifest-cook]") {
        Fixture fixture;
        CancellationSource cancellation;
        auto input = fixture.Input();
        input.hostCancellation = cancellation.Token();
        cancellation.RequestCancellation();
        const auto cooked = CookPlatformProviderManifest(input);
        REQUIRE(cooked.HasError());
        CHECK(cooked.ErrorValue().code.Value() == PlatformProviderManifestCookErrors::Cancelled.code.Value());
    }

    TEST_CASE("Provider manifest cook is byte-stable for equivalent mapping order", "[platform-services][manifest-cook]") {
        Fixture fixture;
        const auto first = CookPlatformProviderManifest(fixture.Input());
        REQUIRE(first.HasValue());
        std::ranges::reverse(fixture.mappings);
        const auto reordered = CookPlatformProviderManifest(fixture.Input());
        REQUIRE(reordered.HasValue());
        CHECK(first.Value().neutralBytes == reordered.Value().neutralBytes);
        CHECK(first.Value().mappingBytes == reordered.Value().mappingBytes);
        CHECK(first.Value().generationFingerprint == reordered.Value().generationFingerprint);
        REQUIRE_FALSE(first.Value().neutralBytes.empty());
        REQUIRE_FALSE(first.Value().mappingBytes.empty());
        for (auto &row : fixture.mappings)
            row.evidence.mappingRevision = 8;
        auto revised = fixture.Input();
        revised.mappingRevision = 8;
        const auto nextGeneration = CookPlatformProviderManifest(revised);
        REQUIRE(nextGeneration.HasValue());
        CHECK(first.Value().neutralBytes == nextGeneration.Value().neutralBytes);
        CHECK(first.Value().mappingBytes != nextGeneration.Value().mappingBytes);
        CHECK(first.Value().generationFingerprint != nextGeneration.Value().generationFingerprint);
        fixture.mappings.front().evidence.mappingRevision = 8;
        fixture.mappings.back().evidence.mappingRevision = 7;
        CheckFieldError(CookPlatformProviderManifest(fixture.Input()), PlatformProviderManifestCookErrors::StaleGeneration,
                        fixture.mappings.front().source);
    }

    TEST_CASE("Cook output is independent of diagnostic paths across checkout locations", "[platform-services][manifest-cook]") {
        Fixture fixture;
        const auto original = CookPlatformProviderManifest(fixture.Input());
        REQUIRE(original.HasValue());
        for (auto &mapping : fixture.mappings)
            mapping.source = "C:/project data/\xc4\xb0stanbul/private-provider-fixture.mapping";
        const auto relocated = CookPlatformProviderManifest(fixture.Input());
        REQUIRE(relocated.HasValue());
        CHECK(relocated.Value().neutralBytes == original.Value().neutralBytes);
        CHECK(relocated.Value().mappingBytes == original.Value().mappingBytes);
        CHECK(relocated.Value().generationFingerprint == original.Value().generationFingerprint);
    }

    TEST_CASE("Cook reports missing mapping at stable source and rejects invalid mapping rows", "[platform-services][manifest-cook]") {
        Fixture fixture;
        fixture.mappings.erase(fixture.mappings.begin());
        CheckFieldError(CookPlatformProviderManifest(fixture.Input()), PlatformProviderManifestCookErrors::MissingMapping,
                        "platform_services.ids.json:1:campaign.win");
        Fixture invalid;
        invalid.mappings.front().evidence.id = invalid.retired.storedId;
        CheckFieldError(CookPlatformProviderManifest(invalid.Input()), PlatformProviderManifestCookErrors::InvalidMapping,
                        invalid.mappings.front().source);
        Fixture invalidEncoding;
        invalidEncoding.mappings.front().source = std::string(1, static_cast<char>(0xff));
        CheckFieldError(CookPlatformProviderManifest(invalidEncoding.Input()), PlatformProviderManifestCookErrors::InvalidInput,
                        "mappings.source");
        Fixture duplicate;
        duplicate.mappings.push_back(duplicate.mappings.front());
        CheckError(CookPlatformProviderManifest(duplicate.Input()), PlatformProviderManifestCookErrors::InvalidMapping);
        Fixture optionalOmission;
        optionalOmission.mappings.resize(1);
        CHECK(CookPlatformProviderManifest(optionalOmission.Input()).HasValue());
    }

    TEST_CASE("Cook fails closed for stale project, cancellation and explicit Null", "[platform-services][manifest-cook]") {
        Fixture fixture;
        const auto otherConfiguration = BuildConfiguration(true, "project.other");
        const PlatformProviderManifestCookInput stale{.stableIds = fixture.ids,
                                                      .achievements = fixture.achievements,
                                                      .leaderboards = fixture.leaderboards,
                                                      .stats = fixture.stats,
                                                      .presence = fixture.statuses,
                                                      .configuration = otherConfiguration,
                                                      .mappingPolicy = {.requiredKinds = {true, false, false, false}},
                                                      .mappingRevision = 7,
                                                      .mappings = fixture.mappings};
        CheckError(CookPlatformProviderManifest(stale), PlatformProviderManifestCookErrors::StaleGeneration);
        std::stop_source source;
        source.request_stop();
        CheckError(CookPlatformProviderManifest(fixture.Input(source.get_token())), PlatformProviderManifestCookErrors::Cancelled);

        fixture.config = BuildConfiguration(false);
        fixture.mappings.clear();
        PlatformProjectConfigurationCandidate requiredNull{.projectId = "project.cook", .profile = PlatformServicesHostProfile::Cook};
        requiredNull.services[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = PlatformServiceRequirement::Required;
        CheckError(BuildPlatformProjectConfiguration(requiredNull, {}, {}),
                   PlatformProjectConfigurationErrors::RequiredCapabilityUnsupported);
        auto nullInput = fixture.Input();
        nullInput.mappingPolicy.requiredKinds = {};
        nullInput.mappingRevision = 0;
        const auto cookedNull = CookPlatformProviderManifest(nullInput);
        REQUIRE(cookedNull.HasValue());
        nullInput.mappingPolicy.requiredKinds.front() = true;
        CheckError(CookPlatformProviderManifest(nullInput), PlatformProviderManifestCookErrors::InvalidInput);
        nullInput.mappingPolicy.requiredKinds = {};
        nullInput.mappingRevision = 7;
        CheckError(CookPlatformProviderManifest(nullInput), PlatformProviderManifestCookErrors::InvalidInput);
    }
}  // namespace Horo::PlatformServices
