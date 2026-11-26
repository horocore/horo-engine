#include "Horo/XR/XRCapabilities.h"
#include "Horo/XR/XRInteraction.h"
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

    TEST_CASE("XR gesture policy reads exact immutable joint evidence without allocations", "[xr][hands][gesture]") {
        HandFixture fixture;
        const auto thumb = Joint(fixture.session, fixture.device.id, XRHandedness::Left, XRHandJoint::ThumbTip);
        const auto index = Joint(fixture.session, fixture.device.id, XRHandedness::Left, XRHandJoint::IndexTip);
        auto descriptor = fixture.Descriptor();
        const std::array joints{thumb, index};
        descriptor.handJoints = joints;
        const auto snapshot = XRTrackingSnapshot::Create(descriptor, fixture.session, Origin());
        REQUIRE(snapshot.HasValue());
        XRContactPinchGesturePolicy contact;
        const auto before = Tests::AllocationProbe::Count();
        const auto gestures = DeriveXRHandGestures(snapshot.Value(), fixture.session, descriptor.revision, Origin(), contact);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(gestures.HasValue());
        CHECK(before == after);
        REQUIRE(gestures.Value().Candidates().size() == 1);
        CHECK(gestures.Value().Candidates().front().confidence == XRTrackingConfidence::High);
        CHECK(snapshot.Value().HandJoints().size() == 2);
        CHECK(DeriveXRHandGestures(snapshot.Value(), fixture.session, Value<XRTrackingSnapshotRevision>(2), Origin(), contact).HasError());

        class SuppressGesture final : public IXRHandGesturePolicy {
        public:
            explicit SuppressGesture(const XRHandJointRecord *published) : published_(published) {}

            mutable bool detached{};

        private:
            std::optional<std::pair<XRHandGestureKind, XRTrackingConfidence>> Classify(
                const std::span<const XRHandJointRecord> input) const noexcept override {
                detached = input.data() != published_ && input.size() == 2;
                return std::nullopt;
            }

            const XRHandJointRecord *published_;
        } suppress{snapshot.Value().HandJoints().data()};

        const auto empty = DeriveXRHandGestures(snapshot.Value(), fixture.session, descriptor.revision, Origin(), suppress);
        REQUIRE(empty.HasValue());
        CHECK(suppress.detached);
        CHECK(empty.Value().Candidates().empty());
        CHECK(snapshot.Value().HandJoints().size() == 2);

        const std::array onlyThumb{thumb};
        descriptor.handJoints = onlyThumb;
        const auto partial = XRTrackingSnapshot::Create(descriptor, fixture.session, Origin());
        REQUIRE(partial.HasValue());
        const auto noTip = DeriveXRHandGestures(partial.Value(), fixture.session, descriptor.revision, Origin(), contact);
        REQUIRE(noTip.HasValue());
        CHECK(noTip.Value().Candidates().empty());
    }

    TEST_CASE("Legacy native XR snapshots default to no hands and preserve existing enum ordinals", "[xr][hands]") {
        static_assert(static_cast<int>(XRTrackedDeviceRole::GenericTracker) == 3);
        static_assert(static_cast<int>(XRTrackedDeviceCapability::Haptics) == 5);
        static_assert(static_cast<int>(XRCapability::SpaceWarp) == 31);
        CHECK(XRTrackingSnapshotLimits{}.maximumHandJoints == 0);
        CHECK(XRTrackingSnapshotDescriptor{}.handJoints.empty());
        CHECK_FALSE(XRTrackedDeviceCapabilities{}.Has(XRTrackedDeviceCapability::HandJoints));
    }

    TEST_CASE("Interaction geometry is bounded and shares the exact focused world tracking cutoff", "[xr][interaction]") {
        HandFixture fixture;
        auto pose = Pose(fixture.session);
        pose.target.kind = XRSpaceKind::World;
        pose.components.orientation.value = Math::Quaternion::FromAxisAngle({0, 1, 0}, 1.57079632679F);
        const auto joint = XRPoseSample::Create(pose, fixture.session, Origin());
        REQUIRE(joint.HasValue());
        fixture.joints.front().sample = joint.Value();
        auto descriptor = fixture.Descriptor();
        const XRInteractionProbe direct{fixture.device.id, XRHandJoint::Palm, XRInteractionDirectProbe{}, XRTrackingConfidence::High};
        const auto sample = XRTrackingSnapshot::Create(descriptor, fixture.session, Origin());
        REQUIRE(sample.HasValue());
        const auto cutoff = DescribeXRInteractionCutoff(sample.Value());
        const std::array probes{XRInteractionProbe{direct.device, direct.pose, XRInteractionRayProbe{{0, 0, -2}, 4},
                                                   direct.minimumConfidence},
                                direct,
                                XRInteractionProbe{direct.device, direct.pose, XRInteractionProximityProbe{0.25F},
                                                   direct.minimumConfidence}};
        const auto before = Tests::AllocationProbe::Count();
        const auto geometry = BuildXRInteractionCandidates(sample.Value(), cutoff, probes);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(geometry.HasValue());
        CHECK(before == after);
        CHECK(geometry.Value().Cutoff() == cutoff);
        const auto candidates = geometry.Value().Candidates();
        REQUIRE(candidates.size() == probes.size());
        const auto &ray = std::get<Math::Ray>(candidates[0].shape);
        CHECK(ray.direction.x < -0.999F);
        CHECK(Math::LengthSquared(ray.direction) >= 0.999F);
        CHECK(ray.maximumDistance == 4.0F);
        const auto point = std::get<Math::Vec3>(candidates[1].shape);
        CHECK((point == Math::Vec3{1, 2, 3}));
        const auto sphere = std::get<Math::BoundingSphere>(candidates[2].shape);
        CHECK(sphere.center == point);
        CHECK(sphere.radius == 0.25F);
        for (const auto &candidate : candidates) {
            CHECK(candidate.space == pose.target);
            CHECK(candidate.device == fixture.device.id);
        }

        std::array<XRInteractionProbe, XRInteractionCandidates::MaximumProbes + 1> full;
        full.fill(direct);
        CHECK(BuildXRInteractionCandidates(sample.Value(), cutoff, std::span{full}.first(full.size() - 1)).HasValue());
        CHECK(ErrorIs(BuildXRInteractionCandidates(sample.Value(), cutoff, full), XRErrors::CapacityExceeded));

        auto wrongDevice = direct;
        ++wrongDevice.device.slot.generation;
        CHECK(ErrorIs(BuildXRInteractionCandidates(sample.Value(), cutoff, std::array{direct, wrongDevice}), XRErrors::IdentityStale));
        auto wrongKind = direct;
        wrongKind.pose = XRControllerInteractionPose::Aim;
        CHECK(ErrorIs(BuildXRInteractionCandidates(sample.Value(), cutoff, std::array{direct, wrongKind}), XRErrors::OperationUnsupported));
        auto malformed = direct;
        malformed.shape = XRInteractionRayProbe{{0, 0, 0}, 4};
        CHECK(ErrorIs(BuildXRInteractionCandidates(sample.Value(), cutoff, std::array{direct, malformed}), XRErrors::OperationInvalid));
        malformed.shape = XRInteractionProximityProbe{std::numeric_limits<float>::infinity()};
        CHECK(ErrorIs(BuildXRInteractionCandidates(sample.Value(), cutoff, std::array{direct, malformed}), XRErrors::OperationInvalid));
        auto stale = cutoff;
        stale.origin = Value<XRWorldOriginRevision>(2);
        CHECK(ErrorIs(BuildXRInteractionCandidates(sample.Value(), stale, probes), XRErrors::OriginRevisionStale));
        stale = cutoff;
        stale.sampledAt = Value<XRRuntimeTime>(101);
        CHECK(ErrorIs(BuildXRInteractionCandidates(sample.Value(), stale, probes), XRErrors::TrackingSnapshotStale));
    }

    TEST_CASE("One interaction batch cannot combine separate World spaces from the same cutoff", "[xr][interaction]") {
        HandFixture fixture;
        std::array joints{fixture.joints.front(), Joint(fixture.session, fixture.device.id, XRHandedness::Left, XRHandJoint::IndexTip)};
        auto pose = Pose(fixture.session);
        pose.target.kind = XRSpaceKind::World;
        for (auto &joint : joints) {
            const auto sample = XRPoseSample::Create(pose, fixture.session, Origin());
            REQUIRE(sample.HasValue());
            joint.sample = sample.Value();
            ++pose.target.id.slot.index;
        }
        auto descriptor = fixture.Descriptor();
        descriptor.handJoints = joints;
        const auto snapshot = XRTrackingSnapshot::Create(descriptor, fixture.session, Origin());
        REQUIRE(snapshot.HasValue());
        const auto cutoff = DescribeXRInteractionCutoff(snapshot.Value());
        const std::array probes{
            XRInteractionProbe{fixture.device.id, XRHandJoint::Palm, XRInteractionDirectProbe{}, XRTrackingConfidence::High},
            XRInteractionProbe{fixture.device.id, XRHandJoint::IndexTip, XRInteractionProximityProbe{0.25F}, XRTrackingConfidence::High}};
        CHECK(BuildXRInteractionCandidates(snapshot.Value(), cutoff, std::span{probes}.first(1)).HasValue());
        CHECK(BuildXRInteractionCandidates(snapshot.Value(), cutoff, std::span{probes}.last(1)).HasValue());
        CHECK(ErrorIs(BuildXRInteractionCandidates(snapshot.Value(), cutoff, probes), XRErrors::CoordinateSpaceIncompatible));
    }
}  // namespace Horo::XR
