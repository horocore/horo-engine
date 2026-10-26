#include "support/OpenXRActionBindingTestSupport.h"

namespace Horo::Tests::OpenXRAction {
    TEST_CASE("OpenXR prepared actions attach before candidate issuance and bind the same activated session", "[xr][native][actions]") {
        ActionFixture fixture{false};
        REQUIRE(fixture.Prepare().HasValue());
        CHECK_FALSE(fixture.session.owner.Session().IsValid());
        CHECK(fixture.script.attaches == 1);
        const auto unpublished =
            fixture.session.owner.BorrowPreparedSession({fixture.session.preflight, fixture.session.preflight.Attempt()});
        REQUIRE(unpublished.HasValue());
        CHECK(unpublished.Value().maximumActions == 0);
        CHECK(fixture.owner.Sync(fixture.session.Request().candidate).HasError());
        CHECK(fixture.owner.Bind(fixture.session.Request().candidate).HasError());
        REQUIRE(fixture.session.owner.Activate(fixture.session.Request()).HasValue());
        REQUIRE(fixture.owner.Bind(fixture.session.Request().candidate).HasValue());
        CHECK(fixture.owner.Bind(fixture.session.Request().candidate).HasError());
        CHECK(fixture.script.attaches == 1);
        REQUIRE(fixture.owner.Sync(fixture.session.Request().candidate).HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.Create().HasError());
    }

    TEST_CASE("OpenXR private action creation rejects invalid bounded capacity before creating children", "[xr][native][actions]") {
        ActionFixture fixture{false};
        SECTION("zero") {
            CHECK(fixture.Prepare(0).HasError());
        }
        SECTION("above hard limit") {
            CHECK(fixture.Prepare(XRActionBindingLimits::MaximumActions + 1).HasError());
        }
        CHECK(fixture.script.liveActions == 0);
        CHECK(fixture.script.liveSets == 0);
        CHECK(fixture.script.attaches == 0);
        CHECK_FALSE(fixture.session.owner.Session().IsValid());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR prepared action cleanup preserves attachment debt and rejects reattachment", "[xr][native][actions]") {
        ActionFixture fixture{false};
        REQUIRE(fixture.Prepare().HasValue());
        fixture.session.script.fail = "destroy-action";
        CHECK(fixture.owner.Close().HasError());
        CHECK(fixture.script.liveActions == 1);
        CHECK(fixture.Prepare().HasError());
        fixture.session.script.fail = {};
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.Prepare().HasError());
        CHECK(fixture.script.attaches == 1);
        CHECK(fixture.script.liveActions == 0);
    }

    TEST_CASE("OpenXR prepared actions reject stale attempt before attachment and support exact retry", "[xr][native][actions]") {
        ActionFixture fixture{false};
        fixture.session.script.stale = true;
        CHECK(fixture.Prepare().HasError());
        CHECK(fixture.script.attaches == 0);
        CHECK(fixture.script.liveActions == 0);
        fixture.session.script.stale = false;
        REQUIRE(fixture.Prepare().HasValue());
        auto stale = fixture.session.Request().candidate;
        ++stale.slot.generation;
        REQUIRE(fixture.session.owner.Activate(fixture.session.Request()).HasValue());
        CHECK(fixture.owner.Bind(stale).HasError());
        REQUIRE(fixture.owner.Bind(fixture.session.Request().candidate).HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR prepared profiles are observed before canonical candidate issuance", "[xr][native][actions]") {
        ActionFixture fixture{false};
        const NativeSessionPreparation preparation{fixture.session.preflight, fixture.session.preflight.Attempt()};
        XrInteractionProfileState premature{XR_TYPE_INTERACTION_PROFILE_STATE};
        CHECK(ActionScript::Profile(XR_NULL_HANDLE, XR_NULL_PATH, &premature) == XR_ERROR_ACTIONSET_NOT_ATTACHED);
        CHECK(fixture.owner.PreparedProfiles(preparation).HasError());
        CHECK(std::ranges::count(fixture.session.script.calls, std::string_view{"profile"}) == 0);
        REQUIRE(fixture.Prepare().HasValue());
        CHECK(fixture.script.attaches == 1);
        const auto actual =
            std::ranges::find(fixture.script.paths, std::string{fixture.profilePaths[1].path}, &std::pair<std::string, XrPath>::first);
        REQUIRE(actual != fixture.script.paths.end());
        fixture.script.currentProfiles[0] = actual->second;
        const auto profiles = fixture.owner.PreparedProfiles(preparation);
        REQUIRE(profiles.HasValue());
        CHECK(profiles.Value()[0].profile == XRInteractionProfileId{2});
        CHECK(profiles.Value()[1].profile == XRInteractionProfileId{});
        CHECK_FALSE(fixture.session.owner.Session().IsValid());
        CHECK(fixture.script.syncs == 0);
        REQUIRE(fixture.session.owner.Activate(fixture.session.Request()).HasValue());
        REQUIRE(fixture.owner.Bind(fixture.session.Request().candidate).HasValue());
        CHECK(fixture.owner.PreparedProfiles(preparation).HasError());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR prepared profile observation preserves native and stale-attempt failures", "[xr][native][actions]") {
        ActionFixture fixture{false};
        REQUIRE(fixture.Prepare().HasValue());
        const NativeSessionPreparation preparation{fixture.session.preflight, fixture.session.preflight.Attempt()};
        fixture.session.script.fail = "profile";
        CHECK(fixture.owner.PreparedProfiles(preparation).HasError());
        fixture.session.script.fail = {};
        fixture.session.script.stale = true;
        CHECK(fixture.owner.PreparedProfiles(preparation).HasError());
        fixture.session.script.stale = false;
        REQUIRE(fixture.owner.PreparedProfiles(preparation).HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR action retirement revokes prepared profile and binding admission before failed destroy", "[xr][native][actions]") {
        ActionFixture fixture{false};
        REQUIRE(fixture.Prepare().HasValue());
        const NativeSessionPreparation preparation{fixture.session.preflight, fixture.session.preflight.Attempt()};
        fixture.session.script.fail = "destroy-action";
        REQUIRE(fixture.owner.Close().HasError());
        CHECK(fixture.owner.PreparedProfiles(preparation).HasError());
        REQUIRE(fixture.session.owner.Activate(fixture.session.Request()).HasValue());
        CHECK(fixture.owner.Bind(fixture.session.Request().candidate).HasError());
        fixture.session.script.fail = {};
        REQUIRE(fixture.owner.Close().HasValue());
    }
}  // namespace Horo::Tests::OpenXRAction
