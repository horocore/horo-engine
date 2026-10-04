#include "Horo/Network/NetworkProjectSettings.h"
#include "NetworkProjectSettingsTestSupport.h"
#include "ReplicationDescriptorTestSupport.h"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>

namespace Horo::Network {
    using namespace TestSupport;

    namespace {
        NetworkProjectSettingsInput Project() {
            auto input = DefaultNetworkProjectSettings(NetworkProjectSettingsId::Create(99).Value()).Value();
            input.supportedRoles = NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::Client | NetworkProjectRoleSet::ListenServer |
                                   NetworkProjectRoleSet::DedicatedServer;
            return input;
        }

        NetworkProjectSettingsInput DeclaredProject() {
            auto input = Project();
            input.replication.declarations = {{Schema()}};
            return input;
        }
    }  // namespace

    TEST_CASE("Network inventory migration never certifies legacy unknown as complete empty", "[unit][network][settings][inventory]") {
        const auto complete = NetworkProjectSettings::Create(Project()).Value();
        const auto legacy = LegacyNetworkSettingsDocument(complete, LegacyNetworkSettingsVersion::Two);
        const auto migrated = ParseNetworkProjectSettings(legacy.dump());
        REQUIRE(migrated.HasValue());
        REQUIRE(migrated.Value().replication.completeness == NetworkReplicationInventoryCompleteness::Unknown);
        const auto unknown = NetworkProjectSettings::Create(migrated.Value()).Value();
        REQUIRE(unknown.Fingerprint() != complete.Fingerprint());
        REQUIRE(ParseNetworkProjectSettings(SerializeNetworkProjectSettings(unknown)).Value().replication.completeness ==
                NetworkReplicationInventoryCompleteness::Unknown);
        TransportCapabilities absent;
        REQUIRE(PreflightNetworkProjectSettings(unknown, NetworkProjectRole::Standalone, absent).HasValue());
        for (const auto role : {NetworkProjectRole::Client, NetworkProjectRole::ListenServer, NetworkProjectRole::DedicatedServer}) {
            const auto denied = PreflightNetworkProjectSettings(unknown, role, absent);
            REQUIRE(denied.HasError());
            REQUIRE(denied.ErrorValue().diagnostics.front().code.Value() == "replication.inventory.incomplete");
        }
    }

    TEST_CASE("Network inventory codec owns and canonicalizes every inert schema semantic", "[unit][network][settings][inventory]") {
        auto input = Project();
        auto schema = Schema(20, {Field(2), Field(1)});
        schema.fields.front().condition = ReplicationCondition::Custom;
        schema.fields.front().customCondition = ReplicationConditionId::Create(9).Value();
        schema.fields.front().requirement = ReplicationFieldRequirement::Optional;
        schema.fields.front().canonicalDefault = ReplicationFieldDefault{{std::byte{0}, std::byte{255}}};
        schema.tombstonedFields = {FieldIdValue(4), FieldIdValue(3)};
        input.replication.declarations = {{schema, ReplicationDeclarationRequirement::Optional}, {Schema(10)}};
        const auto settings = NetworkProjectSettings::Create(input).Value();
        const auto encoded = SerializeNetworkProjectSettings(settings);
        const auto decoded = ParseNetworkProjectSettings(encoded);
        REQUIRE(decoded.HasValue());
        const auto roundTrip = NetworkProjectSettings::Create(decoded.Value()).Value();
        REQUIRE(roundTrip.Fingerprint() == settings.Fingerprint());
        REQUIRE(SerializeNetworkProjectSettings(roundTrip) == encoded);
        const auto &canonical = settings.ReplicationInventory().declarations;
        REQUIRE(canonical.front().schema.id == SchemaId(10));
        REQUIRE(canonical.back().schema.fields.front().id == FieldIdValue(1));
        REQUIRE(canonical.back().schema.tombstonedFields.front() == FieldIdValue(3));
        REQUIRE(canonical.back().schema.fields.back().canonicalDefault == schema.fields.front().canonicalDefault);
        REQUIRE(canonical.back().schema.fields.back().customCondition == schema.fields.front().customCondition);
        std::ranges::reverse(input.replication.declarations);
        std::ranges::reverse(input.replication.declarations.front().schema.fields);
        const auto reordered = NetworkProjectSettings::Create(input).Value();
        REQUIRE(reordered.Fingerprint() == settings.Fingerprint());
        REQUIRE(SerializeNetworkProjectSettings(reordered) == encoded);
        input.replication.declarations.clear();
        REQUIRE(settings.ReplicationInventory().declarations.size() == 2);
    }

