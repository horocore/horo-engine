#include "Horo/XR/XRInputProjection.h"
#include "OpenXRApplicationBindings.h"
#include "support/OpenXRActionBindingTestSupport.h"

namespace Horo::Tests::OpenXRAction {
    namespace {
        /** @brief Supplies a valid physical binding for the host-owned canonical Input registry. */
        std::vector<Input::ActionDescriptor> CanonicalActions(const ActionFixture &fixture) {
            std::vector<Input::ActionDescriptor> result{fixture.registered.begin(), fixture.registered.end()};
            result.front().defaultBindings = {{.kind = Input::BindingControlKind::Key, .key = Input::Key::Space}};
            return result;
        }

        /** @brief Copies the explicit product fixture, excluding its separately owned canonical registry. */
        Application::Internal::OpenXRApplicationBindings ProductBindings(const ActionFixture &fixture) {
            Application::Internal::OpenXRApplicationBindings result;
            result.schema.version = 1;
            result.schema.actions.assign(fixture.actions.begin(), fixture.actions.end());
            result.schema.suggestions.assign(fixture.suggestions.begin(), fixture.suggestions.end());
            result.schema.genericFallback = {1};
            for (const auto &profile : fixture.profilePaths)
                result.profiles.push_back({profile.profile, std::string{profile.path}});
            for (std::size_t index = 0; index < fixture.controls.size(); ++index)
                result.controls.push_back({fixture.controls[index], std::string{fixture.controlPaths[index].path}});
            result.actionLabels.push_back({Input::ActionId{"select"}, "Select"});
            result.setLabels.push_back({{1}, "Gameplay"});
            return result;
        }
    }  // namespace

    TEST_CASE("Application XR bindings join the actual Input registry after native attachment without issuing a session",
              "[xr][host][input]") {
        ActionFixture fixture{false};
        Input::InputRouter router;
        REQUIRE(router.SetActionMap(CanonicalActions(fixture)).HasValue());
        auto context = router.PushContext(Input::InputContextId{"gameplay"}, Input::InputContextKind::Gameplay);
        auto port = router.ActionCollection(context);
        REQUIRE(port.HasValue());
        auto product = ProductBindings(fixture);
        const auto origin = IdentityValue<XRWorldOriginRevision>(1);
        auto plan = product.Prepare(fixture.owner, {fixture.session.preflight, fixture.session.preflight.Attempt()},
                                    {&ActionScript::GetProc}, router.Actions(), {origin, XRSpaceKind::Local});
        REQUIRE(plan.HasValue());
        REQUIRE(plan.Value().bindings.size() == 1);
        CHECK(plan.Value().bindings.front().fallback);
        CHECK(fixture.script.attaches == 1);
        CHECK(fixture.script.syncs == 0);
        CHECK_FALSE(fixture.session.owner.Session().IsValid());
        auto reserved = XRInputProjection::Prepare(port.Value(), plan.Value());
        REQUIRE(reserved.HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("Application XR bindings cannot supply their own semantic registry or exceed bounded native catalogs", "[xr][host][input]") {
        ActionFixture fixture{false};
        Input::InputRouter router;
        auto product = ProductBindings(fixture);
        SECTION("missing registered semantic action") {}
        SECTION("wrong canonical value shape") {
            auto canonical = CanonicalActions(fixture).front();
            canonical.valueType = Input::ActionValueType::Axis1D;
            canonical.defaultBindings = {{.kind = Input::BindingControlKind::GamepadAxis, .gamepadAxis = Input::GamepadAxis::LeftX}};
            REQUIRE(router.SetActionMap({canonical}).HasValue());
        }
        SECTION("oversized native catalog") {
            REQUIRE(router.SetActionMap(CanonicalActions(fixture)).HasValue());
            product.controls.resize(XRActionBindingLimits::MaximumBindings + 1);
        }
        const auto calls = fixture.session.script.calls.size();
        const auto result =
            product.Prepare(fixture.owner, {fixture.session.preflight, fixture.session.preflight.Attempt()}, {&ActionScript::GetProc},
                            router.Actions(), {IdentityValue<XRWorldOriginRevision>(1), XRSpaceKind::Local});
        CHECK(result.HasError());
        CHECK(fixture.session.script.calls.size() == calls);
        CHECK(fixture.script.attaches == 0);
    }

    TEST_CASE("Application XR profile query failures preserve native ownership without publishing Input", "[xr][host][input]") {
        ActionFixture fixture{false};
        Input::InputRouter router;
        REQUIRE(router.SetActionMap(CanonicalActions(fixture)).HasValue());
        const auto product = ProductBindings(fixture);
        fixture.session.script.fail = "profile";
        const auto result =
            product.Prepare(fixture.owner, {fixture.session.preflight, fixture.session.preflight.Attempt()}, {&ActionScript::GetProc},
                            router.Actions(), {IdentityValue<XRWorldOriginRevision>(1), XRSpaceKind::Local});
        CHECK(result.HasError());
        CHECK(fixture.script.attaches == 1);
        CHECK(fixture.script.liveActions == 1);
        CHECK_FALSE(fixture.session.owner.Session().IsValid());
        fixture.session.script.fail = {};
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.script.tracking.spaces == 0);
        CHECK(fixture.script.liveActions == 0);
    }
}  // namespace Horo::Tests::OpenXRAction

namespace Horo::Tests::OpenXRAction {
    TEST_CASE("Application XR bindings reject a missing official dispatch before native acquisition", "[xr][host][input]") {
        ActionFixture fixture{false};
        Input::InputRouter router;
        REQUIRE(router.SetActionMap(CanonicalActions(fixture)).HasValue());
        const auto product = ProductBindings(fixture);
        const auto calls = fixture.session.script.calls.size();
        const auto result = product.Prepare(fixture.owner, {fixture.session.preflight, fixture.session.preflight.Attempt()}, {},
                                            router.Actions(), {IdentityValue<XRWorldOriginRevision>(1), XRSpaceKind::Local});
        CHECK(result.HasError());
        CHECK(fixture.session.script.calls.size() == calls);
        CHECK(fixture.script.attaches == 0);
    }
}  // namespace Horo::Tests::OpenXRAction
