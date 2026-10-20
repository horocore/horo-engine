#include "Horo/Runtime/Scene/NavigationSceneComponents.h"

#include <catch2/catch_test_macros.hpp>

using namespace Horo;
using namespace Horo::Runtime;

namespace {
    template <typename T> T Id(const std::uint64_t value) {
        return T::Create(value).Value();
    }

    /** @brief Owns only the Runtime validation boundary's borrowed source/collider projection, with no authoring-model dependency. */
    struct BindingFixture final {
        std::optional<NavigationSurfaceComponent> surface;
        std::vector<ColliderComponent> colliders;
        std::vector<NavigationSourceComponent> sources;

        explicit BindingFixture(const std::uint64_t owner = 1) {
            const auto asset = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001").Value();
            surface = NavigationSurfaceComponent{.id = Id<Navigation::SurfaceId>(10),
                                                 .definition = asset,
                                                 .profiles = {Id<Navigation::NavigationAgentProfileId>(1)}};
            colliders.push_back({.id = {2},
                                 .collider = {3},
                                 .body = {{owner}, {1}},
                                 .collisionProfile = Physics::CollisionProfileId::FromBytes(asset.Bytes()),
                                 .materials = {{Physics::PhysicsMaterialSlotId::FromValue(1), asset}}});
            sources.push_back({.collider = {3},
                               .surface = Id<Navigation::SurfaceId>(10),
                               .area = Id<Navigation::NavigationAreaId>(3),
                               .filter = Id<Navigation::NavigationFilterId>(4)});
        }

        NavigationSceneComponentView View() const {
            return {.surface = surface ? &*surface : nullptr, .sources = sources, .colliders = colliders};
        }
    };
}  // namespace

TEST_CASE("Navigation sources bind only exact local colliders and existing Scene surfaces", "[unit][navigation][scene][source]") {
    BindingFixture object;
    const auto validate = [&] {
        const std::array views{object.View()};
        return ValidateNavigationSceneComponentViews(views);
    };
    REQUIRE(validate().HasValue());
    SECTION("missing collider") {
        object.sources.front().collider = {99};
    }
    SECTION("ambiguous collider") {
        object.colliders.push_back(object.colliders.front());
    }
    SECTION("missing surface") {
        object.surface.reset();
    }
    SECTION("duplicate source binding") {
        object.sources.push_back(object.sources.front());
    }
    SECTION("invalid area") {
        object.sources.front().area = {};
    }
    SECTION("invalid filter") {
        object.sources.front().filter = {};
    }
    SECTION("invalid generation") {
        object.sources.front().generation = 0;
    }
    CHECK(validate().HasError());
}

TEST_CASE("Navigation source collider identities remain object-local while one surface retains one bake filter",
          "[unit][navigation][scene][source]") {
    BindingFixture first{1};
    BindingFixture second{2};
    second.surface.reset();
    const auto validate = [&] {
        const std::array views{first.View(), second.View()};
        return ValidateNavigationSceneComponentViews(views);
    };
    REQUIRE(validate().HasValue());
    second.sources.front().filter = Id<Navigation::NavigationFilterId>(6);
    CHECK(validate().HasError());
    second.sources.front().enabled = false;
    CHECK(validate().HasError());
}

TEST_CASE("Navigation source admission bounds local and complete capture counts before source traversal",
          "[unit][navigation][scene][source]") {
    BindingFixture object;
    object.sources.resize(MaximumNavigationSourcesPerObject + 1, object.sources.front());
    CHECK(ValidateNavigationSourceBindings(object.sources, object.colliders).HasError());
    object.sources.resize(MaximumNavigationSourcesPerObject);
    constexpr auto count = Navigation::NavigationSourceGeometryLimits::MaximumContributions / MaximumNavigationSourcesPerObject + 1;
    const std::vector views(count, object.View());
    CHECK(ValidateNavigationSceneComponentViews(views).HasError());
}
