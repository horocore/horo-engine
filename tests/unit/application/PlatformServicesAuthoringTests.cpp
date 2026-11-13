#include "Horo/Application/PlatformServicesAuthoring.h"
#include "Horo/Application/PlatformServicesDefinitionCodec.h"
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

        /** @brief Complete semantic source fixture used by codec and cook regressions. */
        struct Definitions {
            AchievementDefinitionRegistry achievements;
            StatDefinitionRegistry stats;
            LeaderboardDefinitionRegistry leaderboards;
            PresenceDefinitionRegistry presence;
        };

        /** @brief Builds semantic fixtures once through their existing owning validators. */
        Definitions Define(const PlatformStableIdRegistry &ids) {
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
            return {achievements.Value(), stats.Value(), leaderboards.Value(), presence.Value()};
        }

        /** @brief Reloads all semantic source documents before cooking, retaining exact semantic fingerprints. */
        PlatformProviderManifestCookOutput Cook(const PlatformStableIdRegistry &ids, const PlatformProjectConfiguration &configuration) {
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
    }  // namespace

    TEST_CASE("Canonical root capture fingerprints exact bytes rather than authored fingerprint fields", "[platform-authoring]") {
        auto root = Root();
        root["sourceFingerprint"] = "not-an-authority";
        const auto bytes = root.dump();
        const auto first = DecodeProjectSourceDocument(bytes);
        const auto reformatted = DecodeProjectSourceDocument(bytes + " ");
        REQUIRE(first.HasValue());
        REQUIRE(reformatted.HasValue());
        REQUIRE(first.Value().sourceFingerprint.has_value());
        CHECK(*first.Value().sourceFingerprint == ComputeSha256(std::as_bytes(std::span{bytes.data(), bytes.size()})));
        CHECK(first.Value().sourceFingerprint != reformatted.Value().sourceFingerprint);
        CHECK(first.Value().metadata.projectId == reformatted.Value().metadata.projectId);
    }

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

    TEST_CASE("Definition documents reject coercion and preserve typed semantic failures", "[platform-authoring]") {
        const auto ids = BuildPlatformStableIdRegistry(Candidate()).Value();
        const auto definitions = Define(ids);
        auto root = nlohmann::json::parse(EncodePlatformStatDefinitions(definitions.stats).Value());
        SECTION("Duplicate members") {
            auto bytes = root.dump();
            bytes.insert(1, "\"schemaVersion\":1,");
            CheckError(DecodePlatformStatDefinitions(bytes, ids), PlatformDefinitionErrors::InvalidDefinition);
        }
        SECTION("Missing field") {
            root["definitions"][0].erase("hidden");
            CheckError(DecodePlatformStatDefinitions(root.dump(), ids), PlatformDefinitionErrors::InvalidDefinition);
        }
        SECTION("Private provider field") {
            root["definitions"][0]["providerNativeName"] = "private-value-must-not-escape";
            const auto result = DecodePlatformStatDefinitions(root.dump(), ids);
            CheckError(result, PlatformDefinitionErrors::InvalidDefinition);
            for (const auto &diagnostic : result.ErrorValue().diagnostics)
                CHECK(diagnostic.message.find("private-value") == std::string::npos);
        }
        SECTION("Boolean coercion") {
            root["definitions"][0]["hidden"] = 0;
            CheckError(DecodePlatformStatDefinitions(root.dump(), ids), PlatformDefinitionErrors::InvalidDefinition);
        }
        SECTION("Fractional integer") {
            root["definitions"][0]["range"]["maximum"] = 100.0;
            CheckError(DecodePlatformStatDefinitions(root.dump(), ids), PlatformDefinitionErrors::InvalidDefinition);
        }
        SECTION("Unsigned overflow") {
            root["definitions"][0]["range"]["maximum"] = UINT64_MAX;
            CheckError(DecodePlatformStatDefinitions(root.dump(), ids), PlatformDefinitionErrors::InvalidDefinition);
        }
        SECTION("Signed endpoint boundaries remain exact") {
            root["definitions"][0]["range"] = {{"minimum", INT64_MIN}, {"maximum", INT64_MAX}};
            const auto decoded = DecodePlatformStatDefinitions(root.dump(), ids);
            REQUIRE(decoded.HasValue());
            CHECK(decoded.Value().Definitions()[0].range.minimum == INT64_MIN);
            CHECK(decoded.Value().Definitions()[0].range.maximum == INT64_MAX);
        }
        SECTION("Unknown enum value") {
            root["definitions"][0]["mutation"] = 4;
            CheckError(DecodePlatformStatDefinitions(root.dump(), ids), PlatformDefinitionErrors::InvalidDefinition);
        }
        SECTION("Malformed UTF-8") {
            auto bytes = root.dump();
            bytes.insert(bytes.find("stat.active"), 1, static_cast<char>(0xFF));
            CheckError(DecodePlatformStatDefinitions(bytes, ids), PlatformDefinitionErrors::InvalidDefinition);
        }
        SECTION("Deep nesting") {
            root["definitions"][0]["range"]["minimum"] = nlohmann::json::parse("[[[[[[[[[[0]]]]]]]]]]");
            CheckError(DecodePlatformStatDefinitions(root.dump(), ids), PlatformDefinitionErrors::InvalidDefinition);
        }
        SECTION("Capacity limits retain typed failure") {
            CheckError(DecodePlatformStatDefinitions(root.dump(), ids, {.maximumDefinitions = 0}),
                       PlatformDefinitionErrors::CapacityExceeded);
        }
        SECTION("Unsupported schema retains validator error") {
            root["schemaVersion"] = 2;
            CheckError(DecodePlatformStatDefinitions(root.dump(), ids), PlatformDefinitionErrors::UnsupportedVersion);
        }
        SECTION("Foreign owner") {
            root["projectId"] = "project.foreign";
            CheckError(DecodePlatformStatDefinitions(root.dump(), ids), PlatformDefinitionErrors::StaleIdentityRegistry);
        }
        SECTION("Stale ledger generation") {
            root["stableIdRegistryFingerprint"] = FormatSha256({});
            CheckError(DecodePlatformStatDefinitions(root.dump(), ids), PlatformDefinitionErrors::StaleIdentityRegistry);
        }
        SECTION("Wrong identity kind") {
            root["definitions"][0]["id"] = FormatPlatformServiceStableId({ids.ResolveAchievement("active.key").Value().value});
            CheckError(DecodePlatformStatDefinitions(root.dump(), ids), PlatformDefinitionErrors::UnknownIdentity);
        }
        SECTION("Tombstone identity") {
            const auto retired = DerivePlatformServiceStableId(Candidate().salt, PlatformServiceIdKind::Stat, "retired.key").Value();
            root["definitions"][0]["id"] = FormatPlatformServiceStableId(retired);
            CHECK(DecodePlatformStatDefinitions(root.dump(), ids).HasError());
        }
        SECTION("Missing active definition") {
            root["definitions"] = nlohmann::json::array();
            CheckError(DecodePlatformStatDefinitions(root.dump(), ids), PlatformDefinitionErrors::IncompleteRegistry);
        }
        SECTION("Duplicate definition") {
            root["definitions"].push_back(root["definitions"][0]);
            CheckError(DecodePlatformStatDefinitions(root.dump(), ids), PlatformDefinitionErrors::DuplicateDefinition);
        }
        SECTION("Document byte bound") {
            CheckError(DecodePlatformStatDefinitions(std::string(4U * 1024U * 1024U + 1, ' '), ids),
                       PlatformDefinitionErrors::InvalidDefinition);
        }
    }

    TEST_CASE("Every definition codec retains owning validator failures", "[platform-authoring]") {
        const auto ids = BuildPlatformStableIdRegistry(Candidate()).Value();
        const auto definitions = Define(ids);
        auto achievement = nlohmann::json::parse(EncodePlatformAchievementDefinitions(definitions.achievements).Value());
        achievement["definitions"][0]["progress"]["total"] = 2;
        CheckError(DecodePlatformAchievementDefinitions(achievement.dump(), ids), AchievementDefinitionErrors::InvalidDefinition);
        auto leaderboard = nlohmann::json::parse(EncodePlatformLeaderboardDefinitions(definitions.leaderboards).Value());
        leaderboard["definitions"][0]["sourceStat"] = FormatPlatformServiceStableId({ids.ResolveAchievement("active.key").Value().value});
        CheckError(DecodePlatformLeaderboardDefinitions(leaderboard.dump(), ids, definitions.stats),
                   PlatformDefinitionErrors::InvalidCrossReference);
        leaderboard["definitions"][0]["sourceStat"] = nullptr;
        const auto independent = DecodePlatformLeaderboardDefinitions(leaderboard.dump(), ids, definitions.stats);
        REQUIRE(independent.HasValue());
        CHECK(nlohmann::json::parse(EncodePlatformLeaderboardDefinitions(independent.Value()).Value())["definitions"][0]["sourceStat"]
                  .is_null());
        auto presence = nlohmann::json::parse(EncodePlatformPresenceDefinitions(definitions.presence).Value());
        presence["definitions"][0]["maximumDetailUtf8Bytes"] = 1;
        CheckError(DecodePlatformPresenceDefinitions(presence.dump(), ids), PlatformDefinitionErrors::InvalidDefinition);
    }

    TEST_CASE("Platform source adoption preserves the existing 256-byte opaque root identity", "[platform-authoring]") {
        auto candidate = Candidate();
        candidate.projectId = std::string(256, 'x');
        const auto ids = BuildPlatformStableIdRegistry(candidate);
        REQUIRE(ids.HasValue());
        const auto reloaded = DecodePlatformServicesLedger(Encode(ids.Value()), candidate.projectId, candidate.salt);
        REQUIRE(reloaded.HasValue());
        CHECK(reloaded.Value().ProjectId() == candidate.projectId);
        auto root = Root();
        root["projectId"] = candidate.projectId;
        const auto captured = DecodeProjectSourceDocument(root.dump());
        REQUIRE(captured.HasValue());
        const auto configuration = ResolveProjectPlatformServicesConfiguration(captured.Value(), PlatformServicesHostProfile::Cook, {}, {});
        REQUIRE(configuration.HasValue());
        CHECK(configuration.Value().ProjectId() == candidate.projectId);
        CHECK_FALSE(Cook(ids.Value(), configuration.Value()).neutralBytes.empty());
        candidate.projectId.push_back('x');
        CheckError(BuildPlatformStableIdRegistry(candidate), StableIdErrors::InvalidLedger);
    }

    TEST_CASE("Definition documents honor supported counts above default limits", "[platform-authoring]") {
        PlatformStableIdRegistryCandidate candidate{.projectId = "project.large-definitions", .salt = Candidate().salt};
        for (std::uint32_t index = 0; index < 4096; ++index) {
            const auto key = "stat.key_" + std::to_string(index);
            candidate.entries.push_back(
                {.kind = PlatformServiceIdKind::Stat,
                 .canonicalKey = key,
                 .storedId = DerivePlatformServiceStableId(candidate.salt, PlatformServiceIdKind::Stat, key).Value()});
        }
        const auto ids = BuildPlatformStableIdRegistry(candidate);
        REQUIRE(ids.HasValue());
        StatDefinitionRegistryCandidate document{.stableIdRegistryFingerprint = ids.Value().Fingerprint()};
        for (const auto &entry : ids.Value().Entries())
            document.definitions.push_back({.id = {entry.storedId.value},
                                            .range = {0, 100},
                                            .mutation = StatMutationPolicy::SetMaximum,
                                            .localizationKey = "stat.title"});
        const PlatformDefinitionRegistryLimits limits{.maximumDefinitions = 4096};
        const auto registry = BuildStatDefinitionRegistry(ids.Value(), document, limits);
        REQUIRE(registry.HasValue());
        const auto bytes = EncodePlatformStatDefinitions(registry.Value());
        REQUIRE(bytes.HasValue());
        const auto reloaded = DecodePlatformStatDefinitions(bytes.Value(), ids.Value(), limits);
        REQUIRE(reloaded.HasValue());
        CHECK(reloaded.Value().Definitions().size() == 4096);
        CHECK(reloaded.Value().Fingerprint() == registry.Value().Fingerprint());
        CheckError(DecodePlatformStatDefinitions(bytes.Value(), ids.Value()), PlatformDefinitionErrors::CapacityExceeded);
    }

    TEST_CASE("Definition document encoders reject uninitialized snapshots", "[platform-authoring]") {
        CheckError(EncodePlatformAchievementDefinitions(AchievementDefinitionRegistry{}), AchievementDefinitionErrors::InvalidDefinition);
        CheckError(EncodePlatformStatDefinitions(StatDefinitionRegistry{}), PlatformDefinitionErrors::InvalidDefinition);
        CheckError(EncodePlatformLeaderboardDefinitions(LeaderboardDefinitionRegistry{}), PlatformDefinitionErrors::InvalidDefinition);
        CheckError(EncodePlatformPresenceDefinitions(PresenceDefinitionRegistry{}), PlatformDefinitionErrors::InvalidDefinition);
    }

    TEST_CASE("Previously captured semantic documents cannot cross a ledger replacement", "[platform-authoring]") {
        auto candidate = Candidate();
        const auto previous = BuildPlatformStableIdRegistry(candidate).Value();
        const auto definitions = Define(previous);
        candidate.entries.front().aliases.push_back("reviewed.compatibility.alias");
        const auto replacement = BuildPlatformStableIdRegistryReplacement(previous, previous.Fingerprint(), candidate);
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value().Fingerprint() != previous.Fingerprint());
        CheckError(DecodePlatformAchievementDefinitions(EncodePlatformAchievementDefinitions(definitions.achievements).Value(),
                                                        replacement.Value()),
                   AchievementDefinitionErrors::StaleIdentityRegistry);
        CheckError(DecodePlatformStatDefinitions(EncodePlatformStatDefinitions(definitions.stats).Value(), replacement.Value()),
                   PlatformDefinitionErrors::StaleIdentityRegistry);
        CheckError(DecodePlatformLeaderboardDefinitions(EncodePlatformLeaderboardDefinitions(definitions.leaderboards).Value(),
                                                        replacement.Value(), Define(replacement.Value()).stats),
                   PlatformDefinitionErrors::StaleIdentityRegistry);
        CheckError(DecodePlatformPresenceDefinitions(EncodePlatformPresenceDefinitions(definitions.presence).Value(), replacement.Value()),
                   PlatformDefinitionErrors::StaleIdentityRegistry);
        CHECK(EncodePlatformServicesLedger(previous).HasValue());
        CHECK(definitions.stats.StableIdRegistryFingerprint() == previous.Fingerprint());
    }
}  // namespace Horo::Application
