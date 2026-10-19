#include "PrefabTemplateProviderTestSupport.h"

namespace Horo::Prefab {
    using namespace TestSupport;

    TEST_CASE("Runtime provider rejects incomplete cooked occurrence projections before queuing", "[prefab][provider][projection]") {
        ProjectionFixture projection;
        auto &options = projection.options;
        SECTION("missing member table") {
            options.members.clear();
        }
        SECTION("dropped cooked member") {
            options.members[1].clear();
            projection.components[1].gameplayComponents.clear();
        }
        SECTION("substituted behavior schema") {
            options.members[0][0].type = Gameplay::BehaviorTypeId::Parse("game.tests.other").Value();
        }
        SECTION("substituted raw occurrence") {
            options.members[1][0].instance = 8;
        }
        projection.RejectWithoutQueuing();
    }

    TEST_CASE("Runtime provider rejects omitted or redirected cooked reference projections before queuing",
              "[prefab][provider][projection]") {
        ProjectionFixture projection;
        auto &options = projection.options;
        SECTION("missing reference table") {
            options.references.clear();
        }
        SECTION("dropped reference") {
            options.references[0].clear();
        }
        SECTION("substituted reference property") {
            options.references[0][0].property = 2;
        }
        SECTION("redirected internal reference") {
            options.references[0][0].target = Runtime::RuntimeGroupEntitySlot{0};
        }
        SECTION("redirected internal member") {
            options.references[0][1].target = Runtime::RuntimeGroupMemberSlot{{0}, 0};
        }
        SECTION("substituted asset dependency") {
            options.references[0][2].target = Assets::AssetDependency{Test::Asset(3), Type("core.mesh")};
        }
        SECTION("unbound required interface") {
            options.references[0][3].target = std::monostate{};
        }
        SECTION("changed external component constraint") {
            options.references[0][4].target = Runtime::RuntimeGroupExternalReference{projection.external, {}};
        }
        projection.RejectWithoutQueuing();
    }

    TEST_CASE("Runtime provider accepts fresh behavior IDs and complete portable reference classes", "[prefab][provider][projection]") {
        ProjectionFixture projection;
        SECTION("unbound optional interface") {
            // Keep the optional interface unbound as admitted by the cooked declaration.
        }
        SECTION("bound optional interface retains its declared component constraint") {
            projection.options.references[0][4].target =
                Runtime::RuntimeGroupExternalReference{projection.external, projection.components[1].gameplayComponents[0].typeId};
        }
        REQUIRE(projection.fixture.provider
                    .QueuePreparedGroup(projection.lease, std::move(projection.components), {}, std::move(projection.options))
                    .HasValue());
        CHECK(projection.fixture.scenes.ActiveScene()->SlotCount() == 0);
        // Queue admission is separate from Scene's live-target and subsystem-owner publication checks.
    }

}  // namespace Horo::Prefab
