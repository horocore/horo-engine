#include "Horo/XR/XRCapabilities.h"
#include "support/AllocationProbe.h"
#include "support/xr/XRHandTestSupport.h"

#include <limits>
#include <type_traits>

namespace Horo::XR {
    namespace {
        using namespace SimulatorTests;

        struct HandFixture final {
            XRSessionId session{{Value<XRRuntimeGeneration>(1), {1, 1}}, {1, 1}};
            XRTrackedDeviceRecord device{{session, {0, 1}}, XRTrackedDeviceRole::LeftHand, XRTrackedDeviceState::Tracked, {}};
            std::array<XRHandJointRecord, 1> joints{Joint(session, device.id)};

            HandFixture() {
                device.capabilities.values[static_cast<std::size_t>(XRTrackedDeviceCapability::HandJoints)] = true;
            }

            XRTrackingSnapshotDescriptor Descriptor() const {
                return {.session = session,
                        .revision = Value<XRTrackingSnapshotRevision>(1),
                        .sequence = 1,
                        .sampledAt = Value<XRRuntimeTime>(100),
                        .worldOriginRevision = Origin(),
                        .focus = XRTrackingFocusState::Focused,
                        .visibility = XRTrackingVisibilityState::Visible,
                        .state = XRTrackingState::Active,
                        .limits = {5, 5, 0, XRHandTrackingHardLimits::MaximumJoints},
                        .devices = {&device, 1},
                        .handJoints = joints};
            }
        };

        bool Accepted(const HandFixture &fixture, const XRTrackingSnapshotDescriptor &descriptor) {
            return XRTrackingSnapshot::Create(descriptor, fixture.session, Origin()).HasValue();
        }
    }  // namespace

    TEST_CASE("Canonical XR hands are bounded owned anatomical evidence with allocation free queries", "[xr][hands]") {
        HandFixture fixture;
        const auto descriptor = fixture.Descriptor();
        const auto origin = Origin();
        const auto before = Tests::AllocationProbe::Count();
        const auto snapshot = XRTrackingSnapshot::Create(descriptor, fixture.session, origin);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(snapshot.HasValue());
        CHECK(before == after);
        const auto queryBefore = Tests::AllocationProbe::Count();
        const auto joint =
            QueryXRHandJoint(snapshot.Value(), fixture.device.id, XRHandJoint::Palm, fixture.session, descriptor.revision, origin);
        const auto queryAfter = Tests::AllocationProbe::Count();
        REQUIRE(joint.HasValue());
        CHECK(queryBefore == queryAfter);
        REQUIRE(joint.Value().sample.has_value());
        const auto &position = joint.Value().sample->Components().positionMeters.value;
        REQUIRE(position.has_value());
        CHECK(position->x == 1);
        CHECK(joint.Value().radiusMeters == 0.01F);
        fixture.joints[0].radiusMeters = 0.5F;
        CHECK(snapshot.Value().HandJoints()[0].radiusMeters == 0.01F);
        CHECK(XRHandTrackingHardLimits::MaximumJoints == 52);
        INFO("snapshot=" << sizeof(XRTrackingSnapshot) << " joint=" << sizeof(XRHandJointRecord));
        CHECK(sizeof(XRTrackingSnapshot) <= 256U * 1024U);
        CHECK(sizeof(XRHandJointRecord) <= 512U);
    }