    TEST_CASE("Network inventory fingerprints include requirements ownership versions fields and tombstones",
              "[unit][network][settings][inventory]") {
        auto input = DeclaredProject();
        const auto original = NetworkProjectSettings::Create(input).Value().Fingerprint();
        SECTION("requirement") {
            input.replication.declarations.front().requirement = ReplicationDeclarationRequirement::Optional;
        }
        SECTION("owner") {
            input.replication.declarations.front().schema.owner.value = "game.other";
        }
        SECTION("identity") {
            input.replication.declarations.front().schema.id = SchemaId(11);
        }
        SECTION("version") {
            input.replication.declarations.front().schema.version = {1, 1};
            input.replication.declarations.front().schema.compatibility.maximum = {1, 1};
        }
        SECTION("codec") {
            input.replication.declarations.front().schema.fields.front().codec = Codec(2);
        }
        SECTION("retirement") {
            input.replication.declarations.front().schema.tombstonedFields = {FieldIdValue(2)};
        }
        REQUIRE(NetworkProjectSettings::Create(input).Value().Fingerprint() != original);
    }

    TEST_CASE("Network inventory codec rejects malformed identities and executable-shaped fields", "[unit][network][settings][inventory]") {
        auto hostile = nlohmann::json::parse(SerializeNetworkProjectSettings(NetworkProjectSettings::Create(DeclaredProject()).Value()));
        SECTION("native layout") {
            hostile["replication"]["declarations"][0]["schema"]["nativeOffset"] = 8;
        }
        SECTION("zero identity") {
            hostile["replication"]["declarations"][0]["schema"]["id"] = 0;
        }
        SECTION("float identity") {
            hostile["replication"]["declarations"][0]["schema"]["fields"][0]["codec"] = 1.0;
        }
        SECTION("uppercase default") {
            hostile["replication"]["declarations"][0]["schema"]["fields"][0]["canonicalDefault"] = "FF";
        }
        SECTION("unknown with declarations") {
            hostile["replication"]["completeness"] = static_cast<unsigned>(NetworkReplicationInventoryCompleteness::Unknown);
        }
        REQUIRE(ParseNetworkProjectSettings(hostile.dump()).HasError());
    }

