#include "helpers/input/BindingCaptureFixture.h"
#include "support/AllocationProbe.h"
#include "support/OwnedTestDirectory.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <new>
#include <utility>
#include <vector>

namespace {
    using namespace Horo::Input;
    using CaptureFixture = Horo::Input::Tests::CaptureFixture;

    TEST_CASE("Captured unknown-controller triggers return to neutral after the remap closes", "[input][rebinding]") {
        const float rest = GENERATE(-1.0F, 1.0F);
        const auto shape = GENERATE(ActionValueType::Digital, ActionValueType::Axis1D, ActionValueType::Axis2D);
        const auto range = rest < 0 ? RawAxisCaptureRange::RestingNegativeOne : RawAxisCaptureRange::RestingPositiveOne;
        CaptureFixture fixture{InputContextKind::ModalRoot, rest, shape, range};
        REQUIRE(fixture.Begin().SetRawGamepadAxis(fixture.raw, 0, -rest));
        const auto result = fixture.Poll();
        REQUIRE(result.status == BindingCaptureStatus::Captured);
        REQUIRE(result.binding);
        const Horo::Tests::OwnedTestDirectory directory{"raw-trigger-remap"};
        const BindingProfilePaths paths{{}, directory.Path() / "editor.json", directory.Path() / "project.json"};
        REQUIRE(CommitBindingProfileChange(fixture.input.Router(), paths, BindingProfileScope::ProjectUser,
                                           {ActionId{"jump"}, {*result.binding}})
                    .HasValue());
        const auto saved = LoadBindingProfile(paths.projectUser);
        REQUIRE(saved.HasValue());
        REQUIRE(saved.Value().overrides.front().bindings.front() == *result.binding);
        fixture.owner.Reset();
        REQUIRE(fixture.Begin().SetRawGamepadAxis(fixture.raw, 0, rest));
        (void)fixture.input.CommitFrame();
        fixture.Begin();
        (void)fixture.input.CommitFrame();
        const auto neutral = fixture.input.Router().ReadAction(fixture.gameplay, ActionId{"jump"});
        CHECK_FALSE(neutral.down);
        CHECK(neutral.x == 0.0F);
        CHECK(neutral.y == 0.0F);

        REQUIRE(fixture.Begin().SetRawGamepadAxis(fixture.raw, 0, -rest));
        (void)fixture.input.CommitFrame();
        const auto pressed = fixture.input.Router().ReadAction(fixture.gameplay, ActionId{"jump"});
        CHECK(pressed.down);
        CHECK(pressed.pressed);
        CHECK(pressed.x == 1.0F);
        REQUIRE(fixture.Begin().SetRawGamepadAxis(fixture.raw, 0, rest));
        (void)fixture.input.CommitFrame();
        const auto released = fixture.input.Router().ReadAction(fixture.gameplay, ActionId{"jump"});
        CHECK_FALSE(released.down);
        CHECK(released.released);
        CHECK(released.x == 0.0F);
    }

    TEST_CASE("Ambiguous raw axis starting samples require explicit calibration without changing profiles", "[input][rebinding]") {
        const auto range = GENERATE(RawAxisCaptureRange::Centered, RawAxisCaptureRange::RestingPositiveOne);
        CaptureFixture fixture{InputContextKind::ModalRoot, -1.0F, ActionValueType::Digital, range};
        const auto profile = fixture.input.Router().Profile();
        REQUIRE(fixture.Begin().SetRawGamepadAxis(fixture.raw, 0, 1.0F));
        (void)fixture.input.CommitFrame();
        const auto before = Horo::Tests::AllocationProbe::Count();
        const auto result = fixture.capture->Poll(fixture.input.Router(), fixture.owner);
        const auto after = Horo::Tests::AllocationProbe::Count();
        CHECK(before == after);
        CHECK(result.status == BindingCaptureStatus::NeedsAxisCalibration);
        CHECK_FALSE(result.binding);
        CHECK(fixture.input.Router().Profile() == profile);
        CHECK(fixture.capture->Poll(fixture.input.Router(), fixture.owner).status == BindingCaptureStatus::Cancelled);
    }

    TEST_CASE("Raw axis capture rejects undeclared ranges without publishing a capture", "[input][rebinding]") {
        CaptureFixture fixture;
        const auto rejected =
            InputBindingCapture::Create(fixture.input.Router(), fixture.owner, ActionId{"jump"}, static_cast<RawAxisCaptureRange>(255));
        CHECK(rejected.HasError());
    }

