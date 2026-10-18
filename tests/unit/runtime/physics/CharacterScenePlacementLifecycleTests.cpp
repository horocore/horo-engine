#include "CharacterScenePlacement.h"
#include "CharacterWorldTestHelpers.h"

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        TEST_CASE("Prepared Character placement publishes exactly once without activating or advancing the world",
                  "[physics][character][scene][placement]") {
            auto world = PreparedWorld(1);
            auto controller = world->CreateController(ControllerDescriptor(world->Descriptor()));
            REQUIRE(controller.HasValue());
            OverlapProbe clear;
            const Math::Quaternion heading{0, 0.70710677F, 0, 0.70710677F};
            auto spawned = Detail::CharacterScenePlacement::Spawn(*world, controller.Value(), clear.Context(world->Descriptor()), heading);
            REQUIRE(spawned.HasValue());
            CHECK(world->State() == CharacterWorldState::Prepared);
            CHECK(world->PublishedTick().completedTick == 0);
            const auto pose = world->ControllerTransform(controller.Value());
            REQUIRE(pose.HasValue());
            CHECK(pose.Value().heading == heading);
            CHECK(pose.Value().sourceTick == 0);
            const auto revision = pose.Value().publicationRevision;
            RequireError(Detail::CharacterScenePlacement::Spawn(*world, controller.Value(), clear.Context(world->Descriptor()), heading),
                         CharacterErrors::InvalidState);
            CHECK(world->ControllerTransform(controller.Value()).Value().publicationRevision == revision);
            REQUIRE(world->Activate().HasValue());
            CHECK(world->ControllerTransform(controller.Value()).Value().publicationRevision == revision);
        }

        TEST_CASE("Prepared Character placement rejects an actual lifecycle transition during its query callback",
                  "[physics][character][scene][placement]") {
            auto world = PreparedWorld(1);
            auto controller = world->CreateController(ControllerDescriptor(world->Descriptor()));
            REQUIRE(controller.HasValue());

            struct Probe final {
                CharacterWorld &world;
                bool shutdown{};
                std::optional<Result<void>> activation;
            } probe{*world};

            SECTION("activation changes the exact entry state") {}
            SECTION("shutdown remains deferred until the in-flight placement returns") {
                probe.shutdown = true;
            }
            OverlapProbe clear;
            auto query = clear.Context(world->Descriptor());
            query.context = &probe;
            query.overlap = [](void *context, const CharacterOverlapProbeRequest &) noexcept {
                auto &state = *static_cast<Probe *>(context);
                if (state.shutdown)
                    state.world.Shutdown();
                else
                    state.activation.emplace(state.world.Activate());
                return Result<CharacterOverlapProbeResult>::Success({});
            };
            RequireError(Detail::CharacterScenePlacement::Spawn(*world, controller.Value(), query, Math::Quaternion::Identity()),
                         CharacterErrors::InvalidState);
            CHECK(world->ControllerTransform(controller.Value()).HasError());
            if (probe.shutdown) {
                CHECK(world->State() == CharacterWorldState::Destroyed);
                CHECK(world->ActiveControllerCount() == 0);
            } else {
                REQUIRE(probe.activation.has_value());
                REQUIRE(probe.activation->HasValue());
                CHECK(world->State() == CharacterWorldState::Active);
                CHECK(world->ActiveControllerCount() == 1);
            }
        }
    }  // namespace
}  // namespace Horo::Character