    TEST_CASE("Network inventory authority retains exact old identity revision and schemas across replacement and shutdown",
              "[unit][network][settings][inventory]") {
        auto initial = DeclaredProject();
        auto authority = NetworkProjectSettingsAuthority::Create(initial).Value();
        const auto old = authority.Snapshot();
        auto successor = initial;
        successor.revision = NetworkProjectSettingsRevision::Create(2).Value();
        successor.replication.declarations.front().schema.fields.front().codec = Codec(2);
        const auto published = authority.Apply({old.settings.Revision(), successor});
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().settings.Settings() == old.settings.Settings());
        REQUIRE(published.Value().settings.Revision() != old.settings.Revision());
        REQUIRE(published.Value().settings.Fingerprint() != old.settings.Fingerprint());
        REQUIRE(old.settings.ReplicationInventory().declarations.front().schema.fields.front().codec == Codec(1));
        successor.revision = NetworkProjectSettingsRevision::Create(3).Value();
        successor.replication.completeness = NetworkReplicationInventoryCompleteness::Unknown;
        REQUIRE(authority.Apply({published.Value().settings.Revision(), successor}).HasError());
        REQUIRE(authority.Snapshot().settings.Fingerprint() == published.Value().settings.Fingerprint());
        authority.Shutdown();
        successor.replication.completeness = NetworkReplicationInventoryCompleteness::Complete;
        REQUIRE(authority.Apply({published.Value().settings.Revision(), successor}).HasError());
        REQUIRE(authority.Snapshot().settings.Fingerprint() == published.Value().settings.Fingerprint());
    }

    TEST_CASE("Network inventory codec enforces exact unsigned identity widths", "[unit][network][settings][inventory]") {
        auto input = DeclaredProject();
        auto &schema = input.replication.declarations.front().schema;
        schema.id = SchemaId(std::numeric_limits<std::uint64_t>::max());
        auto &field = schema.fields.front();
        field.id = FieldIdValue(std::numeric_limits<std::uint32_t>::max());
        field.valueType = ValueType(std::numeric_limits<std::uint32_t>::max());
        field.codec = Codec(std::numeric_limits<std::uint32_t>::max());
        field.condition = ReplicationCondition::Custom;
        field.customCondition = ReplicationConditionId::Create(std::numeric_limits<std::uint32_t>::max()).Value();
        const auto settings = NetworkProjectSettings::Create(input);
        REQUIRE(settings.HasValue());
        auto document = nlohmann::json::parse(SerializeNetworkProjectSettings(settings.Value()));
        REQUIRE(ParseNetworkProjectSettings(document.dump()).HasValue());
        auto &wireSchema = document["replication"]["declarations"][0]["schema"];
        SECTION("negative schema") {
            wireSchema["id"] = -1;
        }
        SECTION("negative field") {
            wireSchema["fields"][0]["id"] = -1;
        }
        SECTION("field overflow") {
            wireSchema["fields"][0]["id"] = std::uint64_t{1} << 32;
        }
        SECTION("codec overflow") {
            wireSchema["fields"][0]["codec"] = std::uint64_t{1} << 32;
        }
        SECTION("value type overflow") {
            wireSchema["fields"][0]["valueType"] = std::uint64_t{1} << 32;
        }
        SECTION("condition overflow") {
            wireSchema["fields"][0]["customCondition"] = std::uint64_t{1} << 32;
        }
        SECTION("version overflow") {
            wireSchema["version"]["major"] = std::uint64_t{1} << 16;
        }
        REQUIRE(ParseNetworkProjectSettings(document.dump()).HasError());
    }

    TEST_CASE("Network inventory canonical default budget spans every schema", "[unit][network][settings][inventory]") {
        auto input = Project();
        const auto &limits = NetworkReplicationInventory::DescriptorLimits;
        const auto count = limits.maximumTotalDefaultBytes / limits.maximumDefaultBytesPerField;
        for (std::size_t index = 0; index <= count; ++index) {
            auto field = Field();
            field.requirement = ReplicationFieldRequirement::Optional;
            field.limits.maximumEncodedBytes = limits.maximumDefaultBytesPerField;
            field.canonicalDefault = ReplicationFieldDefault{std::vector<std::byte>(limits.maximumDefaultBytesPerField)};
            input.replication.declarations.push_back({Schema(index + 1, {field})});
            const auto created = NetworkProjectSettings::Create(input);
            if (index < count) {
                REQUIRE(created.HasValue());
                REQUIRE(ParseNetworkProjectSettings(SerializeNetworkProjectSettings(created.Value())).HasValue());
            } else {
                REQUIRE(created.HasError());
            }
        }
    }

    TEST_CASE("Network inventory fitting alone cannot overflow the combined settings envelope", "[unit][network][settings][inventory]") {
        auto input = Project();
        auto document = nlohmann::json::parse(SerializeNetworkProjectSettings(NetworkProjectSettings::Create(input).Value()));
        bool rejected = false;
        for (std::uint32_t index = 0; index < 512; ++index) {
            const auto fieldIndex = index % 64;
            if (fieldIndex == 0) {
                auto schema = Schema(index / 64 + 1, {});
                input.replication.declarations.push_back({schema});
            }
            input.replication.declarations.back().schema.fields.push_back(Field(fieldIndex + 1));
            const auto created = NetworkProjectSettings::Create(input);
            if (created.HasValue()) {
                document = nlohmann::json::parse(SerializeNetworkProjectSettings(created.Value()));
                REQUIRE(document.dump().size() <= NetworkReplicationInventory::MaximumDocumentBytes);
                REQUIRE(ParseNetworkProjectSettings(document.dump()).HasValue());
                continue;
            }
            REQUIRE(fieldIndex != 0);
            auto &fields = document["replication"]["declarations"].back()["schema"]["fields"];
            auto extra = fields.back();
            extra["id"] = fieldIndex + 1;
            fields.push_back(std::move(extra));
            REQUIRE(document["replication"].dump().size() <= NetworkReplicationInventory::MaximumDocumentBytes);
            REQUIRE(document.dump().size() > NetworkReplicationInventory::MaximumDocumentBytes);
            REQUIRE(ParseNetworkProjectSettings(document.dump()).HasError());
            rejected = true;
            break;
        }
        REQUIRE(rejected);
    }

    TEST_CASE("Network inventory rejects duplicate nested keys schemas and finite envelope overflow",
              "[unit][network][settings][inventory]") {
        auto input = DeclaredProject();
        auto duplicateText = SerializeNetworkProjectSettings(NetworkProjectSettings::Create(input).Value());
        const auto owner = duplicateText.find("\"owner\":\"game.replication\"");
        REQUIRE(owner != std::string::npos);
        duplicateText.insert(owner, "\"owner\":\"game.other\",");
        REQUIRE(ParseNetworkProjectSettings(duplicateText).HasError());
        input.replication.declarations.push_back(input.replication.declarations.front());
        REQUIRE(NetworkProjectSettings::Create(input).HasError());
        input.replication.declarations.resize(NetworkReplicationInventory::DescriptorLimits.maximumSchemas + 1);
        REQUIRE(NetworkProjectSettings::Create(input).HasError());
        input = DeclaredProject();
        input.replication.declarations.front()
            .schema.owner.value.assign(NetworkReplicationInventory::DescriptorLimits.maximumOwnerIdentityBytes + 1, 'x');
        REQUIRE(NetworkProjectSettings::Create(input).HasError());
        input = DeclaredProject();
        auto &field = input.replication.declarations.front().schema.fields.front();
        field.requirement = ReplicationFieldRequirement::Optional;
        field.canonicalDefault =
            ReplicationFieldDefault{std::vector<std::byte>(NetworkReplicationInventory::DescriptorLimits.maximumDefaultBytesPerField + 1)};
        REQUIRE(NetworkProjectSettings::Create(input).HasError());
    }
}  // namespace Horo::Network
