#include "../runtime/networking/NetworkProjectSettingsTestSupport.h"
#include "../runtime/networking/ReplicationDescriptorTestSupport.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/Application/ProjectAiSettings.h"
#include "Horo/Application/ProjectNetworkSettings.h"

#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using Json = nlohmann::json;

    Json RegisteredSource() {
        const auto version = ParseHoroVersion("0.2.0").Value();
        const auto *decision = BuiltInReleaseCompatibilityRegistry().Find({version});
        REQUIRE(decision != nullptr);
        return {{"horoVersion", "0.2.0"},
                {"persistentContract", FormatPersistentContractHash(decision->persistentContract)},
                {"projectId", "project-source"},
                {"name", "Source capture"},
                {"projectVersion", "1.0.0"},
                {"createdAt", "2026-10-04T00:00:00Z"},
                {"settings", {{"renderBackend", "null"}}}};
    }

    Network::NetworkProjectSettings AuthoredNetwork() {
        using namespace Network;
        auto input = DefaultNetworkProjectSettings(NetworkProjectSettingsId::Create(91).Value()).Value();
        input.replication.completeness = NetworkReplicationInventoryCompleteness::Complete;
        std::vector<ReplicationFieldDescriptor> fields;
        for (std::uint32_t index = 1; index <= 32; ++index)
            fields.push_back(TestSupport::Field(index));
        auto schema = TestSupport::Schema(19, std::move(fields));
        schema.tombstonedFields = {TestSupport::FieldIdValue(33)};
        input.replication.declarations = {{std::move(schema)}};
        auto settings = NetworkProjectSettings::Create(input);
        REQUIRE(settings.HasValue());
        return std::move(settings).Value();
    }

    TEST_CASE("Project source accessor preserves absence without inventing Network authority", "[unit][application][source-profile]") {
        const auto captured = DecodeProjectSourceDocument(RegisteredSource().dump());
        REQUIRE(captured.HasValue());
        REQUIRE_FALSE(captured.Value().networkSettingsSource.has_value());
        const auto resolved = ResolveProjectNetworkSettings(captured.Value());
        REQUIRE(resolved.HasValue());
        REQUIRE_FALSE(resolved.Value().has_value());
    }

    TEST_CASE("Project AI policy keeps gameplay budgets independent of rendering and gates packaged debug",
              "[unit][application][source-profile][ai]") {
        auto document = RegisteredSource();
        const auto absent = DecodeProjectSourceDocument(document.dump());
        REQUIRE(absent.HasValue());
        REQUIRE(ResolveProjectAiSettings(absent.Value()).Value() == std::nullopt);
        document["settings"]["ai"] = Json::parse(R"({
            "schemaVersion": 1,
            "scheduler": {
                "mode": "bestEffort",
                "maximumAgents": 64,
                "evaluationsPerTick": 12,
                "workUnitsPerTick": 512,
                "simulationProfile": {
                    "fullDistanceMillimeters": 5000,
                    "reducedDistanceMillimeters": 10000,
                    "suspensionDistanceMillimeters": 20000,
                    "reducedIntervalTicks": 4,
                    "demotionDelayTicks": 8
                }
            },
            "perception": {"maximumWorkUnitsPerTick": 1024},
            "query": {"maximumWorkUnitsPerTick": 2048, "cache": {"maximumEntries": 16, "maximumBytes": 32768, "lifetimeTicks": 30}},
            "development": {"liveMutation": true, "replayCapture": true}
        })");
        for (const auto *backend : {"null", "vulkan", "opengl"}) {
            document["settings"]["renderBackend"] = backend;
            const auto source = DecodeProjectSourceDocument(document.dump());
            REQUIRE(source.HasValue());
            const auto resolved = ResolveProjectAiSettings(source.Value());
            REQUIRE(resolved.HasValue());
            REQUIRE(resolved.Value());
            const auto &policy = *resolved.Value();
            CHECK(policy.HasAuthoredSchedulerMode());
            CHECK(policy.Scheduler().mode == AI::AiSchedulingMode::BestEffort);
            CHECK(policy.Scheduler().maximumAgents == 64);
            CHECK(policy.Scheduler().evaluationsPerTick == 12);
            CHECK(policy.Scheduler().workUnitsPerTick == 512);
            CHECK(policy.PerceptionWorkUnitsLimit() == 1024);
            CHECK(policy.QueryWorkUnitsLimit() == 2048);
            REQUIRE(policy.QueryCacheLimits());
            CHECK(policy.QueryCacheLimits()->maximumEntries == 16);
            CHECK(policy.QueryCacheLimits()->maximumBytes == 32768);
            CHECK(policy.QueryCacheLimits()->lifetimeTicks == 30);
            REQUIRE(policy.Scheduler().simulationProfile);
            CHECK(policy.Scheduler().simulationProfile->reducedIntervalTicks == 4);
            for (const auto operation : {AiProjectDebugOperation::LiveMutation, AiProjectDebugOperation::ReplayCapture}) {
                CHECK(policy.AllowsDebug(AiProjectRuntimeKind::Development, operation));
                CHECK_FALSE(policy.AllowsDebug(AiProjectRuntimeKind::Packaged, operation));
            }
        }
    }

    TEST_CASE("Project AI can author budgets without selecting a scheduler mode", "[unit][application][source-profile][ai]") {
        auto document = RegisteredSource();
        document["settings"]["ai"] =
            Json::parse(R"({"schemaVersion":1,"scheduler":{"evaluationsPerTick":3},"perception":{"maximumWorkUnitsPerTick":16}})");
        const auto source = DecodeProjectSourceDocument(document.dump());
        REQUIRE(source.HasValue());
        const auto resolved = ResolveProjectAiSettings(source.Value());
        REQUIRE(resolved.HasValue());
        REQUIRE(resolved.Value());
        CHECK_FALSE(resolved.Value()->HasAuthoredSchedulerMode());
        CHECK(resolved.Value()->Scheduler().evaluationsPerTick == 3);
        CHECK(resolved.Value()->PerceptionWorkUnitsLimit() == 16);
        CHECK_FALSE(resolved.Value()->QueryCacheLimits());
    }

    TEST_CASE("Project NPC dialogue allows typed IDs without accepting endpoint credentials", "[unit][application][source-profile][ai]") {
        auto document = RegisteredSource();
        auto &ai = document["settings"]["ai"];
        ai = Json::parse(R"({"schemaVersion":1,"npcDialogue":{"provider":8,"destination":9,"version":1,"fields":[4,2]}})");
        const auto resolve = [&] {
            const auto source = DecodeProjectSourceDocument(document.dump());
            REQUIRE(source.HasValue());
            return ResolveProjectAiSettings(source.Value());
        };
        const auto accepted = resolve();
        REQUIRE(accepted.HasValue());
        REQUIRE(accepted.Value());
        REQUIRE(accepted.Value()->NpcDialogue());
        const auto &dialogue = *accepted.Value()->NpcDialogue();
        CHECK(dialogue.route.provider.Value() == 8);
        CHECK(dialogue.route.destination.Value() == 9);
        CHECK(dialogue.fields[0].Value() == 2);
        CHECK(dialogue.fields[1].Value() == 4);
        CHECK(dialogue.triggers.empty());

        auto &draft = ai["npcDialogue"];
        SECTION("endpoint URL is host-owned") { draft["endpoint"] = "https://example.invalid/"; }
        SECTION("duplicate fields are rejected") { draft["fields"] = Json::array({2, 2}); }
        SECTION("unknown protocol is rejected") { draft["version"] = 2; }
        SECTION("over-capacity field sets are rejected") {
            draft["fields"] = Json::array();
            for (std::size_t index = 0; index <= AI::NpcDialogueTurn::MaximumFacts; ++index)
                draft["fields"].push_back(index + 1);
        }
        CHECK(resolve().HasError());
    }

    TEST_CASE("Project NPC dialogue binds distinct triggers to authored agents and fact tokens", "[unit][application][source-profile][ai]") {
        auto document = RegisteredSource();
        auto &ai = document["settings"]["ai"];
        ai = Json::parse(R"({"schemaVersion":1,"npcDialogue":{"provider":8,"destination":9,"version":1,"fields":[4,2],
            "triggers":[{"id":9,"agent":12,"facts":[]},
                        {"id":5,"agent":7,"facts":[{"field":4,"value":42},{"field":2,"value":30}]}]}})");
        auto &triggers = ai["npcDialogue"]["triggers"];
        SECTION("valid source decodes to a canonical catalog") {
            const auto source = DecodeProjectSourceDocument(document.dump());
            REQUIRE(source.HasValue());
            const auto parsed = ResolveProjectAiSettings(source.Value());
            REQUIRE(parsed.HasValue());
            REQUIRE(parsed.Value());
            REQUIRE(parsed.Value()->NpcDialogue());
            const auto &catalog = parsed.Value()->NpcDialogue()->triggers;
            REQUIRE(catalog.size() == 2);
            CHECK(catalog.front().id.Value() == 5);
            CHECK(catalog.front().agent.Value() == 7);
            REQUIRE(catalog.front().facts.size() == 2);
            CHECK(catalog.front().facts.front().field.Value() == 2);
            CHECK(catalog.front().facts.front().value.Value() == 30);
            return;
        }
        SECTION("duplicate trigger IDs fail") { triggers[0]["id"] = 5; }
        SECTION("zero agent IDs fail") { triggers[1]["agent"] = 0; }
        SECTION("duplicate fact fields fail") { triggers[1]["facts"][0]["field"] = 2; }
        SECTION("free-form context fails") { triggers[1]["facts"][0]["value"] = "text"; }
        SECTION("oversized trigger catalogs fail") {
            while (triggers.size() <= ProjectNpcDialogueSettings::MaximumTriggers)
                triggers.push_back({{"id", triggers.size() + 10}, {"agent", 7}, {"facts", Json::array()}});
        }
        const auto source = DecodeProjectSourceDocument(document.dump());
        REQUIRE(source.HasValue());
        CHECK(ResolveProjectAiSettings(source.Value()).HasError());
    }

    TEST_CASE("NPC dialogue presentation requires an authored trigger and fallback cue", "[unit][application][source-profile][ai]") {
        auto document = RegisteredSource();
        auto &dialogue = document["settings"]["ai"];
        dialogue = Json::parse(R"({"schemaVersion":1,"npcDialogue":{"provider":1,"destination":2,"version":1,"fields":[],
          "triggers":[{"id":5,"agent":7,"facts":[]}],
          "presentation":{"trigger":5,"fallback":4,"deadlineTicks":10,"workerMilliseconds":250,
            "captions":[{"id":9,"namespace":"game.dialogue","key":"welcome","text":"Welcome","durationTicks":5},
                        {"id":4,"namespace":"game.dialogue","key":"offline","text":"Come back later.","durationTicks":10}]}}})");
        auto &presentation = dialogue["npcDialogue"]["presentation"];
        const auto resolve = [&] {
            const auto source = DecodeProjectSourceDocument(document.dump());
            REQUIRE(source.HasValue());
            return ResolveProjectAiSettings(source.Value());
        };
        SECTION("valid captions are ordered by stable cue ID") {
            const auto parsed = resolve();
            REQUIRE(parsed.HasValue());
            REQUIRE(parsed.Value());
            REQUIRE(parsed.Value()->NpcDialogue());
            REQUIRE(parsed.Value()->NpcDialogue()->presentation);
            const auto &plan = *parsed.Value()->NpcDialogue()->presentation;
            REQUIRE(plan.captions.size() == 2);
            CHECK(plan.captions.front().cue == plan.fallback);
            CHECK(plan.captions.front().fallbackText == "Come back later.");
            CHECK(plan.captions.front().durationTicks == 10);
            return;
        }
        SECTION("missing trigger is rejected") { presentation["trigger"] = 6; }
        SECTION("missing fallback is rejected") { presentation["fallback"] = 8; }
        SECTION("duplicate cues are rejected") { presentation["captions"][1]["id"] = 9; }
        SECTION("zero budget is rejected") { presentation["workerMilliseconds"] = 0; }
        SECTION("zero caption duration is rejected") { presentation["captions"][0]["durationTicks"] = 0; }
        SECTION("caption destinations do not belong to the local catalog") { presentation["captions"][0]["destination"] = "https://example.invalid/"; }
        CHECK(resolve().HasError());
    }

    TEST_CASE("Project AI admits only typed local-policy targets and controller bindings", "[unit][application][source-profile][ai]") {
        auto document = RegisteredSource();
        auto &ai = document["settings"]["ai"];
        ai = Json::parse(R"({"schemaVersion":1,"localPolicy":{"packageTarget":"headless-null","interpreter":"int16-linear-v1",
            "interpreterTarget":7,"maximumModelBytes":4096,"maximumWorkUnits":128,"blackboardSchema":11,"tasks":[20,21],
            "controllers":[{"controller":3,"artifact":"00000000-0000-0000-0000-000000000042","featureKeys":[18,12]}]}})");
        const auto resolve = [&] {
            const auto source = DecodeProjectSourceDocument(document.dump());
            REQUIRE(source.HasValue());
            return ResolveProjectAiSettings(source.Value());
        };
        const auto captured = resolve();
        REQUIRE(captured.HasValue());
        REQUIRE(captured.Value());
        REQUIRE(captured.Value()->LocalPolicy());
        const auto &policy = *captured.Value()->LocalPolicy();
        CHECK(policy.packageTarget.Value() == "headless-null");
        CHECK(policy.target.id.Value() == 7);
        CHECK(policy.target.maximumWorkUnits == 128);
        REQUIRE(policy.blackboard);
        CHECK(policy.blackboard->Value() == 11);
        REQUIRE(policy.tasks.Tasks().size() == 2);
        REQUIRE(policy.controllers.size() == 1);
        CHECK(policy.controllers.front().controller.Value() == 3);
        REQUIRE(policy.controllers.front().featureKeys.size() == 2);
        CHECK(policy.controllers.front().featureKeys[0].Value() == 18);
        CHECK(policy.controllers.front().featureKeys[1].Value() == 12);

        auto &local = ai["localPolicy"];
        SECTION("unknown interpreter") {
            local["interpreter"] = "unsupported";
        }
        SECTION("missing memory budget") {
            local.erase("maximumModelBytes");
        }
        SECTION("zero typed key") {
            local["controllers"][0]["featureKeys"] = Json::array({0});
        }
        SECTION("duplicate controller") {
            auto duplicate = local["controllers"].front();
            local["controllers"].push_back(std::move(duplicate));
        }
        SECTION("duplicate task") {
            local["tasks"].push_back(20);
        }
        SECTION("invalid package target") {
            local["packageTarget"] = "../other";
        }
        const auto rejected = resolve();
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == AI::AIErrors::SchedulerPolicyInvalid.code.Value());
    }

    TEST_CASE("Project AI rejects invalid canonical settings before activation", "[unit][application][source-profile][ai]") {
        const std::array invalid{
            R"({"schemaVersion":2})",
            R"({"schemaVersion":1,"scheduler":{"maximumAgents":8,"evaluationsPerTick":9}})",
            R"({"schemaVersion":1,"scheduler":{"pendingWorkers":-1}})",
            R"({"schemaVersion":1,"scheduler":{"workUnitsPerTick":12.5}})",
            R"({"schemaVersion":1,"scheduler":{"mode":"bestEffort","simulationProfile":{}}})",
            R"({"schemaVersion":1,"scheduler":{"simulationProfile":{"fullDistanceMillimeters":1,"reducedDistanceMillimeters":2,"suspensionDistanceMillimeters":3,"reducedIntervalTicks":4,"demotionDelayTicks":1}}})",
            R"({"schemaVersion":1,"scheduler":{"renderBackend":"vulkan"}})",
            R"({"schemaVersion":1,"perception":{"maximumWorkUnitsPerTick":0}})",
            R"({"schemaVersion":1,"perception":{"maximumWorkUnitsPerTick":1048577}})",
            R"({"schemaVersion":1,"query":{"maximumWorkUnitsPerTick":0}})",
            R"({"schemaVersion":1,"query":{"maximumWorkUnitsPerTick":1048577}})",
            R"({"schemaVersion":1,"query":{"cache":{"maximumEntries":1,"maximumBytes":4096}}})",
            R"({"schemaVersion":1,"query":{"cache":{"maximumEntries":257,"maximumBytes":4096,"lifetimeTicks":1}}})",
            R"({"schemaVersion":1,"query":{"cache":{"maximumEntries":1,"maximumBytes":134217729,"lifetimeTicks":1}}})",
            R"({"schemaVersion":1,"development":{"liveMutation":"true"}})",
            R"({"schemaVersion":1,"visualization":{"inspectAgent":4}})",
        };
        for (const auto *policy : invalid) {
            INFO(policy);
            auto document = RegisteredSource();
            document["settings"]["ai"] = Json::parse(policy);
            const auto source = DecodeProjectSourceDocument(document.dump());
            REQUIRE(source.HasValue());
            const auto rejected = ResolveProjectAiSettings(source.Value());
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == AI::AIErrors::SchedulerPolicyInvalid.code.Value());
        }
    }

    TEST_CASE("Project source accessor carries bounded authored inventory through sole Network codec",
              "[unit][application][source-profile]") {
        const auto settings = AuthoredNetwork();
        auto document = RegisteredSource();
        document["settings"]["network"] = Json::parse(Network::SerializeNetworkProjectSettings(settings));
        const auto captured = DecodeProjectSourceDocument(document.dump());
        REQUIRE(captured.HasValue());
        const auto resolved = ResolveProjectNetworkSettings(captured.Value());
        REQUIRE(resolved.HasValue());
        REQUIRE(resolved.Value().has_value());
        REQUIRE(resolved.Value()->Fingerprint() == settings.Fingerprint());
        REQUIRE(resolved.Value()->ReplicationInventory().declarations.front().schema.fields.size() == 32);
        REQUIRE(resolved.Value()->ReplicationInventory().completeness == Network::NetworkReplicationInventoryCompleteness::Complete);
        auto duplicate = document.dump();
        const auto contractKey = duplicate.find("\"contractVersion\":");
        REQUIRE(contractKey != std::string::npos);
        duplicate.insert(contractKey, "\"contractVersion\":3,");
        const auto duplicateSource = DecodeProjectSourceDocument(duplicate);
        REQUIRE(duplicateSource.HasError());
        REQUIRE(duplicateSource.ErrorValue().code.Value() == "project.metadata.duplicate_key");
        document["persistentContract"] = FormatPersistentContractHash(
            BuiltInReleaseCompatibilityRegistry().Find({ParseHoroVersion("0.1.0").Value()})->persistentContract);
        const auto mismatched = DecodeProjectSourceDocument(document.dump());
        REQUIRE(mismatched.HasError());
        REQUIRE(mismatched.ErrorValue().code.Value() == "project.metadata.limit_exceeded");
    }

    TEST_CASE("Project source accessor retains legacy Unknown and rejects invalid Network source", "[unit][application][source-profile]") {
        const auto settings = AuthoredNetwork();
        auto document = RegisteredSource();
        document["settings"]["network"] =
            Network::TestSupport::LegacyNetworkSettingsDocument(settings, Network::TestSupport::LegacyNetworkSettingsVersion::Two);
        const auto captured = DecodeProjectSourceDocument(document.dump());
        REQUIRE(captured.HasValue());
        const auto resolved = ResolveProjectNetworkSettings(captured.Value());
        REQUIRE(resolved.HasValue());
        REQUIRE(resolved.Value().has_value());
        REQUIRE(resolved.Value()->ReplicationInventory().completeness == Network::NetworkReplicationInventoryCompleteness::Unknown);
        REQUIRE(resolved.Value()->ReplicationInventory().declarations.empty());
        document["settings"]["network"]["unexpected"] = true;
        const auto invalidSource = DecodeProjectSourceDocument(document.dump());
        REQUIRE(invalidSource.HasValue());
        const auto denied = ResolveProjectNetworkSettings(invalidSource.Value());
        const auto direct = Network::ParseNetworkProjectSettings(*invalidSource.Value().networkSettingsSource);
        REQUIRE(denied.HasError());
        REQUIRE(direct.HasError());
        REQUIRE(denied.ErrorValue().code.Value() == direct.ErrorValue().code.Value());
        document["settings"]["network"] = 0;
        const auto nonObject = DecodeProjectSourceDocument(document.dump());
        REQUIRE(nonObject.HasValue());
        REQUIRE(ResolveProjectNetworkSettings(nonObject.Value()).HasError());
        document["horoVersion"] = "0.1.0";
        REQUIRE(DecodeProjectSourceDocument(document.dump()).HasValue());
    }
}  // namespace
