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
            "query": {"maximumWorkUnitsPerTick": 2048},
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
