#include "Horo/Application/ProjectMigrationCatalog.h"
#include "Horo/Navigation/NavigationDataSerialization.h"
#include "Horo/Network/NetworkProjectSettings.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using Json = nlohmann::json;

    /** @brief Resolves the real convention-discovered pipeline, not a test-only migration substitute. */
    ProjectMigrationDefinition AuthoringMigration() {
        auto catalog = BuildBuiltInProjectMigrationCatalog();
        REQUIRE(catalog.HasValue());
        const auto found = std::ranges::find(catalog.Value(), std::string{"core.authoring.navigation_network"}, [](const auto &entry) {
            return entry.id.value;
        });
        REQUIRE(found != catalog.Value().end());
        return *found;
    }

    Json Metadata() {
        return {{"horoVersion", "0.1.0"},
                {"persistentContract", "sha256:997e790fc23515b362847c755006156aa35353ce7f2624518acf7ed1214ddb03"},
                {"projectId", "migration-project"},
                {"name", "Migration"},
                {"projectVersion", "1.0.0"},
                {"createdAt", "2026-10-04"},
                {"settings",
                 {{"assetCompression", "lz4"},
                  {"textureCompression", "bc7"},
                  {"renderBackend", "null"},
                  {"unrelatedOwnedSetting", {1, 2, 3}}}}};
    }

    Result<MigrationDocumentChange> NormalizeMetadata(const Json &root) {
        const auto text = root.dump();
        const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
        const auto definition = AuthoringMigration();
        return definition.pipeline->Nodes().front().documentStage->Execute({.handle = {1, 1},
                                                                            .path = ".horo/project.json",
                                                                            .kind = MigrationDocumentKind::ProjectMetadata,
                                                                            .bytes = bytes},
                                                                           {}, {});
    }

    Json Replacement(const MigrationDocumentChange &change) {
        return Json::parse(reinterpret_cast<const char *>(change.replacement.data()),
                           reinterpret_cast<const char *>(change.replacement.data() + change.replacement.size()));
    }

    Json PortableNetwork() {
        const auto input = Network::DefaultNetworkProjectSettings(Network::NetworkProjectSettingsId::Create(9).Value());
        REQUIRE(input.HasValue());
        const auto settings = Network::NetworkProjectSettings::Create(input.Value());
        REQUIRE(settings.HasValue());
        return Json::parse(Network::SerializeNetworkProjectSettings(settings.Value()));
    }

    /** @brief Captures a legacy plugin-owned optional/required record through its original explicit support table. */
    std::vector<std::byte> LegacyNavigation(const bool required) {
        using namespace Horo::Navigation;
        const auto type = NavigationAuthoredRecordTypeId::Create(77).Value();
        const std::array support{NavigationAuthoredRecordSupport{.type = type, .minimum = {1, 0}, .maximum = {1, 0}}};
        const NavigationSourceLoadContext context{.supportedRecords = support};
        auto source = NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion,
                                                      {{.id = NavigationAuthoredRecordId::Create(12).Value(),
                                                        .type = type,
                                                        .version = {1, 0},
                                                        .required = required,
                                                        .payload = {std::byte{0x42}, std::byte{0x71}}}},
                                                      {{.type = type, .version = {1, 0}, .payload = {std::byte{0x99}}}}, context);
        REQUIRE(source.HasValue());
        auto bytes = SerializeNavigationSourceRecords(source.Value());
        REQUIRE(bytes.HasValue());
        return std::move(bytes).Value();
    }
}  // namespace

TEST_CASE("0.2 authoring migration is registered with the exact immutable 0.1 source contract",
          "[unit][application][authoring-migration]") {
    const auto definition = AuthoringMigration();
    REQUIRE(FormatHoroVersion(definition.from.value) == "0.1.0");
    REQUIRE(FormatHoroVersion(definition.to.value) == "0.2.0");
    REQUIRE(definition.sourceContract ==
            ParsePersistentContractHash("sha256:997e790fc23515b362847c755006156aa35353ce7f2624518acf7ed1214ddb03").Value());
    REQUIRE(definition.pipeline->Nodes().size() == 3);
    auto support = BuildBuiltInProjectMigrationSupportDescriptor();
    REQUIRE(support.HasValue());
    REQUIRE(definition.targetContract == support.Value().targetContract);
    REQUIRE(support.Value().targetValidator != nullptr);
}

