#include "support/OpenXRActionBindingTestSupport.h"

#include <thread>

namespace Horo::Tests::OpenXR {
    TEST_CASE("OpenXR session retirement retains the sole instance and distinguishes reused native handles", "[xr][native][retirement]") {
        Fixture fixture;
        REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
        const auto first = fixture.owner.Borrow(fixture.Request().candidate);
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().incarnation > 0);
        REQUIRE(fixture.owner.RetireSession().HasValue());
        CHECK_FALSE(fixture.owner.Session().IsValid());
        CHECK(fixture.script.liveSessions == 0);
        CHECK(fixture.script.liveInstances == 1);
        CHECK(fixture.owner.Borrow(fixture.Request().candidate).HasError());
        REQUIRE(fixture.owner.BorrowDiscovery(fixture.preflight.Attempt()).HasValue());
        REQUIRE(fixture.owner.PrepareSession({fixture.preflight, fixture.preflight.Attempt()}).HasValue());
        const auto prepared = fixture.owner.BorrowPreparedSession({fixture.preflight, fixture.preflight.Attempt()});
        REQUIRE(prepared.HasValue());
        CHECK(prepared.Value().session == first.Value().session);  // Script deliberately recycles the raw native handle.
        CHECK(prepared.Value().instance == first.Value().instance);
        CHECK(prepared.Value().incarnation == first.Value().incarnation + 1);
        CHECK(prepared.Value().maximumActions == 0);
        CHECK(prepared.Value().maximumSpaces == 0);
        CHECK(fixture.owner.Activate(fixture.Request()).HasError());  // Canonical generations never rewind on native reuse.
        REQUIRE(fixture.owner.Activate(fixture.Request(2)).HasValue());
        CHECK(fixture.owner.Session() == fixture.Request(2).candidate);
        CHECK(std::ranges::count(fixture.script.calls, "instance") == 1);
        CHECK(std::ranges::count(fixture.script.calls, "session") == 2);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR failed native creation consumes no child receipt before a later successful restart", "[xr][native][retirement]") {
        Fixture fixture;
        REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
        const auto first = fixture.owner.Borrow(fixture.Request().candidate);
        REQUIRE(first.HasValue());
        REQUIRE(fixture.owner.RetireSession().HasValue());
        fixture.script.fail = "session";
        CHECK(fixture.owner.PrepareSession({fixture.preflight, fixture.preflight.Attempt()}).HasError());
        CHECK(fixture.script.liveSessions == 0);
        CHECK(fixture.script.liveInstances == 0);
        fixture.script.fail = {};
        REQUIRE(fixture.owner.Create(fixture.Request(2)).HasValue());
        const auto replacement = fixture.owner.Borrow(fixture.Request(2).candidate);
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value().incarnation == first.Value().incarnation + 1);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR failed session-only retirement retains graphics and rejects native reopening until retry",
              "[xr][native][retirement]") {
        Fixture fixture;
        REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
        fixture.script.failRetirement = true;
        CHECK(fixture.owner.RetireSession().HasError());
        CHECK_FALSE(fixture.owner.Session().IsValid());
        CHECK(fixture.script.liveSessions == 1);
        CHECK(fixture.script.liveInstances == 1);
        CHECK(std::ranges::count(fixture.script.calls, "release-graphics") == 0);
        CHECK(fixture.owner.BorrowPreparedSession({fixture.preflight, fixture.preflight.Attempt()}).HasError());
        CHECK(fixture.owner.PrepareSession({fixture.preflight, fixture.preflight.Attempt()}).HasError());
        fixture.script.failRetirement = false;
        REQUIRE(fixture.owner.RetireSession().HasValue());
        CHECK(fixture.script.liveSessions == 0);
        CHECK(fixture.script.liveInstances == 1);
        CHECK(std::ranges::count(fixture.script.calls, "release-graphics") == 1);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR session retirement rejects reentrant and foreign-lane destruction without losing native parents",
              "[xr][native][retirement]") {
        Fixture fixture;
        REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
        bool rejected{};
        std::thread foreign{[&] {
            rejected = fixture.owner.RetireSession().HasError();
        }};
        foreign.join();
        CHECK(rejected);
        CHECK(fixture.script.liveSessions == 1);
        fixture.script.retirement.owner = &fixture.owner;
        REQUIRE(fixture.owner.RetireSession().HasValue());
        CHECK(fixture.script.retirement.rejectedSession);
        CHECK(fixture.script.retirement.rejectedFull);
        CHECK(std::ranges::count(fixture.script.calls, "destroy-session") == 1);
        CHECK(fixture.script.liveInstances == 1);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR full-close debt cannot be converted into retained discovery by session retirement", "[xr][native][retirement]") {
        Fixture fixture;
        REQUIRE(fixture.owner.Create(fixture.Request()).HasValue());
        fixture.script.failInstanceRetirement = true;
        CHECK(fixture.owner.Close().HasError());
        CHECK(fixture.script.liveInstances == 1);
        CHECK(fixture.owner.RetireSession().HasError());
        CHECK(fixture.owner.BorrowDiscovery(fixture.preflight.Attempt()).HasError());
        fixture.script.failInstanceRetirement = false;
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.script.liveInstances == 0);
    }
}  // namespace Horo::Tests::OpenXR

namespace Horo::Tests::OpenXRAction {
    TEST_CASE("OpenXR action attachment receipt permits a new physical session despite identical raw handle and attempt",
              "[xr][native][actions]") {
        ActionFixture fixture{false};
        REQUIRE(fixture.Prepare().HasValue());
        REQUIRE(fixture.session.owner.Activate(fixture.session.Request()).HasValue());
        REQUIRE(fixture.owner.Bind(fixture.session.Request().candidate).HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
        REQUIRE(fixture.session.owner.RetireSession().HasValue());
        REQUIRE(fixture.session.owner.PrepareSession({fixture.session.preflight, fixture.session.preflight.Attempt()}).HasValue());
        REQUIRE(fixture.Prepare().HasValue());
        REQUIRE(fixture.session.owner.Activate(fixture.session.Request(2)).HasValue());
        REQUIRE(fixture.owner.Bind(fixture.session.Request(2).candidate).HasValue());
        REQUIRE(fixture.owner.Sync(fixture.session.Request(2).candidate).HasValue());
        CHECK(fixture.script.attaches == 2);
        CHECK(fixture.script.liveActions == 1);
        REQUIRE(fixture.owner.Close().HasValue());
    }
}  // namespace Horo::Tests::OpenXRAction
