#include "navigation/NavigationRegionTestFixture.h"

#include <algorithm>
#include <catch2/generators/catch_generators.hpp>
#include <numbers>

using namespace Horo;
using namespace Horo::Application;

namespace {
    namespace Inputs = NavigationInputTestSupport;
    namespace Regions = RegionTestSupport;
    namespace Nav = Navigation;
    using Nav::TestSupport::RequireError;
    using enum Runtime::NavigationRegionMode;

    /** @brief Capture the complete project closure for region/input integration checks. */
    auto Capture(const Inputs::Project &project, const NavigationProjectBakeInputLimits &limits = {}) {
        return CaptureNavigationProjectBakeInput(project.Capture(), NavigationSourceTestSupport::Asset(1), Inputs::Project::Revisions(),
                                                 limits);
    }

}  // namespace

TEST_CASE("Overlapping include regions form a disjoint union before exclusions", "[navigation][geometry][region]") {
    Inputs::Project project;
    std::vector objects{Inputs::Project::Object(), Regions::Region(1, Include, {2, 0, 4}, {2, 2, 4}),
                        Regions::Region(2, Include, {4, 0, 4}, {2, 2, 4})};
    Regions::Scenes(project, objects);
    const auto included = Regions::First(project);
    REQUIRE(included.HasValue());
    REQUIRE(included.Value().size() == 1);
    CHECK(Regions::TopArea(included.Value().front().geometry) == Catch::Approx(48).margin(1.0e-4));
    objects.push_back(Regions::Region(3, Exclude, {4, 0, 4}, {1, 2, 4}));
    Regions::Scenes(project, objects);
    const auto excluded = Regions::First(project);
    REQUIRE(excluded.HasValue());
    CHECK(Regions::TopArea(excluded.Value().front().geometry) == Catch::Approx(32).margin(1.0e-4));
}

TEST_CASE("Region selection clips in the exact rotated affine frame instead of a world AABB", "[navigation][geometry][region]") {
    Inputs::Project project;
    auto region = Regions::Region(1, Include, {}, {2, 2, 2});
    region.localTransform.translation = {4, 0, 4};
    region.localTransform.rotation = Math::Quaternion::FromAxisAngle({0, 1, 0}, std::numbers::pi_v<float> / 4);
    const std::array objects{Inputs::Project::Object(), region};
    Regions::Scenes(project, objects);
    const auto captured = Regions::First(project);
    REQUIRE(captured.HasValue());
    CHECK(Regions::TopArea(captured.Value().front().geometry) == Catch::Approx(16).margin(1.0e-3));
}

TEST_CASE("Exclusion-only regions subtract geometry and boundary contacts never duplicate the floor", "[navigation][geometry][region]") {
    Inputs::Project project;
    auto region = Regions::Region(1, Exclude, {4, 0, 4}, {1, 2, 4});
    double expected = 48;
    if (GENERATE(false, true)) {
        region.components.navigationRegion->localBounds = {{9, 0, 4}, {1, 2, 4}};
        expected = 64;
    }
    const std::array objects{Inputs::Project::Object(), region};
    Regions::Scenes(project, objects);
    const auto captured = Regions::First(project);
    REQUIRE(captured.HasValue());
    CHECK(Regions::TopArea(captured.Value().front().geometry) == Catch::Approx(expected).margin(1.0e-4));
}

TEST_CASE("Complete exclusion retains empty geometry and its original source footprint", "[navigation][geometry][region][empty]") {
    Inputs::Project project;
    const std::array objects{Inputs::Project::Object(), Regions::Region(1, Exclude, {4, 0, 4}, {4, 2, 4})};
    Regions::Scenes(project, objects);
    const auto selected = Regions::First(project);
    REQUIRE(selected.HasValue());
    REQUIRE(selected.Value().front().footprint);
    CHECK(selected.Value().front().geometry.vertices.empty());
    CHECK(selected.Value().front().geometry.triangles.empty());
    CHECK(selected.Value().front().footprint->minimum.x == 0);
    CHECK(selected.Value().front().footprint->maximum.x == 8);
    const auto projectInput = Capture(project);
    REQUIRE(projectInput.HasValue());
    CHECK(projectInput.Value().contributions.size() == 2);
    CHECK(projectInput.Value().input->Partitions().size() == 2);
    REQUIRE(projectInput.Value().tiles.size() == 2);
    CHECK(projectInput.Value().input->Triangles().empty());
    CHECK(projectInput.Value().tiles.front().bounds.maximum.x == 8);
}

