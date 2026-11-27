#include "Horo/Gameplay/GameplayFormation.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Gameplay {
    namespace {
        using namespace Navigation;

        struct FormationFixture final {
            NavigationAgentRegistry registry{NavigationAgentRegistry::Create({.maximumAgents = 4}).Value()};
            NavigationAgentSceneBinding binding{NavigationWorldId::Create(1).Value(), NavigationSceneRuntimeId::Create(2).Value(),
                                                NavigationSceneGeneration::Create(3).Value()};
            std::unique_ptr<NavigationAgentSceneCandidate> scene;
            std::array<GameplayFormationMember, 3> members{};
            GameplayFormationRequest request{.group = GameplayFormationId::Create(1).Value(),
                                             .revision = 1,
                                             .spacingMeters = 2.0F,
                                             .forward = {0, 0, 2}};

            FormationFixture() {
                std::array descriptors{Descriptor(1), Descriptor(2), Descriptor(3)};
                scene = registry.PrepareScene(binding, descriptors).Value();
                scene->Publish();
                const auto snapshot = registry.Snapshot().Value();
                for (std::size_t index = 0; index < members.size(); ++index)
                    members[index] = {snapshot.Agents()[index].handle, {10, 1, 5}};
                request.preferredLeader = members[1].agent;
            }

            NavigationAgentDescriptor Descriptor(std::uint32_t entity) const {
                return {.owner = {binding.scene, entity, 1},
                        .profile = NavigationAgentProfileId::Create(1).Value(),
                        .filter = NavigationFilterId::Create(1).Value()};
            }

            Result<GameplayFormationPlan> Prepare() const {
                return PrepareGameplayFormation(request, registry.Snapshot().Value(), members);
            }
        };
    }  // namespace

    TEST_CASE("Formation prepares stable slots around the explicit live leader", "[gameplay][navigation][formation]") {
        FormationFixture fixture;
        const auto leader = fixture.members[1].agent;
        std::ranges::reverse(fixture.members);
        for (const auto shape : {GameplayFormationShape::Line, GameplayFormationShape::Column, GameplayFormationShape::Wedge}) {
            fixture.request.shape = shape;
            const auto built = fixture.Prepare();
            REQUIRE(built.HasValue());
            const auto &plan = built.Value();
            REQUIRE(plan.availability == GameplayFormationAvailability::Complete);
            REQUIRE(plan.slots.size() == 3);
            CHECK(plan.binding == fixture.binding);
            CHECK(plan.slots.front().agent == leader);
            CHECK(plan.slots.front().slot == 0);
            CHECK(plan.slots[1].agent == fixture.members.back().agent);
            CHECK(plan.slots[1].unprojectedTarget.x == (shape == GameplayFormationShape::Column ? 10.0F : 12.0F));
            CHECK(plan.slots[1].unprojectedTarget.z == (shape == GameplayFormationShape::Line ? 5.0F : 3.0F));
        }
    }

    TEST_CASE("Formation replaces a lost leader and excludes recycled agent generations", "[gameplay][navigation][formation]") {
        FormationFixture fixture;
        REQUIRE(fixture.registry.UnregisterAtSafePoint(fixture.members[1].agent).HasValue());
        const auto replacement = fixture.registry.RegisterAtSafePoint(fixture.Descriptor(4));
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value() != fixture.members[1].agent);
        ++fixture.request.revision;
        const auto built = fixture.Prepare();
        REQUIRE(built.HasValue());
        REQUIRE(built.Value().slots.size() == 2);
        CHECK(built.Value().slots[0].agent == fixture.members[0].agent);
        CHECK(built.Value().slots[1].agent == fixture.members[2].agent);
        CHECK(built.Value().revision == 2);
        REQUIRE(fixture.registry.UnregisterAtSafePoint(fixture.members[0].agent).HasValue());
        REQUIRE(fixture.registry.UnregisterAtSafePoint(fixture.members[2].agent).HasValue());
        const auto empty = fixture.Prepare();
        REQUIRE(empty.HasValue());
        CHECK(empty.Value().availability == GameplayFormationAvailability::Dissolved);
        CHECK(empty.Value().slots.empty());
    }

    TEST_CASE("Custom formation assigns reachable-query proposals and reports slot shortages", "[gameplay][navigation][formation]") {
        FormationFixture fixture;
        fixture.request.shape = GameplayFormationShape::Custom;
        fixture.request.forward = {2, 0, 0};
        const std::array offsets{Math::Vec3{1, 0, 2}};
        fixture.request.customSlots = offsets;
        const auto built = fixture.Prepare();
        REQUIRE(built.HasValue());
        const auto &plan = built.Value();
        REQUIRE(plan.availability == GameplayFormationAvailability::Degraded);
        REQUIRE(plan.slots.size() == 2);
        CHECK(plan.slots.back().unprojectedTarget == Math::Vec3{12, 1, 4});
        REQUIRE(plan.unassigned.size() == 1);
        CHECK(plan.unassigned.front() == fixture.members[2].agent);
        const std::array duplicate{offsets.front(), offsets.front()};
        fixture.request.customSlots = duplicate;
        CHECK(fixture.Prepare().HasError());
        fixture.request.customSlots = offsets;
        fixture.members.back() = fixture.members.front();
        CHECK(fixture.Prepare().HasError());
    }
}  // namespace Horo::Gameplay