    TEST_CASE("Canonical XR hand records reject malformed values and controller aliases", "[xr][hands]") {
        HandFixture fixture;
        SECTION("controller cannot advertise anatomical joints") {
            fixture.device.role = XRTrackedDeviceRole::LeftController;
        }
        SECTION("handedness must match the device") {
            fixture.joints[0].hand = XRHandedness::Right;
        }
        SECTION("unknown anatomy") {
            fixture.joints[0].joint = XRHandJoint::Count;
        }
        SECTION("missing evidence") {
            fixture.joints[0].sample.reset();
        }
        SECTION("nonfinite radius") {
            fixture.joints[0].radiusMeters = std::numeric_limits<float>::quiet_NaN();
        }
        SECTION("negative radius") {
            fixture.joints[0].radiusMeters = -1;
        }
        SECTION("unfocused snapshots cannot retain live hands") {
            auto descriptor = fixture.Descriptor();
            descriptor.focus = XRTrackingFocusState::Unfocused;
            CHECK_FALSE(Accepted(fixture, descriptor));
            return;
        }
        SECTION("stale device generation") {
            ++fixture.joints[0].device.slot.generation;
        }
        SECTION("wrong cutoff") {
            fixture.joints[0] = Joint(fixture.session, fixture.device.id, XRHandedness::Left, XRHandJoint::Palm, 101);
        }
        CHECK_FALSE(Accepted(fixture, fixture.Descriptor()));
    }

    TEST_CASE("Canonical XR hand admission rejects overflow duplicate anatomy and stale queries", "[xr][hands]") {
        HandFixture fixture;
        auto descriptor = fixture.Descriptor();
        descriptor.limits.maximumHandJoints = 0;
        CHECK_FALSE(Accepted(fixture, descriptor));
        descriptor = fixture.Descriptor();
        const std::array duplicates{fixture.joints[0], fixture.joints[0]};
        descriptor.handJoints = duplicates;
        CHECK_FALSE(Accepted(fixture, descriptor));
        descriptor = fixture.Descriptor();
        const auto snapshot = XRTrackingSnapshot::Create(descriptor, fixture.session, Origin()).Value();
        auto stale = fixture.session;
        ++stale.slot.generation;
        CHECK(ErrorIs(QueryXRHandJoint(snapshot, fixture.device.id, XRHandJoint::Palm, stale, descriptor.revision, Origin()),
                      XRErrors::IdentityStale));
        CHECK(ErrorIs(QueryXRHandJoint(snapshot, fixture.device.id, XRHandJoint::Palm, fixture.session, descriptor.revision,
                                       Value<XRWorldOriginRevision>(2)),
                      XRErrors::OriginRevisionStale));
        CHECK(ErrorIs(QueryXRHandJoint(snapshot, fixture.device.id, XRHandJoint::Wrist, fixture.session, descriptor.revision, Origin()),
                      XRErrors::OperationUnavailable));
    }

    TEST_CASE("Canonical XR hand loss publishes empty evidence rather than stale joints", "[xr][hands]") {
        HandFixture fixture;
        fixture.device.state = XRTrackedDeviceState::TrackingLost;
        auto descriptor = fixture.Descriptor();
        descriptor.state = XRTrackingState::TrackingLost;
        CHECK_FALSE(Accepted(fixture, descriptor));
        descriptor.handJoints = {};
        const auto snapshot = XRTrackingSnapshot::Create(descriptor, fixture.session, Origin());
        REQUIRE(snapshot.HasValue());
        CHECK(snapshot.Value().HandJoints().empty());
        CHECK(ErrorIs(QueryXRHandJoint(snapshot.Value(), fixture.device.id, XRHandJoint::Palm, fixture.session, descriptor.revision,
                                       Origin()),
                      XRErrors::OperationUnavailable));
    }

    TEST_CASE("Legacy native XR snapshots default to no hands and preserve existing enum ordinals", "[xr][hands]") {
        static_assert(static_cast<int>(XRTrackedDeviceRole::GenericTracker) == 3);
        static_assert(static_cast<int>(XRTrackedDeviceCapability::Haptics) == 5);
        static_assert(static_cast<int>(XRCapability::SpaceWarp) == 31);
        CHECK(XRTrackingSnapshotLimits{}.maximumHandJoints == 0);
        CHECK(XRTrackingSnapshotDescriptor{}.handJoints.empty());
        CHECK_FALSE(XRTrackedDeviceCapabilities{}.Has(XRTrackedDeviceCapability::HandJoints));
    }
}  // namespace Horo::XR