TEST_CASE("Region ordering is canonical and exact authored generations remain in source evidence",
          "[navigation][geometry][region][identity]") {
    Inputs::Project project;
    std::vector objects{Inputs::Project::Object(), Regions::Region(2, Include, {2, 0, 4}, {2, 2, 4}),
                        Regions::Region(1, Include, {4, 0, 4}, {2, 2, 4})};
    Regions::Scenes(project, objects);
    const auto before = Regions::First(project);
    REQUIRE(before.HasValue());
    std::ranges::reverse(objects);
    Regions::Scenes(project, objects);
    const auto reordered = Regions::First(project);
    REQUIRE(reordered.HasValue());
    CHECK(before.Value().front().geometry.digest == reordered.Value().front().geometry.digest);
    CHECK(before.Value().front().contentDigest == reordered.Value().front().contentDigest);
    ++objects.front().components.navigationRegion->generation;
    Regions::Scenes(project, objects);
    const auto changed = Regions::First(project);
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().front().geometry.digest == reordered.Value().front().geometry.digest);
    CHECK(changed.Value().front().contentDigest != reordered.Value().front().contentDigest);
}

TEST_CASE("Both source-selection policies qualify the explicit static-collider pool without synthesizing new sources",
          "[navigation][geometry][region]") {
    Inputs::Project project;
    auto object = Inputs::Project::Object();
    auto region = Regions::Region(1, Include, {2, 0, 4}, {2, 2, 4});
    region.components.navigationRegion->sourceSelection = GENERATE(Runtime::NavigationRegionSourceSelection::ExplicitContributors,
                                                                   Runtime::NavigationRegionSourceSelection::StaticCollisionInBounds);
    std::vector objects{object, region};
    Regions::Scenes(project, objects);
    const auto captured = Regions::First(project);
    REQUIRE(captured.HasValue());
    CHECK(Regions::TopArea(captured.Value().front().geometry) == Catch::Approx(32).margin(1.0e-4));
    objects.front().components.navigationSources.clear();
    Regions::Scenes(project, objects);
    const auto absent = Regions::First(project);
    REQUIRE(absent.HasValue());
    CHECK(absent.Value().empty());
}

TEST_CASE("Disabled regions preserve unfiltered geometry and original source evidence", "[navigation][geometry][region]") {
    Inputs::Project project;
    auto region = Regions::Region(1, Exclude, {4, 0, 4}, {4, 2, 4});
    region.components.navigationRegion->enabled = false;
    const std::array objects{Inputs::Project::Object(), region};
    Regions::Scenes(project, objects);
    const auto captured = Regions::First(project);
    REQUIRE(captured.HasValue());
    CHECK(Regions::TopArea(captured.Value().front().geometry) == Catch::Approx(64).margin(1.0e-4));
    CHECK(captured.Value().front().geometry.digest == captured.Value().front().contentDigest);
}

TEST_CASE("Region metadata fragments and work ceilings reject complete candidates instead of truncating",
          "[navigation][geometry][region][budget]") {
    Inputs::Project project;
    const std::array objects{Inputs::Project::Object(), Regions::Region(1, Include, {2, 0, 4}, {2, 2, 4}),
                             Regions::Region(2, Include, {4, 0, 4}, {2, 2, 4})};
    Regions::Scenes(project, objects);
    NavigationSceneGeometryPolicy policy;
    SECTION("metadata count") {
        policy.maximumRegions = 1;
    }
    SECTION("fragment count") {
        policy.maximumRegionFragments = 1;
    }
    SECTION("work") {
        policy.maximumRegionWorkUnits = 1;
    }
    RequireError(Regions::First(project, policy), Nav::NavigationErrors::CapacityExceeded);
}

TEST_CASE("Region work is shared across project Scenes", "[navigation][geometry][region][budget]") {
    Inputs::Project project;
    auto object = Inputs::Project::Object();
    const auto region = Regions::Region(1, Include, {4, 0, 4}, {4, 2, 4});
    const std::array objects{object, region};
    Regions::Scenes(project, objects);
    NavigationProjectBakeInputLimits limits;
    limits.scene.maximumRegionWorkUnits = 1500;
    RequireError(Capture(project, limits), Nav::NavigationErrors::CapacityExceeded);
    object.components.navigationSources.clear();
    const std::array emptyObjects{object, region};
    project.source.Write("Assets/second.horo", SceneSource::EncodeSceneSource({emptyObjects, {}}));
    CHECK(Capture(project, limits).HasValue());
}
