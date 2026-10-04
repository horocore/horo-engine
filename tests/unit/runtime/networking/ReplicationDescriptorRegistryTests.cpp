#include "Horo/Network/ReplicationDescriptorRegistry.h"
#include "ReplicationDescriptorTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <utility>

namespace Horo::Network {
    using namespace TestSupport;

    namespace {
        ReplicationDescriptorSnapshotPtr InitialSnapshot() {
            const std::array descriptors{Schema(10, {Field(2), Field(1)})};
            return BuildReplicationDescriptorSnapshot(descriptors, Limits).Value();
        }
    }  // namespace

    TEST_CASE("Replication snapshot owns and canonicalizes complete descriptor input", "[unit][network][replication][registry]") {
        ReplicationDescriptorSnapshotPtr snapshot;
        {
            const std::array descriptors{Schema(20, {Field(3)}), Schema(10, {Field(2), Field(1)})};
            snapshot = BuildReplicationDescriptorSnapshot(descriptors, Limits).Value();
        }

        REQUIRE(snapshot->Schemas().size() == 2);
        REQUIRE(snapshot->Schemas()[0].id == SchemaId(10));
        REQUIRE(snapshot->Schemas()[0].fields[0].id == FieldIdValue(1));
        REQUIRE(snapshot->Schemas()[0].fields[1].id == FieldIdValue(2));
        REQUIRE(snapshot->Find(SchemaId(10)).Value() == &snapshot->Schemas()[0]);
        RequireError(snapshot->Find(SchemaId(999)), NetworkErrors::ReplicationSchemaUnknown);
    }

    TEST_CASE("Schema fingerprint ignores contribution and storage order", "[unit][network][replication][registry]") {
        const std::array first{Schema(20, {Field(3)}), Schema(10, {Field(2), Field(1)})};
        const std::array second{Schema(10, {Field(1), Field(2)}), Schema(20, {Field(3)})};
        const auto firstSnapshot = BuildReplicationDescriptorSnapshot(first, Limits).Value();
        const auto secondSnapshot = BuildReplicationDescriptorSnapshot(second, Limits).Value();
        REQUIRE(firstSnapshot->Fingerprint() == secondSnapshot->Fingerprint());

        struct OldStorage final {
            int health;
        };

        struct RefactoredStorage final {
            int unrelated;
            int renamedHealth;
        };

        static_assert(offsetof(OldStorage, health) != offsetof(RefactoredStorage, renamedHealth));
        REQUIRE(firstSnapshot->Schemas()[0].fields[0].id == secondSnapshot->Schemas()[0].fields[0].id);
    }

    TEST_CASE("Schema fingerprint changes with wire semantics", "[unit][network][replication][registry]") {
        const auto initial = InitialSnapshot();
        auto changed = Schema(10, {Field(2), Field(1)});
        changed.fields.front().codec = ReplicationCodecId::Create(2).Value();
        const std::array descriptors{changed};
        const auto replacement = BuildReplicationDescriptorSnapshot(descriptors, Limits).Value();
        REQUIRE(initial->Fingerprint() != replacement->Fingerprint());
    }

    TEST_CASE("Snapshot construction rejects duplicate schemas and aggregate default capacity", "[unit][network][replication][registry]") {
        const std::array duplicates{Schema(10, {Field(1)}), Schema(10, {Field(2)})};
        RequireError(BuildReplicationDescriptorSnapshot(duplicates, Limits), NetworkErrors::ReplicationDescriptorConflict);

        auto optional = Field(1);
        optional.requirement = ReplicationFieldRequirement::Optional;
        optional.canonicalDefault = ReplicationFieldDefault{std::vector<std::byte>(8)};
        const std::array descriptors{Schema(10, {optional}), Schema(20, {optional})};
        ReplicationDescriptorLimits narrow = Limits;
        narrow.maximumTotalDefaultBytes = 15;
        RequireError(BuildReplicationDescriptorSnapshot(descriptors, narrow), NetworkErrors::ReplicationCapacityExceeded);
    }

