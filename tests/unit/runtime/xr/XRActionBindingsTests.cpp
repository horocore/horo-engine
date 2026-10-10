#include "Horo/XR/XRActionBindings.h"
#include "support/TypedIdentityTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::XR;
    using namespace Horo::Input;

    struct Fixture final {
        std::array<ActionDescriptor, 2>
            registered{ActionDescriptor{ActionId{"select"}, ActionValueType::Digital, InputContextId{"gameplay"}, true, {}},
                       ActionDescriptor{ActionId{"move"}, ActionValueType::Axis2D, InputContextId{"gameplay"}, false, {}}};
        std::array<XRActionDeclaration, 2> actions{XRActionDeclaration{ActionId{"select"},
                                                                       InputContextId{"gameplay"},
                                                                       ActionValueType::Digital,
                                                                       {1},
                                                                       XRTrackedDeviceRole::LeftController,
                                                                       true},
                                                   XRActionDeclaration{ActionId{"move"},
                                                                       InputContextId{"gameplay"},
                                                                       ActionValueType::Axis2D,
                                                                       {2},
                                                                       XRTrackedDeviceRole::RightController,
                                                                       false}};
        std::array<XRProfileControl, 4> controls{XRProfileControl{{1}, {1}, XRTrackedDeviceRole::LeftController, ActionValueType::Digital},
                                                 XRProfileControl{{2}, {1}, XRTrackedDeviceRole::LeftController, ActionValueType::Digital},
                                                 XRProfileControl{{2}, {2}, XRTrackedDeviceRole::RightController, ActionValueType::Axis2D},
                                                 XRProfileControl{{2}, {3}, XRTrackedDeviceRole::LeftController, ActionValueType::Digital}};
        std::array<XRSuggestedActionBinding, 3> suggestions{XRSuggestedActionBinding{ActionId{"select"}, {1}, {1}},
                                                            XRSuggestedActionBinding{ActionId{"select"}, {2}, {1}},
                                                            XRSuggestedActionBinding{ActionId{"move"}, {2}, {2}}};
        std::array<XRActiveInteractionProfile, 2> profiles{XRActiveInteractionProfile{XRTrackedDeviceRole::LeftController, {2}},
                                                           XRActiveInteractionProfile{XRTrackedDeviceRole::RightController, {2}}};

        XRActionBindingSchema Schema() const {
            return {2, actions, suggestions, {}, {1}, registered};
        }

        XRSessionId Session() const {
            return {{Horo::Tests::IdentityValue<XRRuntimeGeneration>(1), {1, 1}}, {1, 1}};
        }
    };

    template <typename T> void RequireError(const Result<T> &result, std::string_view code) {
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == code);
        REQUIRE_FALSE(result.ErrorValue().message.empty());
    }

    struct Neutralizer final : IXRBindingNeutralizer {
        unsigned calls{};
        std::uint64_t lastRevision{};
        XRSessionId lastSession;

        void Neutralize(const XRSessionId &session, std::uint64_t revision) noexcept override {
            ++calls;
            lastRevision = revision;
            lastSession = session;
        }
    };

    TEST_CASE("XR bindings are canonical for equivalent action catalog suggestion and profile enumeration", "[xr][input][bindings]") {
        Fixture fixture;
        const auto original = ResolveXRActionBindings(fixture.Schema(), fixture.controls, fixture.profiles);
        REQUIRE(original.HasValue());
        REQUIRE(original.Value().bindings.size() == 2);
        REQUIRE(original.Value().bindings.front().action == ActionId{"move"});
        std::ranges::reverse(fixture.actions);
        std::ranges::reverse(fixture.controls);
        std::ranges::reverse(fixture.suggestions);
        std::ranges::reverse(fixture.profiles);
        const auto shuffled = ResolveXRActionBindings(fixture.Schema(), fixture.controls, fixture.profiles);
        REQUIRE(shuffled.HasValue());
        REQUIRE(shuffled.Value() == original.Value());
    }

    TEST_CASE("XR unknown and partial profiles use only declared generic fallback", "[xr][input][bindings]") {
        Fixture fixture;
        fixture.profiles[0].profile = {999};
        fixture.profiles[1].profile = {};
        const auto fallback = ResolveXRActionBindings(fixture.Schema(), fixture.controls, fixture.profiles);
        REQUIRE(fallback.HasValue());
        REQUIRE(fallback.Value().bindings.size() == 1);
        REQUIRE(fallback.Value().bindings[0].fallback);
        REQUIRE(fallback.Value().bindings[0].profile == XRInteractionProfileId{1});
        auto schema = fixture.Schema();
        schema.genericFallback = {};
        RequireError(ResolveXRActionBindings(schema, fixture.controls, fixture.profiles), "xr.operation.unsupported");
    }

    TEST_CASE("XR compatible override migration preserves semantic action role and value type", "[xr][input][bindings]") {
        Fixture fixture;
        const std::array migration{XRBindingMigration{1, {77}, {3}}};
        const std::array overrides{XRActionBindingOverride{1, ActionId{"select"}, {2}, {77}}};
        auto schema = fixture.Schema();
        schema.migrations = migration;
        const auto result = ResolveXRActionBindings(schema, fixture.controls, fixture.profiles, overrides);
        REQUIRE(result.HasValue());
        const auto &binding = result.Value().bindings.back();
        REQUIRE(binding.action == ActionId{"select"});
        REQUIRE(binding.control == XRPhysicalControlId{3});
        REQUIRE(binding.migratedOverride);
        REQUIRE(overrides[0].control == XRPhysicalControlId{77});
        schema.migrations = {};
        RequireError(ResolveXRActionBindings(schema, fixture.controls, fixture.profiles, overrides), "xr.operation.incompatible");
    }

    TEST_CASE("XR incompatible schemas and override conflicts fail atomically", "[xr][input][bindings]") {
        Fixture fixture;
        auto schema = fixture.Schema();
        std::array overrides{XRActionBindingOverride{2, ActionId{"select"}, {2}, {3}}};
        SECTION("future override version") {
            overrides[0].schemaVersion = 3;
        }
        SECTION("removed semantic action") {
            overrides[0].action = ActionId{"removed"};
        }
        SECTION("incompatible value type or role") {
            overrides[0].control = {2};
        }
        RequireError(ResolveXRActionBindings(schema, fixture.controls, fixture.profiles, overrides), "xr.operation.incompatible");
    }

    TEST_CASE("XR duplicate overrides and shared controls within one context are conflicts", "[xr][input][bindings]") {
        Fixture fixture;
        const std::array overrides{XRActionBindingOverride{2, ActionId{"select"}, {2}, {1}},
                                   XRActionBindingOverride{2, ActionId{"select"}, {2}, {3}}};
        RequireError(ResolveXRActionBindings(fixture.Schema(), fixture.controls, fixture.profiles, overrides), "xr.operation.incompatible");
        fixture.actions[1].role = XRTrackedDeviceRole::LeftController;
        fixture.actions[1].valueType = ActionValueType::Digital;
        fixture.registered[1].valueType = ActionValueType::Digital;
        fixture.suggestions[2].control = {1};
        RequireError(ResolveXRActionBindings(fixture.Schema(), fixture.controls, fixture.profiles), "xr.operation.incompatible");
        fixture.actions[1].context = InputContextId{"editor"};
        fixture.registered[1].context = InputContextId{"editor"};
        REQUIRE(ResolveXRActionBindings(fixture.Schema(), fixture.controls, fixture.profiles).HasValue());
    }

    TEST_CASE("XR schema bounds duplicate roles and invalid action contracts reject admission", "[xr][input][bindings]") {
        Fixture fixture;
        SECTION("duplicate active role") {
            fixture.profiles[1].role = fixture.profiles[0].role;
            RequireError(ResolveXRActionBindings(fixture.Schema(), fixture.controls, fixture.profiles), "xr.operation.incompatible");
        }
        SECTION("oversized catalog") {
            const std::vector<XRProfileControl> oversized(XRActionBindingLimits::MaximumBindings + 1);
            RequireError(ResolveXRActionBindings(fixture.Schema(), oversized, fixture.profiles), "xr.operation.capacity_exceeded");
        }
        SECTION("unsupported tracked role") {
            fixture.actions[0].role = XRTrackedDeviceRole::Head;
            REQUIRE(ValidateXRActionBindings(fixture.Schema(), fixture.controls, {}).HasError());
        }
        SECTION("invalid schema") {
            auto schema = fixture.Schema();
            schema.version = 0;
            REQUIRE(ValidateXRActionBindings(schema, fixture.controls, {}).HasError());
        }
        SECTION("unregistered action") {
            fixture.registered[0].id = ActionId{"another_action"};
            RequireError(ResolveXRActionBindings(fixture.Schema(), fixture.controls, fixture.profiles), "xr.operation.incompatible");
        }
        SECTION("Input registry owns value type") {
            fixture.registered[0].valueType = ActionValueType::Axis1D;
            RequireError(ResolveXRActionBindings(fixture.Schema(), fixture.controls, fixture.profiles), "xr.operation.incompatible");
        }
    }

    TEST_CASE("XR hot profile replacement unsupported profile and shutdown commit neutral boundaries", "[xr][input][bindings][lifecycle]") {
        Fixture fixture;
        Neutralizer neutralizer;
        XRActionBindingCoordinator coordinator{fixture.Session(), neutralizer};
        REQUIRE(coordinator.Update(fixture.Session(), fixture.Schema(), fixture.controls, fixture.profiles).HasValue());
        REQUIRE(coordinator.Revision() == 1);
        REQUIRE(neutralizer.calls == 0);
        std::ranges::reverse(fixture.profiles);
        REQUIRE(coordinator.Update(fixture.Session(), fixture.Schema(), fixture.controls, fixture.profiles).HasValue());
        REQUIRE(coordinator.Revision() == 1);
        REQUIRE(neutralizer.calls == 0);
        fixture.profiles[0].profile = {1};
        REQUIRE(coordinator.Update(fixture.Session(), fixture.Schema(), fixture.controls, fixture.profiles).HasValue());
        REQUIRE(coordinator.Revision() == 2);
        REQUIRE(neutralizer.calls == 1);
        REQUIRE(neutralizer.lastRevision == 1);
        fixture.profiles[1].profile = {999};
        auto unsupported = fixture.Schema();
        unsupported.genericFallback = {};
        REQUIRE(coordinator.Update(fixture.Session(), unsupported, fixture.controls, fixture.profiles).HasError());
        REQUIRE(coordinator.Current() == nullptr);
        REQUIRE(neutralizer.calls == 2);
        REQUIRE(coordinator.Update(fixture.Session(), fixture.Schema(), fixture.controls, fixture.profiles).HasValue());
        REQUIRE(coordinator.Current() != nullptr);
        coordinator.Shutdown();
        coordinator.Shutdown();
        REQUIRE(neutralizer.calls == 3);
        REQUIRE(neutralizer.lastSession == fixture.Session());
        REQUIRE(coordinator.Current() == nullptr);
        REQUIRE(coordinator.Update(fixture.Session(), fixture.Schema(), fixture.controls, fixture.profiles).HasError());
    }

    TEST_CASE("XR foreign session cannot neutralize a current binding publication", "[xr][input][bindings][lifecycle]") {
        Fixture fixture;
        Neutralizer neutralizer;
        XRActionBindingCoordinator coordinator{fixture.Session(), neutralizer};
        REQUIRE(coordinator.Update(fixture.Session(), fixture.Schema(), fixture.controls, fixture.profiles).HasValue());
        auto foreign = fixture.Session();
        ++foreign.slot.generation;
        REQUIRE(coordinator.Update(foreign, fixture.Schema(), fixture.controls, {}).HasError());
        REQUIRE(coordinator.Current() != nullptr);
        REQUIRE(neutralizer.calls == 0);
    }
}  // namespace
