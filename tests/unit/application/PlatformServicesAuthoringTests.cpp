#include "Horo/Application/PlatformServicesAuthoring.h"
#include "Horo/PlatformServices/PlatformProviderManifestCook.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace Horo::Application {
    using namespace PlatformServices;

    namespace {
        /** @brief Builds a portable fixture with live identity and retained history of every kind. */
        PlatformStableIdRegistryCandidate Candidate() {
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
        std::string Encode(const PlatformStableIdRegistry &registry) {
            auto encoded = EncodePlatformServicesLedger(registry);
            REQUIRE(encoded.HasValue());
            return std::move(encoded).Value();
        }

        /** @brief Requires the exact stable machine-readable failure identity. */
        void CheckError(const auto &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        /** @brief Requires an initialized policy before producing persisted project bytes. */
        std::string EncodePolicy(const PlatformProjectConfiguration &configuration) {
            auto encoded = EncodeProjectPlatformServicesConfiguration(configuration);
            REQUIRE(encoded.HasValue());
            return std::move(encoded).Value();
        }

        /** @brief Constructs a real project root using the registered current contract and explicit Null policy. */
        nlohmann::json Root() {
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

        /** @brief Composes real semantic registries from a captured ledger before the common Null cook. */
        PlatformProviderManifestCookOutput Cook(const PlatformStableIdRegistry &ids, const PlatformProjectConfiguration &configuration) {
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
                BuildAchievementDefinitionRegistry(ids,
                                                   {.stableIdRegistryFingerprint = ids.Fingerprint(),
                                                    .definitions = {{.id = ids.ResolveAchievement("active.key").Value(),
                                                                     .authority = ProgressionAuthorityMode::LocalProduct,
                                                                     .progress = {.kind = AchievementProgressKind::UnlockOnce, .total = 1},
                                                                     .presentation = {.titleLocalizationKey = "achievement.active.title",
                                                                                      .descriptionLocalizationKey =
                                                                                          "achievement.active.description"}}}});
            REQUIRE(achievements.HasValue());
            const auto leaderboards =
                BuildLeaderboardDefinitionRegistry(ids, stats.Value(),
                                                   {.stableIdRegistryFingerprint = ids.Fingerprint(),
                                                    .definitions = {{.id = ids.ResolveLeaderboard("active.key").Value(),
                                                                     .authority = ProgressionAuthorityMode::LocalProduct,
                                                                     .valueKind = ProgressionValueKind::SignedInteger64,
                                                                     .range = {0, 100},
                                                                     .ordering = LeaderboardOrdering::HighestFirst,
                                                                     .sourceStat = stat,
                                                                     .localizationKey = "leaderboard.active"}}});
            REQUIRE(leaderboards.HasValue());
            const auto presence =
                BuildPresenceDefinitionRegistry(ids, {.stableIdRegistryFingerprint = ids.Fingerprint(),
                                                      .definitions = {{.id = ids.ResolvePresenceStatus("active.key").Value(),
                                                                       .detailPolicy = PresenceDetailPolicy::Forbidden,
                                                                       .maximumDetailUtf8Bytes = 0,
                                                                       .localizationKey = "presence.active"}}});
            REQUIRE(presence.HasValue());
            auto cooked = CookPlatformProviderManifest({.stableIds = ids,
                                                        .achievements = achievements.Value(),
                                                        .leaderboards = leaderboards.Value(),
                                                        .stats = stats.Value(),
                                                        .presence = presence.Value(),
                                                        .configuration = configuration});
            REQUIRE(cooked.HasValue());
            return std::move(cooked).Value();
        }
    }  // namespace

    TEST_CASE("Project ledger codec produces the same complete bytes after reordering and clean reload", "[platform-authoring]") {
        auto candidate = Candidate();
        const auto first = BuildPlatformStableIdRegistry(candidate);
        REQUIRE(first.HasValue());
        const auto bytes = Encode(first.Value());
        std::ranges::reverse(candidate.entries);
        for (auto &entry : candidate.entries)
            std::ranges::reverse(entry.aliases);
        const auto reordered = BuildPlatformStableIdRegistry(candidate);
        REQUIRE(reordered.HasValue());
        CHECK(Encode(reordered.Value()) == bytes);
        const auto reloaded = DecodePlatformServicesLedger(bytes, candidate.projectId, candidate.salt);
        REQUIRE(reloaded.HasValue());
        CHECK(reloaded.Value().Fingerprint() == first.Value().Fingerprint());
        CHECK(Encode(reloaded.Value()) == bytes);
        CHECK(reloaded.Value().Entries().size() == 8);
        REQUIRE(reloaded.Value().ResolveAchievement("active.key.a").HasValue());
        CheckError(reloaded.Value().ResolveAchievement("retired.key.z"), StableIdErrors::Tombstoned);
        CHECK(bytes.find("psid1:") == std::string::npos);
        CHECK(bytes.find("platformServicesIdSalt") == std::string::npos);
    }

    TEST_CASE("Project ledger codec rejects hostile documents before publication", "[platform-authoring]") {
        const auto candidate = Candidate();
        const auto registry = BuildPlatformStableIdRegistry(candidate).Value();
        const auto valid = Encode(registry);
        auto root = nlohmann::json::parse(valid);
        SECTION("Unsupported schema") {
            root["schemaVersion"] = 2;
        }
        SECTION("Fractional schema") {
            root["schemaVersion"] = 1.0;
        }
        SECTION("Negative schema") {
            root["schemaVersion"] = -1;
        }
        SECTION("Missing schema") {
            root.erase("schemaVersion");
        }
        SECTION("Foreign project") {
            root["projectId"] = "project.foreign";
        }
        SECTION("Unknown algorithm") {
            root["algorithm"] = "std.hash";
        }
        SECTION("Second salt authority") {
            root["salt"] = FormatPlatformServicesIdSalt(candidate.salt);
        }
        SECTION("Missing entry field") {
            root["entries"][0].erase("state");
        }
        SECTION("Unknown entry field") {
            root["entries"][0]["providerId"] = "restricted";
        }
        SECTION("Oversized key") {
            root["entries"][0]["canonicalKey"] = std::string(97, 'a');
        }
        SECTION("Oversized provenance") {
            root["entries"][0]["removalProvenance"] = std::string(257, 'a');
        }
        SECTION("Too many aliases") {
            root["entries"][0]["aliases"] = std::vector<std::string>(17, "alias");
        }
        SECTION("Numeric ID") {
            root["entries"][0]["id"] = UINT64_MAX;
        }
        SECTION("Uppercase ID") {
            root["entries"][0]["id"] = "sid1:ABCDEF0000000001";
        }
        SECTION("Zero ID") {
            root["entries"][0]["id"] = "sid1:0000000000000000";
        }
        SECTION("Unknown kind") {
            root["entries"][0]["kind"] = 5;
        }
        SECTION("Unknown state") {
            root["entries"][0]["state"] = "deprecated";
        }
        SECTION("Too many entries") {
            root["entries"] = std::vector<nlohmann::json>(4097, root["entries"][0]);
        }
        CheckError(DecodePlatformServicesLedger(root.dump(), candidate.projectId, candidate.salt), StableIdErrors::InvalidLedger);
        CHECK(Encode(registry) == valid);
    }

    TEST_CASE("Project ledger codec rejects malformed duplicate encoded and unbounded input", "[platform-authoring]") {
        const auto candidate = Candidate();
        const auto bytes = Encode(BuildPlatformStableIdRegistry(candidate).Value());
        const auto check = [&](const std::string_view text) {
            CheckError(DecodePlatformServicesLedger(text, candidate.projectId, candidate.salt), StableIdErrors::InvalidLedger);
        };
        check("");
        check("{");
        check(bytes + " trailing");
        check(bytes + bytes);
        check("{\"schemaVersion\":1,\"schemaVersion\":1}");
        check("{\"" + std::string(65, 'k') + "\":0}");
        auto duplicate = bytes;
        duplicate.insert(duplicate.find("\"canonicalKey\""), "\"kind\":1,");
        check(duplicate);
        check(std::string(4U * 1024U * 1024U + 1U, ' '));
        check(std::string(1024, '[') + std::string(1024, ']'));
        auto encoding = bytes;
        encoding.insert(encoding.find("project.portable"), 1, static_cast<char>(0xff));
        check(encoding);
        CheckError(DecodePlatformServicesLedger(bytes, candidate.projectId, {}), StableIdErrors::InvalidSalt);
        auto staleSalt = candidate.salt;
        staleSalt.bytes.back() = std::byte{1};
        CheckError(DecodePlatformServicesLedger(bytes, candidate.projectId, staleSalt), StableIdErrors::StoredIdMismatch);
    }

    TEST_CASE("Project ledger load preserves registry uniqueness diagnostics", "[platform-authoring]") {
        const auto candidate = Candidate();
        auto root = nlohmann::json::parse(Encode(BuildPlatformStableIdRegistry(candidate).Value()));
        auto &entries = root["entries"];
        auto active = std::ranges::find_if(entries, [](const auto &entry) {
            return entry["state"] == "active";
        });
        REQUIRE(active != entries.end());
        (*active)["aliases"] = {"active.key"};
        CheckError(DecodePlatformServicesLedger(root.dump(), candidate.projectId, candidate.salt), StableIdErrors::DuplicateKey);
        (*active)["aliases"] = {"retired.key"};
        CheckError(DecodePlatformServicesLedger(root.dump(), candidate.projectId, candidate.salt), StableIdErrors::Tombstoned);
    }

    TEST_CASE("Project ledger encoding rejects uninitialized snapshots and round-trips escaped UTF-8", "[platform-authoring]") {
        CheckError(EncodePlatformServicesLedger({}), StableIdErrors::InvalidLedger);
        CheckError(EncodeProjectPlatformServicesConfiguration({}), PlatformProjectConfigurationErrors::InvalidConfiguration);
        auto candidate = Candidate();
        candidate.projectId = "project.\"\\\n\xc3\xbc";
        for (auto &entry : candidate.entries)
            if (entry.state == PlatformStableIdState::Tombstoned)
                entry.removalProvenance = "migration.\"\\\n\xc3\xbc";
        const auto registry = BuildPlatformStableIdRegistry(candidate);
        REQUIRE(registry.HasValue());
        const auto bytes = Encode(registry.Value());
        const auto decoded = DecodePlatformServicesLedger(bytes, candidate.projectId, candidate.salt);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().Fingerprint() == registry.Value().Fingerprint());
        CHECK(Encode(decoded.Value()) == bytes);
    }

    TEST_CASE("Canonical project capture resolves portable policy for every host profile", "[platform-authoring]") {
        const auto root = Root();
        const auto capture = DecodeProjectSourceDocument(root.dump());
        REQUIRE(capture.HasValue());
        const auto salt = ResolveProjectPlatformServicesSalt(capture.Value());
        REQUIRE(salt.HasValue());
        CHECK(salt.Value() == Candidate().salt);
        for (const auto profile : {PlatformServicesHostProfile::InteractiveDevelopment, PlatformServicesHostProfile::HeadlessServer,
                                   PlatformServicesHostProfile::Cook, PlatformServicesHostProfile::Certification}) {
            const auto configuration = ResolveProjectPlatformServicesConfiguration(capture.Value(), profile, {}, {});
            REQUIRE(configuration.HasValue());
            CHECK(configuration.Value().UsesNullProvider());
            CHECK(configuration.Value().Profile() == profile);
            CHECK(configuration.Value().ProjectId() == capture.Value().metadata.projectId);
            CHECK(nlohmann::json::parse(EncodePolicy(configuration.Value())) == root["settings"]["platformServices"]);
        }
    }

    TEST_CASE("Root salt absence and malformed authority never trigger read-only repair", "[platform-authoring]") {
        auto root = Root();
        SECTION("Missing salt") {
            root.erase("platformServicesIdSalt");
        }
        SECTION("Zero salt") {
            root["platformServicesIdSalt"] = "psid1:00000000000000000000000000000000";
        }
        SECTION("Numeric salt") {
            root["platformServicesIdSalt"] = 1;
        }
        SECTION("Wrong type") {
            root["platformServicesIdSalt"] = nlohmann::json::object();
        }
        SECTION("Noncanonical salt") {
            root["platformServicesIdSalt"] = "psid1:0102030405060708090A0b0c0d0e0f10";
        }
        const auto original = root.dump();
        const auto captured = DecodeProjectSourceDocument(original);
        REQUIRE(captured.HasValue());
        const auto diagnostic = ResolveProjectPlatformServicesSalt(captured.Value());
        CheckError(diagnostic, StableIdErrors::InvalidSalt);
        REQUIRE_FALSE(diagnostic.ErrorValue().diagnostics.empty());
        CHECK(diagnostic.ErrorValue().diagnostics.front().location.source == ".horo/project.json");
        CHECK(diagnostic.ErrorValue().diagnostics.front().path == "platformServicesIdSalt");
        CheckError(ResolveProjectPlatformServicesConfiguration(captured.Value(), PlatformServicesHostProfile::Cook, {}, {}),
                   StableIdErrors::InvalidSalt);
        CHECK(root.dump() == original);
    }

    TEST_CASE("All host profiles reject the same malformed portable project policy", "[platform-authoring]") {
        auto root = Root();
        auto &policy = root["settings"]["platformServices"];
        const ErrorCodeDescriptor *expected = &PlatformProjectConfigurationErrors::InvalidConfiguration;
        SECTION("Missing policy") {
            root["settings"].erase("platformServices");
        }
        SECTION("Unsupported schema") {
            policy["schemaVersion"] = 2;
            expected = &PlatformProjectConfigurationErrors::UnsupportedVersion;
        }
        SECTION("Deprecated field") {
            policy["steamAppId"] = 480;
        }
        SECTION("Host profile override") {
            policy["profile"] = "certification";
        }
        SECTION("Wrong service count") {
            policy["services"] = {0, 0};
        }
        SECTION("Unknown service requirement") {
            policy["services"][0] = 3;
        }
        SECTION("Fractional requirement") {
            policy["services"][0] = 1.0;
        }
        SECTION("Negative requirement") {
            policy["services"][0] = -1;
        }
        SECTION("Provider fallback request") {
            policy["provider"]["fallback"] = "null";
        }
        SECTION("Unknown provider selection") {
            policy["provider"]["mode"] = "auto";
        }
        const auto captured = DecodeProjectSourceDocument(root.dump());
        REQUIRE(captured.HasValue());
        for (const auto profile : {PlatformServicesHostProfile::InteractiveDevelopment, PlatformServicesHostProfile::HeadlessServer,
                                   PlatformServicesHostProfile::Cook, PlatformServicesHostProfile::Certification})
            CheckError(ResolveProjectPlatformServicesConfiguration(captured.Value(), profile, {}, {}), *expected);
    }

    TEST_CASE("Project policy codec honors the common validator's explicit identity limits", "[platform-authoring]") {
        auto root = Root();
        const std::string key = "platform." + std::string(192, 'a');
        root["settings"]["platformServices"]["provider"] = {{"mode", "exact"}, {"key", key}};
        const auto captured = DecodeProjectSourceDocument(root.dump());
        REQUIRE(captured.HasValue());
        PlatformProviderModuleContribution contribution{.module = ModuleId{"horo.platform.test"},
                                                        .providerKey = key,
                                                        .provider = PlatformProviderId{41},
                                                        .interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                             PlatformServicesBackendInterfaceMinor},
                                                        .allowedProfiles = PlatformServicesHostProfileMask::Cook};
        contribution.supportedServices.fill(true);
        const std::array contributions{contribution};
        const std::array trusted{contribution.module};
        const auto policy = ResolveProjectPlatformServicesConfiguration(captured.Value(), PlatformServicesHostProfile::Cook, contributions,
                                                                        trusted, {.maximumProviderKeyBytes = 256});
        REQUIRE(policy.HasValue());
        CHECK(policy.Value().SelectedProviderKey() == key);
        CheckError(ResolveProjectPlatformServicesConfiguration(captured.Value(), PlatformServicesHostProfile::Cook, contributions, trusted),
                   PlatformProjectConfigurationErrors::InvalidConfiguration);
    }

    TEST_CASE("Project policy preserves exact provider and required-capability failures", "[platform-authoring]") {
        auto root = Root();
        root["settings"]["platformServices"]["services"][0] = 2;
        auto captured = DecodeProjectSourceDocument(root.dump());
        REQUIRE(captured.HasValue());
        CheckError(ResolveProjectPlatformServicesConfiguration(captured.Value(), PlatformServicesHostProfile::Cook, {}, {}),
                   PlatformProjectConfigurationErrors::RequiredCapabilityUnsupported);
        root["settings"]["platformServices"]["provider"] = {{"mode", "exact"}, {"key", "platform.missing"}};
        captured = DecodeProjectSourceDocument(root.dump());
        REQUIRE(captured.HasValue());
        CheckError(ResolveProjectPlatformServicesConfiguration(captured.Value(), PlatformServicesHostProfile::Cook, {}, {}),
                   PlatformProjectConfigurationErrors::ProviderNotFound);
        CheckError(ResolveProjectPlatformServicesConfiguration(captured.Value(), PlatformServicesHostProfile::Cook, {}, {},
                                                               {.maximumProviderKeyBytes = 0}),
                   PlatformProjectConfigurationErrors::CapacityExceeded);
        auto duplicate = root.dump();
        duplicate.insert(duplicate.find("\"platformServicesIdSalt\""), "\"platformServicesIdSalt\":\"bad\",");
        REQUIRE(DecodeProjectSourceDocument(duplicate).HasError());
    }

    TEST_CASE("Incremental ledger edits converge with clean source reload through the real semantic cook", "[platform-authoring]") {
        auto candidate = Candidate();
        const auto originalIds = BuildPlatformStableIdRegistry(candidate);
        REQUIRE(originalIds.HasValue());
        const auto source = DecodeProjectSourceDocument(Root().dump());
        REQUIRE(source.HasValue());
        const auto configuration = ResolveProjectPlatformServicesConfiguration(source.Value(), PlatformServicesHostProfile::Cook, {}, {});
        REQUIRE(configuration.HasValue());
        const auto initial = Cook(originalIds.Value(), configuration.Value());

        candidate.entries.front().aliases.push_back("active.compatibility");
        const auto incrementalIds =
            BuildPlatformStableIdRegistryReplacement(originalIds.Value(), originalIds.Value().Fingerprint(), candidate);
        REQUIRE(incrementalIds.HasValue());
        const auto incremental = Cook(incrementalIds.Value(), configuration.Value());
        CHECK(initial.generationFingerprint != incremental.generationFingerprint);
        const auto cleanIds = DecodePlatformServicesLedger(Encode(incrementalIds.Value()), source.Value().metadata.projectId,
                                                           ResolveProjectPlatformServicesSalt(source.Value()).Value());
        REQUIRE(cleanIds.HasValue());
        const auto clean = Cook(cleanIds.Value(), configuration.Value());
        CHECK(clean.neutralBytes == incremental.neutralBytes);
        CHECK(clean.mappingBytes == incremental.mappingBytes);
        CHECK(clean.generationFingerprint == incremental.generationFingerprint);
        CHECK(originalIds.Value().Fingerprint() != incrementalIds.Value().Fingerprint());
        CHECK(originalIds.Value().ResolveAchievement("active.key").Value() == cleanIds.Value().ResolveAchievement("active.key").Value());
        CheckError(cleanIds.Value().ResolveAchievement("retired.key"), StableIdErrors::Tombstoned);
    }
}  // namespace Horo::Application
