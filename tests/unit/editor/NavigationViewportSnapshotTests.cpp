#include "Horo/Editor/NavigationViewportSnapshot.h"
#include "navigation/IncrementalBakeFixture.h"
#include "navigation/NavigationSceneSourceFixture.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Editor {
    namespace {
        namespace N = Navigation;
        using N::TestSupport::Id;
        using Tests::NavigationSources::Asset;

        /** @brief Saved typed source with a transformed surface, a cylinder and a directed link. */
        Application::NavigationSceneBakeSource Scene(const std::uint8_t identity = 2) {
            auto object = Tests::NavigationSources::Object();
            object.localTransform.translation = {10, 2, 3};
            auto &surface = *object.components.navigationSurface;
            surface.bakeScope = Runtime::NavigationBakeScope::LocalBounds;
            surface.localBounds = Runtime::NavigationLocalBounds{};
            object.components.navigationModifier =
                Runtime::NavigationModifierComponent{.id = Id<N::NavigationModifierId>(9),
                                                     .surface = surface.id,
                                                     .volume = Runtime::NavigationCylinderVolume{},
                                                     .operation = Runtime::NavigationModifierOperation::Exclude};
            object.components.navigationLink =
                Runtime::NavigationLinkComponent{.id = Id<N::NavigationLinkId>(10),
                                                 .start = {.surface = surface.id, .localPosition = {0, 0, 0}},
                                                 .end = {.surface = surface.id, .localPosition = {4, 0, 0}},
                                                 .profiles = surface.profiles};
            return {.asset = Asset(identity),
                    .path = ProjectPath::Parse("Assets/scene" + std::to_string(identity) + ".horo").Value(),
                    .digest = N::TestSupport::Digest(identity),
                    .document = {.objects = {object}}};
        }
    }  // namespace

    TEST_CASE("Navigation viewport owns transformed authored geometry after all source memory is destroyed",
              "[editor][navigation][overlay]") {
        std::shared_ptr<const NavigationViewportSnapshot> snapshot;
        {
            const std::array scenes{Scene()};
            auto captured = NavigationViewportSnapshot::Capture(scenes, Asset(1));
            REQUIRE(captured.HasValue());
            snapshot = std::move(captured).Value();
        }
        REQUIRE(snapshot->Scenes().size() == 1);
        const auto &scene = snapshot->Scenes().front();
        CHECK(scene.sourceDigest == N::TestSupport::Digest(2));
        CHECK_FALSE(snapshot->Truncated());
        CHECK(std::ranges::count(scene.primitives, NavigationOverlayKind::Surface, &NavigationOverlayPrimitive::kind) == 12);
        CHECK(std::ranges::count(scene.primitives, NavigationOverlayKind::Modifier, &NavigationOverlayPrimitive::kind) == 36);
        const auto link = std::ranges::find(scene.primitives, NavigationOverlayKind::Link, &NavigationOverlayPrimitive::kind);
        REQUIRE(link != scene.primitives.end());
        CHECK(link->points[0] == Math::Vec3{10, 2, 3});
        CHECK(link->points[1] == Math::Vec3{14, 2, 3});
        CHECK(link->validity == NavigationOverlayValidity::Unvalidated);
        CHECK(link->link == Id<N::NavigationLinkId>(10));
        CHECK_FALSE(link->bidirectional);
    }

    TEST_CASE("Navigation viewport resolves parent frames independently of input order and preserves two-way links",
              "[editor][navigation][overlay]") {
        auto scene = Scene();
        scene.document.objects.front().parent = SceneSource::SceneObjectId{2};
        scene.document.objects.front().components.navigationLink->direction = Runtime::NavigationLinkDirection::Bidirectional;
        scene.document.objects.push_back({.id = {2}, .name = "Parent", .localTransform = {.translation = {5, 0, 0}, .scale = {2, 1, 1}}});
        const auto snapshot = NavigationViewportSnapshot::Capture(std::span{&scene, 1}, Asset(1));
        REQUIRE(snapshot.HasValue());
        const auto &primitives = snapshot.Value()->Scenes().front().primitives;
        const auto link = std::ranges::find(primitives, NavigationOverlayKind::Link, &NavigationOverlayPrimitive::kind);
        REQUIRE(link != primitives.end());
        CHECK((link->points[0] == Math::Vec3{25, 2, 3}));
        CHECK((link->points[1] == Math::Vec3{33, 2, 3}));
        CHECK(link->bidirectional);
        NavigationOverlayFilter filter;
        auto destination = *link;
        destination.otherSurface = Id<N::SurfaceId>(11);
        filter.surface = destination.otherSurface;
        CHECK(filter.Matches(destination));
        destination.area = Id<N::NavigationAreaId>(8);
        filter.area = destination.area;
        CHECK(filter.Matches(destination));
    }

    TEST_CASE("Navigation viewport isolates repeated surface identities by Scene and applies every typed filter",
              "[editor][navigation][overlay]") {
        auto first = Scene(2);
        auto second = Scene(3);
        second.document.objects.front().components.navigationSurface->enabled = false;
        const std::array scenes{first, second};
        const auto captured = NavigationViewportSnapshot::Capture(scenes, Asset(1));
        REQUIRE(captured.HasValue());
        REQUIRE(captured.Value()->Scenes().size() == 2);
        CHECK(captured.Value()->Scenes()[0].scope != captured.Value()->Scenes()[1].scope);
        CHECK(captured.Value()->Scenes()[0].path != captured.Value()->Scenes()[1].path);
        auto primitive = captured.Value()->Scenes()[1].primitives.front();
        CHECK(primitive.validity == NavigationOverlayValidity::Disabled);
        NavigationOverlayFilter filter;
        CHECK(filter.Matches(primitive));
        filter.profile = Id<N::NavigationAgentProfileId>(900);
        CHECK_FALSE(filter.Matches(primitive));
        filter.profile = primitive.profile;
        filter.surface = Id<N::SurfaceId>(900);
        CHECK_FALSE(filter.Matches(primitive));
        filter.surface = primitive.surface;
        filter.area = Id<N::NavigationAreaId>(900);
        CHECK_FALSE(filter.Matches(primitive));
        filter.area.reset();
        filter.validity = NavigationOverlayValidity::Cooked;
        CHECK_FALSE(filter.Matches(primitive));
        filter.validity = primitive.validity;
        CHECK(filter.Matches(primitive));
        filter.kinds[0] = false;
        CHECK_FALSE(filter.Matches(primitive));
    }

    TEST_CASE("Navigation capture bounds both geometry and rejected-input work", "[editor][navigation][overlay][budget]") {
        const std::array scenes{Scene()};
        const auto captured = NavigationViewportSnapshot::Capture(scenes, Asset(1), {.maximumPrimitives = 5});
        REQUIRE(captured.HasValue());
        CHECK(captured.Value()->Truncated());
        REQUIRE(captured.Value()->Scenes().size() == 1);
        CHECK(captured.Value()->Scenes().front().primitives.size() == 5);
        const auto exhausted = NavigationViewportSnapshot::Capture(scenes, Asset(1), {.maximumWork = 1});
        REQUIRE(exhausted.HasValue());
        CHECK(exhausted.Value()->Truncated());
        CHECK(exhausted.Value()->Scenes().front().primitives.empty());
        CHECK(NavigationViewportSnapshot::Capture(scenes, Asset(1), {.maximumPrimitives = 0}).HasError());
        CHECK(NavigationViewportSnapshot::Capture(scenes, Asset(1), {.maximumWork = NavigationOverlayBudget::HardMaximumWork + 1})
                  .HasError());
    }

    TEST_CASE("Navigation capture fails closed on cyclic or missing parents", "[editor][navigation][overlay]") {
        auto scene = Scene();
        scene.document.objects.front().parent = scene.document.objects.front().id;
        CHECK(NavigationViewportSnapshot::Capture(std::span{&scene, 1}, Asset(1)).HasError());
        scene.document.objects.front().parent = SceneSource::SceneObjectId{999};
        CHECK(NavigationViewportSnapshot::Capture(std::span{&scene, 1}, Asset(1)).HasError());
    }

    TEST_CASE("Navigation cooked overlay copies exact tile geometry and releases its publication", "[editor][navigation][overlay]") {
        const std::array scenes{Scene()};
        const auto authored = NavigationViewportSnapshot::Capture(scenes, Asset(1)).Value();
        std::shared_ptr<const NavigationViewportSnapshot> snapshot;
        std::weak_ptr<const N::NavigationCookedTile> observed;
        {
            N::TestSupport::IncrementalBakeFixture fixture;
            auto prepared = N::PrepareNavigationBakeTile(*fixture.Input(), fixture.Tiles().front(), fixture.compatibility).Value();
            prepared.tile.key.scope = authored->Scenes().front().scope;
            auto tile = N::NavigationCookedTile::Create(prepared, {.key = prepared.tile.key.tile, .bounds = prepared.tile.bounds});
            REQUIRE(tile.HasValue());
            observed = tile.Value();
            N::NavigationCookedTileSet set{.inputFingerprint = fixture.Input()->Fingerprint(), .tiles = {tile.Value()}};
            const auto cooked = NavigationViewportSnapshot::WithCooked(*authored, set);
            REQUIRE(cooked.HasValue());
            snapshot = cooked.Value();
        }
        CHECK(observed.expired());
        CHECK_FALSE(snapshot->Truncated());
        const auto &primitives = snapshot->Scenes().front().primitives;
        CHECK(std::ranges::count(primitives, NavigationOverlayKind::Tile, &NavigationOverlayPrimitive::kind) == 12);
        CHECK(std::ranges::count(primitives, NavigationOverlayKind::Surface, &NavigationOverlayPrimitive::kind) == 24);
        const auto tile = std::ranges::find(primitives, NavigationOverlayKind::Tile, &NavigationOverlayPrimitive::kind);
        REQUIRE(tile != primitives.end());
        CHECK(tile->tile.has_value());
        CHECK(tile->validity == NavigationOverlayValidity::Cooked);
        CHECK(authored->Scenes().front().primitives.size() == 49);
        CHECK(NavigationViewportSnapshot::WithCooked(*authored, {}, {.maximumPrimitives = 1}).Value()->Truncated());
    }
}  // namespace Horo::Editor