    TEST_CASE("Compatible minor replacement admits only optional defaulted additions", "[unit][network][replication][registry]") {
        const auto previous = InitialSnapshot();
        auto replacement = Schema(10, {Field(1), Field(2)});
        replacement.version = {1, 1};
        replacement.compatibility.maximum = replacement.version;
        auto added = Field(3, {1, 1});
        added.requirement = ReplicationFieldRequirement::Optional;
        added.canonicalDefault = ReplicationFieldDefault{{std::byte{0x2A}}};
        replacement.fields.push_back(added);
        const std::array descriptors{replacement};

        const auto result = BuildReplicationDescriptorReplacement(previous, descriptors, Limits);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value() != previous);
        REQUIRE((previous->Schemas().front().version == ReplicationSchemaVersion{1, 0}));
        REQUIRE((result.Value()->Schemas().front().version == ReplicationSchemaVersion{1, 1}));
    }

    TEST_CASE("Replacement rejects stable field semantic changes and preserves prior snapshot", "[unit][network][replication][registry]") {
        const auto previous = InitialSnapshot();
        const Sha256Digest priorFingerprint = previous->Fingerprint();
        auto replacement = Schema(10, {Field(1), Field(2)});
        replacement.version = {1, 1};
        replacement.compatibility.maximum = replacement.version;
        replacement.fields.front().valueType = ReplicationValueTypeId::Create(9).Value();
        const std::array descriptors{replacement};

        RequireError(BuildReplicationDescriptorReplacement(previous, descriptors, Limits),
                     NetworkErrors::ReplicationDescriptorIncompatible);
        REQUIRE(previous->Fingerprint() == priorFingerprint);
        REQUIRE(previous->Schemas().front().fields.front().valueType == ReplicationValueTypeId::Create(1).Value());
    }

    TEST_CASE("Replacement tombstones removals but cannot remove required fields in a compatible minor",
              "[unit][network][replication][registry]") {
        const auto previous = InitialSnapshot();
        auto compatibleRemoval = Schema(10, {Field(1)});
        compatibleRemoval.version = {1, 1};
        compatibleRemoval.compatibility.maximum = compatibleRemoval.version;
        compatibleRemoval.tombstonedFields = {FieldIdValue(2)};
        const std::array compatibleDescriptors{compatibleRemoval};
        RequireError(BuildReplicationDescriptorReplacement(previous, compatibleDescriptors, Limits),
                     NetworkErrors::ReplicationDescriptorIncompatible);

        auto majorRemoval = compatibleRemoval;
        majorRemoval.version = {2, 0};
        majorRemoval.compatibility = {.minimum = {2, 0}, .maximum = {2, 0}};
        const std::array majorDescriptors{majorRemoval};
        REQUIRE(BuildReplicationDescriptorReplacement(previous, majorDescriptors, Limits).HasValue());
    }

    TEST_CASE("Replacement rejects schema removal tombstone reuse and null prior lifecycle", "[unit][network][replication][registry]") {
        const auto previous = InitialSnapshot();
        const std::array<ReplicationSchemaDescriptor, 1> missing{Schema(20, {Field(1)})};
        RequireError(BuildReplicationDescriptorReplacement(previous, missing, Limits), NetworkErrors::ReplicationDescriptorIncompatible);

        auto tombstoned = Schema(10, {Field(1)});
        tombstoned.version = {2, 0};
        tombstoned.compatibility = {.minimum = {2, 0}, .maximum = {2, 0}};
        tombstoned.tombstonedFields = {FieldIdValue(2)};
        const std::array retiredDescriptors{tombstoned};
        const auto retired = BuildReplicationDescriptorReplacement(previous, retiredDescriptors, Limits).Value();

        auto reused = tombstoned;
        reused.version = {3, 0};
        reused.compatibility = {.minimum = {3, 0}, .maximum = {3, 0}};
        reused.fields = {Field(2, {3, 0})};
        reused.tombstonedFields = {FieldIdValue(1)};
        const std::array reusedDescriptors{reused};
        RequireError(BuildReplicationDescriptorReplacement(retired, reusedDescriptors, Limits),
                     NetworkErrors::ReplicationDescriptorIncompatible);

        RequireError(BuildReplicationDescriptorReplacement({}, retiredDescriptors, Limits), NetworkErrors::ReplicationDescriptorInvalid);
    }

    TEST_CASE("Replacement collects attributed failures across every schema without publishing", "[unit][network][replication][registry]") {
        const std::array original{Schema(30), Schema(10, {Field(2), Field(1)}), Schema(20)};
        const auto previous = BuildReplicationDescriptorSnapshot(original, Limits).Value();
        const auto fingerprint = previous->Fingerprint();
        auto changed = Schema(10, {Field(1)});
        changed.version = {1, 1};
        changed.compatibility.maximum = changed.version;
        changed.fields.front().codec = Codec(2);
        auto foreign = Schema(20);
        foreign.owner.value = "game.foreign";
        const std::array candidate{foreign, changed};

        const auto result = BuildReplicationDescriptorReplacement(previous, candidate, Limits, 16);
        RequireError(result, NetworkErrors::ReplicationDescriptorIncompatible);
        const auto &diagnostics = result.ErrorValue().diagnostics;
        REQUIRE(diagnostics.size() == 6);
        REQUIRE(diagnostics[0].path == "schemas[10].fields[1]");
        REQUIRE(diagnostics[1].path == "schemas[10].fields[2]");
        REQUIRE(diagnostics[2].path == "schemas[10].fields[2]");
        REQUIRE(diagnostics[3].path == "schemas[20]");
        REQUIRE(diagnostics[4].path == "schemas[20]");
        REQUIRE(diagnostics[5].path == "schemas[30]");
        REQUIRE(diagnostics[0].code.Value() == "replication.field_semantics_changed");
        REQUIRE(diagnostics[1].code.Value() == "replication.field_retirement_missing");
        REQUIRE(diagnostics[2].code.Value() == "replication.required_field_removed");
        REQUIRE(diagnostics[3].code.Value() == "replication.owner_mismatch");
        REQUIRE(diagnostics[4].code.Value() == "replication.unversioned_change");
        REQUIRE(diagnostics[5].code.Value() == "replication.schema_missing");
        REQUIRE(diagnostics.front().message.find("game.replication") != std::string::npos);
        REQUIRE(diagnostics.front().message.find("prior 1.0, candidate 1.1") != std::string::npos);
        REQUIRE(previous->Fingerprint() == fingerprint);
        REQUIRE(previous->Schemas().size() == 3);
        REQUIRE(previous->Schemas().front().fields.front().codec == Codec(1));

        const std::array reordered{changed, foreign};
        const auto repeated = BuildReplicationDescriptorReplacement(previous, reordered, Limits, 16);
        RequireError(repeated, NetworkErrors::ReplicationDescriptorIncompatible);
        REQUIRE(repeated.ErrorValue().diagnostics.size() == diagnostics.size());
        for (std::size_t index = 0; index < diagnostics.size(); ++index) {
            REQUIRE(repeated.ErrorValue().diagnostics[index].code.Value() == diagnostics[index].code.Value());
            REQUIRE(repeated.ErrorValue().diagnostics[index].path == diagnostics[index].path);
            REQUIRE(repeated.ErrorValue().diagnostics[index].message == diagnostics[index].message);
        }
    }

    TEST_CASE("Replacement reports version regression and every erased retirement without retiring the old generation",
              "[unit][network][replication][registry]") {
        auto original = Schema();
        original.version = {2, 0};
        original.compatibility = {original.version, original.version};
        original.tombstonedFields = {FieldIdValue(2), FieldIdValue(3)};
        const std::array descriptors{original};
        const auto previous = BuildReplicationDescriptorSnapshot(descriptors, Limits).Value();
        const auto fingerprint = previous->Fingerprint();
        const std::array candidate{Schema()};
        const auto rejected = BuildReplicationDescriptorReplacement(previous, candidate, Limits, 8);
        RequireError(rejected, NetworkErrors::ReplicationDescriptorIncompatible);
        const auto &diagnostics = rejected.ErrorValue().diagnostics;
        REQUIRE(diagnostics.size() == 3);
        REQUIRE(diagnostics[0].code.Value() == "replication.version_regression");
        REQUIRE(diagnostics[1].code.Value() == "replication.tombstone_missing");
        REQUIRE(diagnostics[2].code.Value() == "replication.tombstone_missing");
        REQUIRE(diagnostics[1].path == "schemas[10].fields[2]");
        REQUIRE(diagnostics[2].path == "schemas[10].fields[3]");
        REQUIRE(previous->Fingerprint() == fingerprint);
        REQUIRE(previous->Schemas().front().tombstonedFields == original.tombstonedFields);
    }

    TEST_CASE("Replacement diagnostic overflow is deterministic and always rejects", "[unit][network][replication][registry]") {
        const std::array original{Schema(20), Schema(10)};
        const auto previous = BuildReplicationDescriptorSnapshot(original, Limits).Value();
        const std::array candidate{Schema(30)};
        const auto result = BuildReplicationDescriptorReplacement(previous, candidate, Limits, 1);
        RequireError(result, NetworkErrors::ReplicationDescriptorIncompatible);
        REQUIRE(result.ErrorValue().diagnostics.size() == 2);
        REQUIRE(result.ErrorValue().diagnostics.front().path == "schemas[10]");
        REQUIRE(result.ErrorValue().diagnostics.back().message.find("limit") != std::string::npos);
        RequireError(BuildReplicationDescriptorReplacement(previous, candidate, Limits, 0), NetworkErrors::ReplicationDescriptorInvalid);
        REQUIRE(previous->Schemas().size() == 2);
    }
}  // namespace Horo::Network