    TEST_CASE("Raw stick capture can retry from an explicitly centered baseline", "[input][rebinding]") {
        CaptureFixture fixture;
        REQUIRE(fixture.Begin().SetRawGamepadAxis(fixture.raw, 0, 0.0F));
        REQUIRE(fixture.Poll().status == BindingCaptureStatus::NeedsAxisCalibration);
        fixture.Begin();
        (void)fixture.input.CommitFrame();
        fixture.Begin();
        (void)fixture.input.CommitFrame();
        auto retry = InputBindingCapture::Create(fixture.input.Router(), fixture.owner, ActionId{"jump"});
        REQUIRE(retry.HasValue());
        fixture.capture.emplace(std::move(retry).Value());
        REQUIRE(fixture.Begin().SetRawGamepadAxis(fixture.raw, 0, -1.0F));
        const auto result = fixture.Poll();
        REQUIRE(result.status == BindingCaptureStatus::Captured);
        REQUIRE(result.binding);
        CHECK(result.binding->deadzoneKind == DeadzoneKind::Axial);
    }

    TEST_CASE("Raw axis capture deadzones cover the admitted near-neutral starting sample", "[input][rebinding]") {
        const float rest = GENERATE(-0.2F, 0.2F);
        CaptureFixture fixture{InputContextKind::ModalRoot, rest, ActionValueType::Axis1D};
        REQUIRE(fixture.Begin().SetRawGamepadAxis(fixture.raw, 0, 1.0F));
        const auto result = fixture.Poll();
        REQUIRE(result.status == BindingCaptureStatus::Captured);
        REQUIRE(result.binding);
        REQUIRE(fixture.input.Router().SetProfile({.overrides = {{ActionId{"jump"}, {*result.binding}}}}).HasValue());
        fixture.owner.Reset();
        REQUIRE(fixture.Begin().SetRawGamepadAxis(fixture.raw, 0, rest));
        (void)fixture.input.CommitFrame();
        fixture.Begin();
        (void)fixture.input.CommitFrame();
        CHECK(fixture.input.Router().ReadAction(fixture.gameplay, ActionId{"jump"}).x == 0.0F);
    }

    TEST_CASE("Raw trigger policies cannot normalize canonical controls or unknown enum values", "[input][rebinding]") {
        InputRouter router;
        const InputBinding valid{.kind = BindingControlKind::RawGamepadAxis, .deadzoneKind = DeadzoneKind::UnipolarFromNegativeOne};
        const ActionDescriptor action{ActionId{"axis"}, ActionValueType::Axis1D, InputContextId{"gameplay"}, true, {valid}};
        REQUIRE(router.SetActionMap({action}).HasValue());
        const std::vector<ActionDescriptor> before(router.Actions().begin(), router.Actions().end());
        auto invalid = action;
        SECTION("Canonical axes already have their canonical range") {
            invalid.defaultBindings.front().kind = BindingControlKind::GamepadAxis;
        }
        SECTION("Unsupported policy") {
            invalid.defaultBindings.front().deadzoneKind = static_cast<DeadzoneKind>(255);
        }
        CHECK(router.SetActionMap({invalid}).HasError());
        CHECK(std::ranges::equal(router.Actions(), before));
    }

    TEST_CASE("Capture preparation allocation failure preserves the caller's context and profile", "[input][rebinding][allocation]") {
        CaptureFixture fixture;
        const auto profile = fixture.input.Router().Profile();
        const auto routing = fixture.input.Router().RoutingState(fixture.owner);
        const ActionId action{"jump"};
        bool failed{};
        {
            Horo::Tests::AllocationProbe::ScopedFailure failure;
            try {
                [[maybe_unused]] auto capture = InputBindingCapture::Create(fixture.input.Router(), fixture.owner, action);
            } catch (const std::bad_alloc &) {
                failed = true;
            }
        }
        REQUIRE(failed);
        CHECK(fixture.input.Router().IsContextActive(fixture.owner));
        CHECK(fixture.input.Router().RoutingState(fixture.owner).contextIdentity == routing.contextIdentity);
        CHECK(fixture.input.Router().Profile() == profile);
    }

}  // namespace
