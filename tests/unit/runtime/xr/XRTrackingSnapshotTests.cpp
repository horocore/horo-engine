#include "Horo/XR/XRErrors.h"
#include "Horo/XR/XRTrackingSnapshot.h"
#include "support/AllocationProbe.h"
#include "support/TypedIdentityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>
#include <vector>

namespace Horo::XR {
    namespace {
        [[nodiscard]] XRSessionId ActiveSession(const std::uint32_t generation = 5) {
            return {{Horo::Tests::IdentityValue<XRRuntimeGeneration>(1), {2, 3}}, {4, generation}};
        }

        [[nodiscard]] XRCoordinateSpace TrackingSpace(const XRSessionId &session, const XRSpaceKind kind, const std::uint32_t index) {
            return {.id = {session, {index, 1}}, .kind = kind, .worldOriginRevision = Horo::Tests::IdentityValue<XRWorldOriginRevision>(9)};
        }

        [[nodiscard]] XRPoseSample PoseSample(const XRSessionId &session, const XRRuntimeTime sampledAt) {
            const auto trackedPosition =
                XRPoseComponent<Math::Vec3>{.value = Math::Vec3{1.0F, 2.0F, 3.0F}, .validity = XRPoseComponentValidity::Tracked};
            const auto trackedOrientation =
                XRPoseComponent<Math::Quaternion>{.value = Math::Quaternion::Identity(), .validity = XRPoseComponentValidity::Tracked};
            auto result =
                XRPoseSample::Create({.session = session,
                                      .source = TrackingSpace(session, XRSpaceKind::View, 11),
                                      .target = TrackingSpace(session, XRSpaceKind::Local, 12),
                                      .purpose = XRPosePurpose::SimulationInput,
                                      .time = {.runtimeSample = sampledAt, .simulation = Horo::Tests::IdentityValue<XRSimulationTime>(80)},
                                      .components = {.positionMeters = trackedPosition,
                                                     .orientation = trackedOrientation,
                                                     .confidence = XRTrackingConfidence::High,
                                                     .loss = XRTrackingLossState::None}},
                                     session, Horo::Tests::IdentityValue<XRWorldOriginRevision>(9));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        [[nodiscard]] XRTrackedDeviceCapabilities Capabilities(const std::initializer_list<XRTrackedDeviceCapability> capabilities) {
            XRTrackedDeviceCapabilities result;
            for (const auto capability : capabilities)
                result.values[static_cast<std::size_t>(capability)] = true;
            return result;
        }

        class TrackingFixture final {
        public:
            TrackingFixture()
                : session(ActiveSession()), sampledAt(Horo::Tests::IdentityValue<XRRuntimeTime>(100)),
                  origin(Horo::Tests::IdentityValue<XRWorldOriginRevision>(9)),
                  revision(Horo::Tests::IdentityValue<XRTrackingSnapshotRevision>(7)),
                  head{.id = {session, {20, 1}},
                       .role = XRTrackedDeviceRole::Head,
                       .state = XRTrackedDeviceState::Tracked,
                       .capabilities = Capabilities({XRTrackedDeviceCapability::HeadPose})},
                  controller{.id = {session, {21, 1}},
                             .role = XRTrackedDeviceRole::LeftController,
                             .state = XRTrackedDeviceState::Tracked,
                             .capabilities = Capabilities({XRTrackedDeviceCapability::GripPose, XRTrackedDeviceCapability::AimPose})} {
                devices = {head, controller};
                poses.emplace_back(XRTrackedPoseRecord{head.id, XRTrackedPoseKind::Head, PoseSample(session, sampledAt)});
                poses.emplace_back(XRTrackedPoseRecord{controller.id, XRTrackedPoseKind::Grip, PoseSample(session, sampledAt)});
            }

            [[nodiscard]] XRTrackingSnapshotDescriptor Descriptor() const {
                return {.session = session,
                        .revision = revision,
                        .sequence = 41,
                        .sampledAt = sampledAt,
                        .worldOriginRevision = origin,
                        .focus = XRTrackingFocusState::Focused,
                        .visibility = XRTrackingVisibilityState::Visible,
                        .state = XRTrackingState::Active,
                        .limits = {.maximumDevices = 8, .maximumPoses = 16},
                        .devices = devices,
                        .poses = poses};
            }

            [[nodiscard]] XRTrackingSnapshot Snapshot() const {
                auto result = XRTrackingSnapshot::Create(Descriptor(), session, origin);
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            XRSessionId session;
            XRRuntimeTime sampledAt;
            XRWorldOriginRevision origin;
            XRTrackingSnapshotRevision revision;
            XRTrackedDeviceRecord head;
            XRTrackedDeviceRecord controller;
            std::vector<XRTrackedDeviceRecord> devices;
            std::vector<XRTrackedPoseRecord> poses;
        };

        template <typename Value> [[nodiscard]] bool HasErrorIdentity(const Result<Value> &result, const ErrorCodeDescriptor &descriptor) {
            return result.HasError() && result.ErrorValue().domain.Value() == descriptor.domain.Value() &&
                   result.ErrorValue().code.Value() == descriptor.code.Value();
        }

        void RequireInvalidSnapshot(const TrackingFixture &fixture) {
            REQUIRE(HasErrorIdentity(XRTrackingSnapshot::Create(fixture.Descriptor(), fixture.session, fixture.origin),
                                     XRErrors::TrackingSnapshotInvalid));
        }
    }  // namespace

    TEST_CASE("XR tracking snapshots own bounded canonical evidence", "[unit][xr][tracking-snapshot]") {
        TrackingFixture fixture;
        auto snapshot = XRTrackingSnapshot::Create(fixture.Descriptor(), fixture.session, fixture.origin);
        REQUIRE(snapshot.HasValue());
        REQUIRE(snapshot.Value().Revision() == fixture.revision);
        REQUIRE(snapshot.Value().Sequence() == 41);
        REQUIRE(snapshot.Value().SampledAt() == fixture.sampledAt);
        REQUIRE(snapshot.Value().Focus() == XRTrackingFocusState::Focused);
        REQUIRE(snapshot.Value().Visibility() == XRTrackingVisibilityState::Visible);
        REQUIRE(snapshot.Value().Devices().size() == 2);
        REQUIRE(snapshot.Value().Poses().size() == 2);

        fixture.devices.clear();
        fixture.poses.clear();
        REQUIRE(snapshot.Value().Devices().size() == 2);
        static_assert(std::is_same_v<decltype(snapshot.Value().Devices()), std::span<const XRTrackedDeviceRecord>>);
    }

    TEST_CASE("XR tracking queries are allocation-free and preserve exact confidence", "[unit][xr][tracking-snapshot]") {
        const TrackingFixture fixture;
        const auto snapshot = fixture.Snapshot();
        const auto before = Tests::AllocationProbe::Count();
        auto pose =
            QueryXRTrackedPose(snapshot, fixture.controller.id, XRTrackedPoseKind::Grip, fixture.session, fixture.revision, fixture.origin);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(pose.HasValue());
        REQUIRE(pose.Value().Components().confidence == XRTrackingConfidence::High);
        REQUIRE(pose.Value().Components().positionMeters.validity == XRPoseComponentValidity::Tracked);
        CHECK(after == before);
    }

    TEST_CASE("XR tracking snapshots reject malformed versions bounds and canonical order", "[unit][xr][tracking-snapshot]") {
        TrackingFixture fixture;
        auto descriptor = fixture.Descriptor();
        descriptor.contractVersion = {};
        REQUIRE(
            HasErrorIdentity(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin), XRErrors::ContractVersionInvalid));

        descriptor = fixture.Descriptor();
        descriptor.limits.maximumDevices = 1;
        REQUIRE(HasErrorIdentity(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin), XRErrors::CapacityExceeded));

