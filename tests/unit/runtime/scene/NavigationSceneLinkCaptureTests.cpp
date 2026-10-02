#include "Horo/Navigation/NavigationSceneLinkCapture.h"
#include "navigation/NavigationTestAssertions.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Navigation {
    namespace {
        using TestSupport::Id;
        using TestSupport::RequireError;

        Runtime::NavigationLinkComponent AuthoredLink() {
            return {.id = Id<NavigationLinkId>(1),
                    .start = {.surface = Id<SurfaceId>(10), .localPosition = {0.0F, 0.0F, 0.0F}},
                    .end = {.surface = Id<SurfaceId>(20), .localPosition = {1.0F, 0.0F, 0.0F}},
                    .kind = Runtime::NavigationLinkKind::Door,
                    .direction = Runtime::NavigationLinkDirection::Bidirectional,
                    .profiles = {Id<NavigationAgentProfileId>(1)},
                    .traversalCost = 2.0F};
        }
    }  // namespace

    TEST_CASE("Committed Scene link capture transforms positions and preserves kind direction metre radii and cost",
              "[unit][navigation][scene][links]") {
        auto link = AuthoredLink();
        const std::array capture{
            NavigationSceneLinkCaptureInput{.link = &link,
                                            .localToCanonicalMeters = {.translation = {3.0F, 0.0F, 0.0F}, .scale = {2.0F, 2.0F, 2.0F}}}};
        const auto result = CaptureNavigationSceneLinks(capture, Id<NavigationAgentProfileId>(1));
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().size() == 1);
        REQUIRE(result.Value().front().start.position == Math::Vec3{3.0F, 0.0F, 0.0F});
        REQUIRE(result.Value().front().end.position == Math::Vec3{5.0F, 0.0F, 0.0F});
        REQUIRE(result.Value().front().start.connectionRadiusMeters == 0.5F);
        REQUIRE(result.Value().front().kind == NavigationLinkKind::Door);
        REQUIRE(result.Value().front().direction == NavigationLinkDirection::Bidirectional);
        REQUIRE(result.Value().front().traversalCost == 2.0F);
        link.end.localPosition.x = 99.0F;
        REQUIRE(result.Value().front().end.position.x == 5.0F);
    }

    TEST_CASE("Disabled and unselected-profile links stay authored and are omitted from bake capture", "[unit][navigation][scene][links]") {
        auto link = AuthoredLink();
        SECTION("Disabled") {
            link.enabled = false;
        }
        SECTION("Another profile") {
            link.profiles = {Id<NavigationAgentProfileId>(2)};
        }
        const auto result =
            CaptureNavigationSceneLinks(std::array{NavigationSceneLinkCaptureInput{.link = &link}}, Id<NavigationAgentProfileId>(1));
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().empty());
    }

    TEST_CASE("Scene bake-link capture rejects malformed components transforms identities and duplicate links transactionally",
              "[unit][navigation][scene][links][hostile]") {
        auto link = AuthoredLink();
        NavigationSceneLinkCaptureInput capture{.link = &link};
        const ErrorCodeDescriptor *error = &NavigationErrors::BakeInputInvalid;
        auto profile = Id<NavigationAgentProfileId>(1);
        SECTION("Null component") {
            capture.link = nullptr;
        }
        SECTION("Invalid profile") {
            profile = {};
        }
        SECTION("Zero scale") {
            capture.localToCanonicalMeters.scale.x = 0.0F;
        }
        SECTION("Negative scale") {
            capture.localToCanonicalMeters.scale.x = -1.0F;
        }
        SECTION("Nonfinite transform") {
            capture.localToCanonicalMeters.translation.x = std::numeric_limits<float>::infinity();
        }
        SECTION("Malformed authored payload") {
            link.start.connectionRadiusMeters = 0.0F;
            error = &NavigationErrors::SceneComponentInvalid;
        }
        RequireError(CaptureNavigationSceneLinks(std::array{capture}, profile), *error);
        link = AuthoredLink();
        RequireError(CaptureNavigationSceneLinks(std::array{NavigationSceneLinkCaptureInput{.link = &link},
                                                            NavigationSceneLinkCaptureInput{.link = &link}},
                                                 Id<NavigationAgentProfileId>(1)),
                     NavigationErrors::DescriptorConflict);
    }
}  // namespace Horo::Navigation
