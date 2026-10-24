#include "support/OpenXRActionBindingTestSupport.h"

#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::XR;
    using namespace Horo::XR::OpenXRInternal;
    using namespace Horo::Tests::OpenXRAction;

    TEST_CASE("OpenXR scalar sampling requires a fresh synchronization and ignores undefined inactive payloads", "[xr][openxr][actions]") {
        ActionFixture fixture;
        REQUIRE(fixture.Create().HasValue());
        const auto plan = fixture.Plan();
        const std::array devices{fixture.Device()};
        const auto session = fixture.session.Request().candidate;
        CHECK(fixture.owner.Sample(session, plan, devices).HasError());
        REQUIRE(fixture.owner.Sync(session).HasValue());
        auto packet = fixture.owner.Sample(session, plan, devices);
        REQUIRE(packet.HasValue());
        REQUIRE(packet.Value().Actions().size() == 1);
        CHECK(packet.Value().Actions()[0].device == devices[0].id);
        CHECK(packet.Value().Actions()[0].active);
        CHECK(packet.Value().Actions()[0].x == 1.0F);
        CHECK(packet.Value().Actions()[0].bindingIndex == 0);
        CHECK(fixture.owner.Sample(session, plan, devices).HasError());
        fixture.script.sample.active = XR_FALSE;
        fixture.script.sample.boolean = 99;  // Undefined inactive payload is deliberately not interpreted.
        REQUIRE(fixture.owner.Sync(session).HasValue());
        packet = fixture.owner.Sample(session, plan, devices);
        REQUIRE(packet.HasValue());
        CHECK_FALSE(packet.Value().Actions()[0].active);
        CHECK(packet.Value().Actions()[0].x == 0.0F);
    }

    TEST_CASE("OpenXR scalar sampling snapshots caller carriers and fences reentrant native retirement", "[xr][openxr][actions]") {
        ActionFixture fixture;
        REQUIRE(fixture.Create().HasValue());
        auto plan = fixture.Plan();
        std::array devices{fixture.Device()};
        const auto expected = devices[0].id;
        fixture.script.sample.mutatePlan = &plan;
        fixture.script.sample.mutateDevice = &devices[0];
        fixture.script.sample.reentrantOwner = &fixture.owner;
        const auto session = fixture.session.Request().candidate;
        REQUIRE(fixture.owner.Sync(session).HasValue());
        const auto packet = fixture.owner.Sample(session, plan, devices);
        REQUIRE(packet.HasValue());
        REQUIRE(packet.Value().Actions().size() == 1);
        CHECK(packet.Value().Actions()[0].device == expected);
        CHECK(plan.bindings.empty());
        CHECK(devices[0].id != expected);
        CHECK(fixture.script.sample.closeRejected);
        CHECK(fixture.script.liveActions == 1);
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.script.liveActions == 0);
    }

    TEST_CASE("OpenXR scalar sampling preserves native errors and rejects malformed Boolean states atomically", "[xr][openxr][actions]") {
        ActionFixture fixture;
        REQUIRE(fixture.Create().HasValue());
        const auto plan = fixture.Plan();
        const std::array devices{fixture.Device()};
        const auto session = fixture.session.Request().candidate;
        REQUIRE(fixture.owner.Sync(session).HasValue());
        fixture.session.script.fail = "sample-boolean";
        const auto native = fixture.owner.Sample(session, plan, devices);
        REQUIRE(native.HasError());
        CHECK(native.ErrorValue().message.find("native result=") != std::string::npos);
        fixture.session.script.fail = {};
        fixture.script.sample.boolean = 2;
        REQUIRE(fixture.owner.Sync(session).HasValue());
        CHECK(fixture.owner.Sample(session, plan, devices).HasError());
        fixture.script.sample.boolean = XR_TRUE;
        auto foreign = session;
        ++foreign.slot.generation;
        REQUIRE(fixture.owner.Sync(session).HasValue());
        CHECK(fixture.owner.Sample(foreign, plan, devices).HasError());
        REQUIRE(fixture.owner.Sync(session).HasValue());
        REQUIRE(fixture.owner.Sample(session, plan, devices).HasValue());
    }

    TEST_CASE("OpenXR float and vector action states are finite normalized simulation values", "[xr][openxr][actions]") {
        for (const auto type : {Input::ActionValueType::Axis1D, Input::ActionValueType::Axis2D}) {
            ActionFixture fixture;
            fixture.registered[0].valueType = type;
            fixture.actions[0].valueType = type;
            for (auto &control : fixture.controls)
                control.valueType = type;
            fixture.script.sample.expectedType =
                type == Input::ActionValueType::Axis1D ? XR_ACTION_TYPE_FLOAT_INPUT : XR_ACTION_TYPE_VECTOR2F_INPUT;
            REQUIRE(fixture.Create().HasValue());
            const auto plan = fixture.Plan();
            const std::array devices{fixture.Device()};
            const auto session = fixture.session.Request().candidate;
            REQUIRE(fixture.owner.Sync(session).HasValue());
            const auto packet = fixture.owner.Sample(session, plan, devices);
            REQUIRE(packet.HasValue());
            REQUIRE(packet.Value().Actions().size() == 1);
            CHECK(packet.Value().Actions()[0].active);
            CHECK(packet.Value().Actions()[0].x == (type == Input::ActionValueType::Axis1D ? 0.5F : 0.25F));
            fixture.script.sample.scalar = std::numeric_limits<float>::quiet_NaN();
            fixture.script.sample.vector.y = 1.01F;
            REQUIRE(fixture.owner.Sync(session).HasValue());
            CHECK(fixture.owner.Sample(session, plan, devices).HasError());
        }
    }

    TEST_CASE("OpenXR scalar sampling requires complete dispatch before native action ownership", "[xr][openxr][actions]") {
        for (const auto function : {"xrGetActionStateBoolean", "xrGetActionStateFloat", "xrGetActionStateVector2f"}) {
            ActionFixture fixture;
            fixture.session.script.absent = function;
            REQUIRE(fixture.Create().HasError());
            CHECK(fixture.script.liveActions == 0);
            CHECK(fixture.script.liveSets == 0);
            CHECK(std::ranges::find(fixture.session.script.calls, "create-set") == fixture.session.script.calls.end());
        }
    }

    TEST_CASE("OpenXR failed resynchronization invalidates the previous unsampled opportunity", "[xr][openxr][actions]") {
        ActionFixture fixture;
        REQUIRE(fixture.Create().HasValue());
        const auto session = fixture.session.Request().candidate;
        const auto plan = fixture.Plan();
        const std::array devices{fixture.Device()};
        REQUIRE(fixture.owner.Sync(session).HasValue());
        fixture.session.script.fail = "sync";
        CHECK(fixture.owner.Sync(session).HasError());
        fixture.session.script.fail = {};
        CHECK(fixture.owner.Sample(session, plan, devices).HasError());
        REQUIRE(fixture.owner.Sync(session).HasValue());
        REQUIRE(fixture.owner.Sample(session, plan, devices).HasValue());
    }

}  // namespace
