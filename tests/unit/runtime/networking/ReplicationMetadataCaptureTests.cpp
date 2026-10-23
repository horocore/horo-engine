#include "Horo/Network/ReplicationMetadataCapture.h"
#include "ReplicationDescriptorTestSupport.h"

#include <algorithm>
#include <array>

namespace Horo::Network {
    using namespace TestSupport;

    TEST_CASE("Replication metadata capture preserves absent unknown and complete empty policy", "[network][replication][capture]") {
        const auto absent = ReplicationMetadataCapture::Build({}, {}, {});
        REQUIRE(absent.HasValue());
        CHECK_FALSE(absent.Value()->Inventory());
        CHECK_FALSE(absent.Value()->Registered());
        CHECK(absent.Value()->Assess(ReplicationDeclarationUse::Cook).Value().RequireAdmission().HasValue());
        NetworkReplicationInventory unknown;
        const auto legacy = ReplicationMetadataCapture::Build(unknown, {}, {});
        REQUIRE(legacy.HasValue());
        CHECK(legacy.Value()->Inventory()->completeness == NetworkReplicationInventoryCompleteness::Unknown);
        CHECK(legacy.Value()->Assess(ReplicationDeclarationUse::Cook).HasError());
        CHECK_FALSE(legacy.Value()->Assess(ReplicationDeclarationUse::EditorInspection).Value().Admitted());
        unknown.completeness = NetworkReplicationInventoryCompleteness::Complete;
        const auto complete = ReplicationMetadataCapture::Build(unknown, {}, {});
        REQUIRE(complete.HasValue());
        CHECK(complete.Value()->Assess(ReplicationDeclarationUse::Cook).Value().Admitted());
        CHECK_FALSE(complete.Value()->Registered());
        CHECK(absent.Value()->Fingerprint() != complete.Value()->Fingerprint());
        CHECK(legacy.Value()->Fingerprint() != complete.Value()->Fingerprint());
        CHECK(complete.Value()->Assess(ReplicationDeclarationUse::ClientActivation).HasError());
    }

    TEST_CASE("Replication metadata capture owns canonical values independently of source generations", "[network][replication][capture]") {
        std::array schemas{Schema(20), Schema(10)};
        std::array<ModuleId, 3> modules{{{"game.replication"}, {"other.module"}, {"game.replication"}}};
        NetworkReplicationInventory inventory{NetworkReplicationInventoryCompleteness::Complete, {{schemas[1]}, {schemas[0]}}};
        const auto capture = ReplicationMetadataCapture::Build(inventory, schemas, modules);
        REQUIRE(capture.HasValue());
        std::ranges::reverse(schemas);
        std::ranges::reverse(modules);
        std::ranges::reverse(inventory.declarations);
        const auto equivalent = ReplicationMetadataCapture::Build(inventory, schemas, modules);
        REQUIRE(equivalent.HasValue());
        CHECK(equivalent.Value()->Fingerprint() == capture.Value()->Fingerprint());
        CHECK(equivalent.Value()->Registered() != capture.Value()->Registered());
        inventory.declarations.clear();
        schemas[0].owner.value = "changed";
        modules[0].value = "changed";
        CHECK(capture.Value()->Inventory()->declarations.size() == 2);
        CHECK(capture.Value()->Modules().size() == 2);
        CHECK(capture.Value()->Registered()->Schemas().front().owner.value == "game.replication");
        CHECK(capture.Value()->Assess(ReplicationDeclarationUse::Cook).Value().Admitted());
    }

    TEST_CASE("Replication metadata capture keeps missing declarations opaque only for inspection", "[network][replication][capture]") {
        NetworkReplicationInventory inventory{NetworkReplicationInventoryCompleteness::Complete, {{Schema()}}};
        auto required = ReplicationMetadataCapture::Build(inventory, {}, {});
        REQUIRE(required.HasValue());
        const auto inspection = required.Value()->Assess(ReplicationDeclarationUse::EditorInspection);
        REQUIRE(inspection.HasValue());
        CHECK_FALSE(inspection.Value().Admitted());
        REQUIRE(inspection.Value().OpaqueDeclarations().size() == 1);
        CHECK(inspection.Value().Diagnostics().front().problem == ReplicationDeclarationProblem::MissingModule);
        CHECK(required.Value()->Assess(ReplicationDeclarationUse::Cook).Value().RequireAdmission().HasError());
        inventory.declarations.front().requirement = ReplicationDeclarationRequirement::Optional;
        const auto optional = ReplicationMetadataCapture::Build(inventory, {}, {});
        REQUIRE(optional.HasValue());
        CHECK(optional.Value()->Assess(ReplicationDeclarationUse::Cook).Value().Admitted());
        CHECK(optional.Value()->Fingerprint() != required.Value()->Fingerprint());
    }

    TEST_CASE("Replication metadata capture rejects malformed duplicate and oversized generations", "[network][replication][capture]") {
        const std::array duplicate{Schema(), Schema()};
        CHECK(ReplicationMetadataCapture::Build({}, duplicate, {}).HasError());
        const std::array<ModuleId, 1> invalid{{{""}}};
        CHECK(ReplicationMetadataCapture::Build({}, {}, invalid).HasError());
        std::vector<ModuleId> oversized(1025, ModuleId{"game.replication"});
        CHECK(ReplicationMetadataCapture::Build({}, {}, oversized).HasError());
        NetworkReplicationInventory inventory{NetworkReplicationInventoryCompleteness::Unknown, {{Schema()}}};
        CHECK(ReplicationMetadataCapture::Build(inventory, {}, {}).HasError());
    }
}  // namespace Horo::Network
