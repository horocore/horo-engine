#include "Horo/XR/XRInputProjection.h"
#include "support/TypedIdentityTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::XR {
    namespace {
        struct ProjectionFixture final {
            Input::InputRouter router;
            Input::RawInputCollector collector;
            Input::ActionDescriptor action{Input::ActionId{"select"}, Input::ActionValueType::Digital, Input::InputContextId{"play"}};
            Input::InputContextToken context = router.PushContext(Input::InputContextId{"play"}, Input::InputContextKind::Gameplay);
            XRSessionId session{{Tests::IdentityValue<XRRuntimeGeneration>(1), {2, 1}}, {3, 1}};
            XRWorldOriginRevision origin = Tests::IdentityValue<XRWorldOriginRevision>(1);
            XRTrackingSnapshotRevision revision = Tests::IdentityValue<XRTrackingSnapshotRevision>(1);
            XRResolvedActionBindings bindings{1,
                                              {{.action = action.id,
                                                .context = action.context,
                                                .valueType = action.valueType,
                                                .set = {1},
                                                .role = XRTrackedDeviceRole::LeftController}}};
            XRTrackedDeviceRecord device{.id = {session, {1, 1}},
                                         .role = XRTrackedDeviceRole::LeftController,
                                         .state = XRTrackedDeviceState::TrackingLost};

            ProjectionFixture() {
                REQUIRE(router.SetActionMap({action}).HasValue());
                device.capabilities.values[static_cast<std::size_t>(XRTrackedDeviceCapability::GripPose)] = true;
            }

            Input::InputActionCollection Port() {
                auto port = router.ActionCollection(context);
                REQUIRE(port.HasValue());
                return std::move(port).Value();
            }

            XRTrackingSnapshot Snapshot(const std::uint64_t binding = 4) const {
                const std::array actions{XRTrackedActionRecord{device.id, 0, XRTrackedActionKind::Boolean, true, 1, 0}};
                auto value = XRTrackingSnapshot::Create({.session = session,
                                                         .revision = revision,
                                                         .sequence = 1,
                                                         .sampledAt = Tests::IdentityValue<XRRuntimeTime>(100),
                                                         .worldOriginRevision = origin,
                                                         .focus = XRTrackingFocusState::Focused,
                                                         .visibility = XRTrackingVisibilityState::Visible,
                                                         .state = XRTrackingState::Active,
                                                         .limits = {1, 1, 1},
                                                         .devices = std::span{&device, 1},
                                                         .bindingRevision = binding,
                                                         .actions = actions},
                                                        session, origin);
                REQUIRE(value.HasValue());
                return std::move(value).Value();
            }
        };
    }  // namespace

    TEST_CASE("XR scalar projection joins the existing Input cutoff without pose tracking or a second commit", "[unit][xr][input]") {
        ProjectionFixture fixture;
        auto port = fixture.Port();
        auto created = XRInputProjection::Create(port, fixture.session, fixture.bindings, 4);
        REQUIRE(created.HasValue());
        auto projection = std::move(created).Value();
        fixture.collector.BeginFrame(1);
        auto opened = port.Begin(fixture.collector.Commit());
        REQUIRE(opened.HasValue());
        auto frame = std::move(opened).Value();
        const auto snapshot = fixture.Snapshot();
        REQUIRE(projection.Append(snapshot, fixture.session, fixture.revision, fixture.origin, 4, frame).HasValue());
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
        REQUIRE(frame.Commit().HasValue());
        CHECK(fixture.router.ReadAction(fixture.context, fixture.action.id).pressed);
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).pressed);
    }

    TEST_CASE("XR projection refuses stale plan and session provenance before any Input candidate escapes", "[unit][xr][input]") {
        ProjectionFixture fixture;
        auto port = fixture.Port();
        auto created = XRInputProjection::Create(port, fixture.session, fixture.bindings, 4);
        REQUIRE(created.HasValue());
        auto projection = std::move(created).Value();
        auto opened = port.Begin(fixture.collector.Commit());
        REQUIRE(opened.HasValue());
        auto frame = std::move(opened).Value();
        CHECK(projection.Append(fixture.Snapshot(5), fixture.session, fixture.revision, fixture.origin, 4, frame).HasError());
        auto replaced = fixture.session;
        ++replaced.slot.generation;
        CHECK(projection.Append(fixture.Snapshot(), replaced, fixture.revision, fixture.origin, 4, frame).HasError());
        CHECK(projection.Append(fixture.Snapshot(), fixture.session, fixture.revision, fixture.origin, 5, frame).HasError());
        REQUIRE(frame.Commit().HasValue());
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
    }

    TEST_CASE("XR input preparation binds its same reservation only after canonical session issuance", "[unit][xr][input]") {
        ProjectionFixture fixture;
        auto port = fixture.Port();
        auto reserved = XRInputProjection::Prepare(port, fixture.bindings);
        REQUIRE(reserved.HasValue());
        auto projection = std::move(reserved).Value();
        auto opened = port.Begin(fixture.collector.Commit());
        REQUIRE(opened.HasValue());
        auto frame = std::move(opened).Value();
        CHECK(projection.Append(fixture.Snapshot(), fixture.session, fixture.revision, fixture.origin, 4, frame).HasError());
        CHECK(projection.Bind({}, 4).HasError());
        CHECK(projection.Bind(fixture.session, 0).HasError());
        REQUIRE(projection.Bind(fixture.session, 4).HasValue());
        auto foreign = fixture.session;
        ++foreign.slot.generation;
        CHECK(projection.Bind(foreign, 5).HasError());
        CHECK(projection.Bind(fixture.session, 4).HasError());
        REQUIRE(projection.Append(fixture.Snapshot(), fixture.session, fixture.revision, fixture.origin, 4, frame).HasValue());
        REQUIRE(frame.Commit().HasValue());
        CHECK(fixture.router.ReadAction(fixture.context, fixture.action.id).pressed);
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).pressed);
    }

    TEST_CASE("XR input bind cannot revive a reservation revoked during native preparation", "[unit][xr][input]") {
        ProjectionFixture fixture;
        auto prepared = XRInputProjection::Prepare(fixture.Port(), fixture.bindings);
        REQUIRE(prepared.HasValue());
        auto projection = std::move(prepared).Value();
        REQUIRE(fixture.router.SetActionMap({fixture.action}).HasValue());
        CHECK(projection.Bind(fixture.session, 4).HasError());
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
        REQUIRE(projection.Reset().HasValue());
        fixture.router.BeginFrame(fixture.collector.Commit());  // Actual neutral commit, not a registration-only reset.
        auto replacement = XRInputProjection::Create(fixture.Port(), fixture.session, fixture.bindings, 4);
        REQUIRE(replacement.HasValue());
    }

    TEST_CASE("XR cancelled preparation releases exactly one registration after a real neutral commit", "[unit][xr][input]") {
        ProjectionFixture fixture;
        auto port = fixture.Port();
        auto prepared = XRInputProjection::Prepare(port, fixture.bindings);
        REQUIRE(prepared.HasValue());
        auto projection = std::move(prepared).Value();
        std::array<std::optional<Input::InputActionSourceToken>, Input::MaximumCollectedInputSources - 1> retained;
        for (auto &slot : retained) {
            auto source = port.RegisterSource(std::span{&fixture.action, 1});
            REQUIRE(source.HasValue());
            slot.emplace(std::move(source).Value());
        }
        REQUIRE(projection.Reset().HasValue());
        REQUIRE(projection.Reset().HasValue());
        CHECK(projection.Bind(fixture.session, 4).HasError());
        CHECK(XRInputProjection::Prepare(port, fixture.bindings).HasError());
        fixture.router.BeginFrame(fixture.collector.Commit());
        auto replacement = XRInputProjection::Create(port, fixture.session, fixture.bindings, 4);
        REQUIRE(replacement.HasValue());
        CHECK(port.RegisterSource(std::span{&fixture.action, 1}).HasError());
    }

    TEST_CASE("XR atomic Create rejects zero binding revision before reserving a source", "[unit][xr][input]") {
        ProjectionFixture fixture;
        auto port = fixture.Port();
        CHECK(XRInputProjection::Create(port, fixture.session, fixture.bindings, 0).HasError());
        std::array<std::optional<Input::InputActionSourceToken>, Input::MaximumCollectedInputSources> retained;
        for (auto &slot : retained) {
            auto source = port.RegisterSource(std::span{&fixture.action, 1});
            REQUIRE(source.HasValue());
            slot.emplace(std::move(source).Value());
        }
    }
}  // namespace Horo::XR
