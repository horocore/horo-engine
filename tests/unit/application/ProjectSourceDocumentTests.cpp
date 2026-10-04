#include "../runtime/networking/NetworkProjectSettingsTestSupport.h"
#include "../runtime/networking/ReplicationDescriptorTestSupport.h"
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