        descriptor = fixture.Descriptor();
        descriptor.limits = {.maximumDevices = XRTrackingSnapshotHardLimits::MaximumDevices,
                             .maximumPoses = XRTrackingSnapshotHardLimits::MaximumPoses};
        REQUIRE(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin).HasValue());

        descriptor.limits.maximumPoses = XRTrackingSnapshotHardLimits::MaximumPoses + 1;
        REQUIRE(HasErrorIdentity(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin), XRErrors::CapacityExceeded));

        descriptor = fixture.Descriptor();
        std::swap(fixture.devices[0], fixture.devices[1]);
        descriptor.devices = fixture.devices;
        REQUIRE(
            HasErrorIdentity(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin), XRErrors::TrackingSnapshotInvalid));

        fixture.devices = {fixture.head, fixture.controller};
        fixture.devices.push_back(fixture.controller);
        RequireInvalidSnapshot(fixture);

        fixture.devices = {fixture.head, fixture.controller};
        auto duplicateHead = fixture.head;
        duplicateHead.id.slot.index = 22;
        fixture.devices.push_back(duplicateHead);
        RequireInvalidSnapshot(fixture);

        fixture.devices = {fixture.head, fixture.controller};
        fixture.devices[1].capabilities.values[static_cast<std::size_t>(XRTrackedDeviceCapability::HeadPose)] = true;
        RequireInvalidSnapshot(fixture);

        fixture.devices = {fixture.head, fixture.controller};
        fixture.devices[1].role = XRTrackedDeviceRole::Count;
        RequireInvalidSnapshot(fixture);
    }

    TEST_CASE("XR tracking snapshots reject contradictory device and pose evidence", "[unit][xr][tracking-snapshot]") {
        TrackingFixture fixture;
        auto descriptor = fixture.Descriptor();
        fixture.devices[0].state = XRTrackedDeviceState::TrackingLost;
        descriptor.devices = fixture.devices;
        REQUIRE(
            HasErrorIdentity(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin), XRErrors::TrackingSnapshotInvalid));

        fixture.devices[0] = fixture.head;
        fixture.poses[0].kind = XRTrackedPoseKind::Grip;
        descriptor = fixture.Descriptor();
        REQUIRE(
            HasErrorIdentity(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin), XRErrors::TrackingSnapshotInvalid));

        fixture.poses[0] = XRTrackedPoseRecord{fixture.head.id, XRTrackedPoseKind::Head,
                                               PoseSample(fixture.session, Horo::Tests::IdentityValue<XRRuntimeTime>(101))};
        descriptor = fixture.Descriptor();
        REQUIRE(
            HasErrorIdentity(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin), XRErrors::TrackingSnapshotInvalid));
    }

    TEST_CASE("XR tracking loss clears all pose evidence without fabricating a replacement", "[unit][xr][tracking-snapshot]") {
        TrackingFixture fixture;
        fixture.poses.clear();
        for (auto &device : fixture.devices)
            device.state = XRTrackedDeviceState::TrackingLost;
        auto descriptor = fixture.Descriptor();
        descriptor.state = XRTrackingState::TrackingLost;
        auto snapshot = XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin);
        REQUIRE(snapshot.HasValue());
        REQUIRE(snapshot.Value().Poses().empty());
        REQUIRE(HasErrorIdentity(QueryXRTrackedPose(snapshot.Value(), fixture.head.id, XRTrackedPoseKind::Head, fixture.session,
                                                    fixture.revision, fixture.origin),
                                 XRErrors::OperationUnavailable));

        descriptor.state = XRTrackingState::PermissionLost;
        REQUIRE(
            HasErrorIdentity(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin), XRErrors::TrackingSnapshotInvalid));
        fixture.devices.clear();
        descriptor = fixture.Descriptor();
        descriptor.state = XRTrackingState::PermissionLost;
        REQUIRE(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin).HasValue());
    }

    TEST_CASE("XR tracking fences replacement shutdown revision and origin", "[unit][xr][tracking-snapshot]") {
        const TrackingFixture fixture;
        const auto snapshot = fixture.Snapshot();
        REQUIRE(HasErrorIdentity(ValidateXRTrackingSnapshot(snapshot, ActiveSession(6), fixture.revision, fixture.origin),
                                 XRErrors::IdentityStale));
        REQUIRE(HasErrorIdentity(ValidateXRTrackingSnapshot(snapshot, {}, fixture.revision, fixture.origin), XRErrors::IdentityInvalid));
        REQUIRE(HasErrorIdentity(ValidateXRTrackingSnapshot(snapshot, fixture.session,
                                                            Horo::Tests::IdentityValue<XRTrackingSnapshotRevision>(8), fixture.origin),
                                 XRErrors::TrackingSnapshotStale));
        REQUIRE(HasErrorIdentity(ValidateXRTrackingSnapshot(snapshot, fixture.session, fixture.revision,
                                                            Horo::Tests::IdentityValue<XRWorldOriginRevision>(10)),
                                 XRErrors::OriginRevisionStale));

        auto replacedDevice = fixture.controller.id;
        ++replacedDevice.slot.generation;
        REQUIRE(HasErrorIdentity(QueryXRTrackedPose(snapshot, replacedDevice, XRTrackedPoseKind::Grip, fixture.session, fixture.revision,
                                                    fixture.origin),
                                 XRErrors::IdentityStale));
    }

    TEST_CASE("XR tracking queries distinguish unsupported from unavailable", "[unit][xr][tracking-snapshot]") {
        const TrackingFixture fixture;
        const auto snapshot = fixture.Snapshot();
        REQUIRE(HasErrorIdentity(QueryXRTrackedPose(snapshot, fixture.controller.id, XRTrackedPoseKind::Palm, fixture.session,
                                                    fixture.revision, fixture.origin),
                                 XRErrors::OperationUnsupported));

        auto unknown = fixture.controller.id;
        unknown.slot.index = 99;
        REQUIRE(HasErrorIdentity(QueryXRTrackedPose(snapshot, unknown, XRTrackedPoseKind::Grip, fixture.session, fixture.revision,
                                                    fixture.origin),
                                 XRErrors::OperationUnavailable));
        REQUIRE(HasErrorIdentity(QueryXRTrackedPose(snapshot, fixture.controller.id, XRTrackedPoseKind::Aim, fixture.session,
                                                    fixture.revision, fixture.origin),
                                 XRErrors::OperationUnavailable));
        REQUIRE(HasErrorIdentity(QueryXRTrackedPose(snapshot, fixture.controller.id, XRTrackedPoseKind::Count, fixture.session,
                                                    fixture.revision, fixture.origin),
                                 XRErrors::OperationInvalid));
    }

    TEST_CASE("XR action snapshots bind exact generation and plan revision independently of pose validity",
              "[unit][xr][tracking-snapshot]") {
        TrackingFixture fixture;
        fixture.poses.clear();
        fixture.devices[1].state = XRTrackedDeviceState::TrackingLost;
        const std::array actions{XRTrackedActionRecord{fixture.controller.id, 4, XRTrackedActionKind::Boolean, true, 1, 0}};
        auto descriptor = fixture.Descriptor();
        descriptor.limits.maximumActions = 8;
        descriptor.bindingRevision = 19;
        descriptor.actions = actions;
        const auto before = Tests::AllocationProbe::Count();
        auto candidate = XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(candidate.HasValue());
        CHECK(after == before);
        CHECK(candidate.Value().BindingRevision() == 19);
        REQUIRE(candidate.Value().Actions().size() == 1);
        CHECK(candidate.Value().Actions()[0] == actions[0]);
        const auto copyBefore = Tests::AllocationProbe::Count();
        const auto retained = candidate.Value();
        const auto copyAfter = Tests::AllocationProbe::Count();
        CHECK(copyBefore == copyAfter);
        CHECK(retained.Actions()[0].device == fixture.controller.id);
        CHECK(retained.Poses().empty());
    }

    TEST_CASE("XR action snapshot validation rejects complete malformed candidates atomically", "[unit][xr][tracking-snapshot]") {
        TrackingFixture fixture;
        std::array actions{XRTrackedActionRecord{fixture.controller.id, 4, XRTrackedActionKind::Boolean, true, 1, 0}};
        auto descriptor = fixture.Descriptor();
        descriptor.limits.maximumActions = 1;
        descriptor.bindingRevision = 19;
        descriptor.actions = actions;
        REQUIRE(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin).HasValue());
        descriptor.bindingRevision = 0;
        CHECK(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin).HasError());
        descriptor.bindingRevision = 19;
        for (const auto invalid : {XRTrackedActionRecord{fixture.controller.id, 4, XRTrackedActionKind::Boolean, true, 0.5F, 0},
                                   XRTrackedActionRecord{fixture.controller.id, 4, XRTrackedActionKind::Scalar, false, 1, 0},
                                   XRTrackedActionRecord{fixture.controller.id, 4, XRTrackedActionKind::Scalar, true, 1, 1}}) {
            actions[0] = invalid;
            CHECK(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin).HasError());
        }
        actions[0] = {fixture.controller.id, 4, XRTrackedActionKind::Boolean, true, 1, 0};
        descriptor.focus = XRTrackingFocusState::Unfocused;
        CHECK(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin).HasError());
        descriptor.focus = XRTrackingFocusState::Focused;
        actions[0].device = {fixture.session, {21, 2}};
        CHECK(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin).HasError());
        CHECK(actions[0].device.slot.generation == 2);
    }

    TEST_CASE("XR action snapshot ordering and aggregate capacity are finite", "[unit][xr][tracking-snapshot]") {
        TrackingFixture fixture;
        const XRTrackedActionRecord value{fixture.controller.id, 4, XRTrackedActionKind::Boolean, true, 1, 0};
        const std::array duplicate{value, value};
        auto descriptor = fixture.Descriptor();
        descriptor.limits.maximumActions = 1;
        descriptor.bindingRevision = 2;
        descriptor.actions = duplicate;
        CHECK(HasErrorIdentity(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin), XRErrors::CapacityExceeded));
        descriptor.limits.maximumActions = 2;
        CHECK(HasErrorIdentity(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin), XRErrors::TrackingSnapshotInvalid));
        descriptor.limits.maximumActions = XRTrackingSnapshotHardLimits::MaximumActions + 1;
        CHECK(HasErrorIdentity(XRTrackingSnapshot::Create(descriptor, fixture.session, fixture.origin), XRErrors::CapacityExceeded));
    }

    TEST_CASE("XR inline snapshot storage has an explicit finite copy budget", "[unit][xr][tracking-snapshot][storage-budget]") {
        CHECK(sizeof(XRTrackingSnapshot) <= 256U * 1024U);
        CHECK(sizeof(XRTrackedDeviceRecord) <= 128U);
        CHECK(sizeof(XRTrackedPoseRecord) <= 512U);
        CHECK(sizeof(XRTrackedActionRecord) <= 128U);
        static_assert(std::is_nothrow_copy_constructible_v<XRTrackingSnapshot>);
    }
}  // namespace Horo::XR