TEST_CASE("0.2 migration never fabricates Network policy when no project policy exists", "[unit][application][authoring-migration]") {
    auto changed = NormalizeMetadata(Metadata());
    REQUIRE(changed.HasValue());
    REQUIRE_FALSE(changed.Value().changed);
    REQUIRE(changed.Value().replacement.empty());
}

TEST_CASE("0.2 migration preserves legacy unknown inventory and unrelated project settings", "[unit][application][authoring-migration]") {
    auto root = Metadata();
    auto legacy = PortableNetwork();
    legacy["contractVersion"] = 2;
    legacy.erase("replication");
    root["settings"]["network"] = legacy;
    auto changed = NormalizeMetadata(root);
    REQUIRE(changed.HasValue());
    const auto candidate = Replacement(changed.Value());
    REQUIRE(candidate["settings"]["unrelatedOwnedSetting"] == root["settings"]["unrelatedOwnedSetting"]);
    REQUIRE(candidate["settings"]["network"]["contractVersion"] == 3);
    REQUIRE(candidate["settings"]["network"]["replication"]["completeness"] == 0);
    REQUIRE(candidate["settings"]["network"]["replication"]["declarations"].empty());
    REQUIRE(candidate["horoVersion"] == root["horoVersion"]);
    REQUIRE(candidate["persistentContract"] == root["persistentContract"]);
}

TEST_CASE("0.2 migration preserves explicit authored complete empty rather than deriving it from absence",
          "[unit][application][authoring-migration]") {
    auto root = Metadata();
    root["settings"]["network"] = PortableNetwork();
    auto changed = NormalizeMetadata(root);
    REQUIRE(changed.HasValue());
    REQUIRE(Replacement(changed.Value())["settings"]["network"] == root["settings"]["network"]);
}

TEST_CASE("0.2 migration rejects malformed carried Network policy without a replacement", "[unit][application][authoring-migration]") {
    auto root = Metadata();
    root["settings"]["network"] = PortableNetwork();
    root["settings"]["network"]["unregisteredPolicy"] = true;
    REQUIRE(NormalizeMetadata(root).HasError());
}

TEST_CASE("0.2 Navigation migration preserves optional opaque authoring and invalidates generated quarantine",
          "[unit][application][authoring-migration]") {
    const auto original = LegacyNavigation(false);
    const auto definition = AuthoringMigration();
    auto changed =
        definition.pipeline->Nodes()[1].documentStage->Execute({.handle = {2, 1}, .path = "Assets/navigation.horoasset", .bytes = original},
                                                               {}, {});
    REQUIRE(changed.HasValue());
    REQUIRE(changed.Value().changed);
    const Navigation::NavigationSourceLoadContext context{.unknownPolicy =
                                                              Navigation::NavigationUnknownRecordPolicy::PreserveOptionalInert};
    const auto decoded = Navigation::DeserializeNavigationSourceRecords(changed.Value().replacement, context);
    REQUIRE(decoded.HasValue());
    REQUIRE(decoded.Value().Records().size() == 1);
    REQUIRE(decoded.Value().Records().front().id.Value() == 12);
    REQUIRE(decoded.Value().Records().front().payload == std::vector<std::byte>{std::byte{0x42}, std::byte{0x71}});
    REQUIRE(decoded.Value().Records().front().opaque);
    REQUIRE_FALSE(decoded.Value().HasQuarantinedGeneratedPayloads());
    REQUIRE(decoded.Value().GeneratedPayloads().empty());
}

TEST_CASE("0.2 Navigation migration rejects required unknown semantics and cancellation without rewriting source",
          "[unit][application][authoring-migration]") {
    const auto original = LegacyNavigation(true);
    const auto definition = AuthoringMigration();
    const ProjectDocumentView source{.handle = {2, 1}, .path = "Assets/navigation.horoasset", .bytes = original};
    REQUIRE(definition.pipeline->Nodes()[1].documentStage->Execute(source, {}, {}).HasError());
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    REQUIRE(definition.pipeline->Nodes()[1].documentStage->Execute(source, {}, cancellation.Token()).HasError());
    REQUIRE(original == LegacyNavigation(true));
}
