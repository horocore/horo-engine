#include "OpenXRActionBindings.h"
#include "support/OpenXRActionBindingTestSupport.h"

#include <catch2/matchers/catch_matchers_exception.hpp>
#include <format>
#include <new>
#include <stdexcept>

namespace {
    using namespace Horo;
    using namespace Horo::XR;
    using namespace Horo::XR::OpenXRInternal;
    using namespace Horo::Tests::OpenXR;

    using namespace Horo::Tests::OpenXRAction;

    struct Neutralizer final : IXRBindingNeutralizer {
        unsigned calls{};

        void Neutralize(const XRSessionId &, std::uint64_t) noexcept override {
            ++calls;
        }
    };

    TEST_CASE("OpenXR action ownership creates suggests attaches synchronizes and retires deterministically", "[xr][openxr][actions]") {
        ActionFixture fixture;
        REQUIRE(fixture.Create().HasValue());
        REQUIRE(fixture.script.liveSets == 1);
        REQUIRE(fixture.script.liveActions == 1);
        REQUIRE(fixture.script.suggestions == 2);
        REQUIRE(fixture.script.attaches == 1);
        REQUIRE(fixture.owner.Sync(fixture.session.Request().candidate).HasValue());
        REQUIRE(fixture.script.syncs == 1);
        REQUIRE(fixture.owner.Close().HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
        REQUIRE(fixture.script.liveSets == 0);
        REQUIRE(fixture.script.liveActions == 0);
        REQUIRE(fixture.owner.Sync(fixture.session.Request().candidate).HasError());
        REQUIRE(fixture.Create().HasError());  // OpenXR permits only one attachment per native session.
    }

    TEST_CASE("OpenXR active profile changes join the Input neutral boundary without exposing native paths", "[xr][openxr][actions]") {
        ActionFixture fixture;
        REQUIRE(fixture.Create().HasValue());
        Neutralizer neutralizer;
        XRActionBindingCoordinator coordinator{fixture.session.Request().candidate, neutralizer};
        fixture.script.currentProfiles[0] = fixture.script.Find("/interaction_profiles/oculus/touch_controller");
        auto profiles = fixture.owner.Profiles(fixture.session.Request().candidate);
        REQUIRE(profiles.HasValue());
        REQUIRE(profiles.Value()[0].profile == XRInteractionProfileId{2});
        REQUIRE(coordinator
                    .Update(fixture.session.Request().candidate, {1, fixture.actions, fixture.suggestions, {}, {1}, fixture.registered},
                            fixture.controls, profiles.Value())
                    .HasValue());
        fixture.script.currentProfiles[0] = 9999;
        profiles = fixture.owner.Profiles(fixture.session.Request().candidate);
        REQUIRE(profiles.Value()[0].profile == XRInteractionProfileId{});
        REQUIRE(coordinator
                    .Update(fixture.session.Request().candidate, {1, fixture.actions, fixture.suggestions, {}, {1}, fixture.registered},
                            fixture.controls, profiles.Value())
                    .HasValue());
        REQUIRE(neutralizer.calls == 1);
        REQUIRE(coordinator.Current()->bindings[0].fallback);
        coordinator.Shutdown();
        REQUIRE(neutralizer.calls == 2);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR native action preparation rolls back every failed phase before publication", "[xr][openxr][actions]") {
        ActionFixture fixture;
        SECTION("set creation") {
            fixture.session.script.fail = "create-set";
        }
        SECTION("action creation") {
            fixture.session.script.fail = "create-action";
        }
        SECTION("suggestion") {
            fixture.session.script.fail = "suggest";
        }
        SECTION("attachment") {
            fixture.session.script.fail = "attach";
        }
        REQUIRE(fixture.Create().HasError());
        REQUIRE(fixture.script.liveSets == 0);
        REQUIRE(fixture.script.liveActions == 0);
        REQUIRE(fixture.owner.Profiles(fixture.session.Request().candidate).HasError());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR rejects invalid paths missing dispatch stale generations and unfocused synchronization", "[xr][openxr][actions]") {
        ActionFixture fixture;
        SECTION("wrong-hand component path") {
            fixture.controlPaths[0].path = "/user/hand/right/input/select/click";
            REQUIRE(fixture.Create().HasError());
            REQUIRE(fixture.script.liveSets == 0);
        }
        SECTION("missing destroy dispatch") {
            fixture.session.script.absent = "xrDestroyAction";
            REQUIRE(fixture.Create().HasError());
            REQUIRE(fixture.script.liveSets == 0);
        }
        SECTION("stale generation") {
            REQUIRE(fixture.Create().HasValue());
            auto stale = fixture.session.Request().candidate;
            ++stale.slot.generation;
            REQUIRE(fixture.owner.Sync(stale).HasError());
            REQUIRE(fixture.script.syncs == 0);
        }
        SECTION("focus loss is unavailable, never success") {
            REQUIRE(fixture.Create().HasValue());
            fixture.session.script.fail = "sync";
            fixture.session.script.failureResult = XR_SESSION_NOT_FOCUSED;
            REQUIRE(fixture.owner.Sync(fixture.session.Request().candidate).HasError());
        }
    }

    TEST_CASE("OpenXR action callback exceptions roll back acquired ownership before returning or propagating", "[xr][native][lifecycle]") {
        ActionFixture fixture;
        SECTION("supported format errors become typed preparation failure") {
            fixture.script.throwFormat = true;
            const auto result = fixture.Create();
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == "xr.operation.unavailable");
        }
        SECTION("supported allocation errors become typed preparation failure") {
            fixture.script.throwAllocation = true;
            const auto result = fixture.Create();
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == "xr.operation.unavailable");
        }
        SECTION("unrelated runtime exceptions preserve type and message") {
            fixture.script.throwRuntime = true;
            REQUIRE_THROWS_MATCHES(fixture.Create(), std::runtime_error,
                                   Catch::Matchers::Message("unsupported scripted runtime exception"));
        }
        SECTION("unsupported standard exceptions preserve type and message") {
            fixture.script.throwUnsupportedStandard = true;
            REQUIRE_THROWS_MATCHES(fixture.Create(), std::logic_error,
                                   Catch::Matchers::Message("unsupported scripted preparation exception"));
        }
        SECTION("foreign callback exceptions preserve their original identity") {
            fixture.script.throwForeign = true;
            REQUIRE_THROWS_AS(fixture.Create(), std::uint32_t);
        }
        REQUIRE(fixture.script.liveActions == 0);
        REQUIRE(fixture.script.liveSets == 0);
        REQUIRE(fixture.script.attaches == 0);
        const auto sync = fixture.owner.Sync(fixture.session.Request().candidate);
        REQUIRE(sync.HasError());
        REQUIRE(sync.ErrorValue().code.Value() == "xr.operation.unavailable");
    }

    TEST_CASE("OpenXR failed action retirement closes admission and retains handles for explicit retry", "[xr][openxr][actions]") {
        ActionFixture fixture;
        REQUIRE(fixture.Create().HasValue());
        fixture.session.script.fail = "destroy-action";
        REQUIRE(fixture.owner.Close().HasError());
        REQUIRE(fixture.script.liveActions == 1);
        REQUIRE(fixture.owner.Sync(fixture.session.Request().candidate).HasError());
        fixture.session.script.fail = {};
        REQUIRE(fixture.owner.Close().HasValue());
        REQUIRE(fixture.script.liveActions == 0);
        REQUIRE(fixture.script.liveSets == 0);
    }

    TEST_CASE("OpenXR foreign preparation exceptions preserve retryable retirement failure", "[xr][native][lifecycle]") {
        ActionFixture fixture;
        fixture.script.throwForeign = true;
        fixture.session.script.fail = "destroy-action";
        REQUIRE_THROWS_AS(fixture.Create(), std::uint32_t);
        REQUIRE(fixture.script.liveActions == 1);
        REQUIRE(fixture.script.liveSets == 1);
        REQUIRE(fixture.script.attaches == 0);
        REQUIRE(fixture.owner.Sync(fixture.session.Request().candidate).HasError());
        fixture.session.script.fail = {};
        REQUIRE(fixture.owner.Close().HasValue());
        REQUIRE(fixture.script.liveActions == 0);
        REQUIRE(fixture.script.liveSets == 0);
    }
}  // namespace
