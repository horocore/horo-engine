#include "XRTrackingPublication.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::XR::RuntimeInternal {
    namespace {
        template <typename Type> Type Value(const std::uint64_t value) {
            const auto made = Type::Create(value);
            REQUIRE(made.HasValue());
            return made.Value();
        }

        struct Resources final : IXRSessionResources {
            XRTrackingPublication *tracking{};

            Result<void> Prepare(XRSessionPreparation stage, const XRSessionId &candidate, const XRFeaturePlan &) override {
                if (stage == XRSessionPreparation::InputAndUi)
                    return tracking->Bind(candidate, {3, 5, 1}, Value<XRWorldOriginRevision>(1));
                return Result<void>::Success();
            }

            void Release(XRSessionPreparation stage, const XRSessionId &session) noexcept override {
                if (stage == XRSessionPreparation::InputAndUi)
                    tracking->Close(session);
            }
        };

        struct PublicationFixture final {
            Resources resources;
            XRSessionLifecycle lifecycle{resources};
            XRTrackingPublication owner{lifecycle};
            XRSessionId session;
            XRTrackedDeviceCapabilities capabilities;

            PublicationFixture() {
                resources.tracking = &owner;
                XRCapabilityDescriptor descriptor{.system = {Value<XRRuntimeGeneration>(1), {0, 1}},
                                                  .revision = Value<XRCapabilityRevision>(1),
                                                  .limits = {.maximumViews = 2,
                                                             .maximumSpaces = 8,
                                                             .maximumActions = 8,
                                                             .maximumDevices = 4}};
                descriptor.states.fill(XRCapabilityState::Available);
                const auto evidence = XRCapabilitySnapshot::Create(descriptor);
                REQUIRE(evidence.HasValue());
                const auto plan = NegotiateXRFeatures(evidence.Value(), descriptor.system, descriptor.revision,
                                                      {.profile = XRFeatureProfile::Projection1_0,
                                                       .requestedLimits = {.maximumViews = 2,
                                                                           .maximumSpaces = 3,
                                                                           .maximumActions = 8,
                                                                           .maximumDevices = 3}});
                REQUIRE(plan.plan.has_value());
                const auto activated = lifecycle.Activate(evidence.Value(), descriptor.system, descriptor.revision, *plan.plan);
                REQUIRE(activated.HasValue());
                session = activated.Value();
                REQUIRE(lifecycle.ApplyEvent(session, XRSessionEvent::Started).HasValue());
                REQUIRE(lifecycle.ApplyEvent(session, XRSessionEvent::BecameVisible).HasValue());
                REQUIRE(lifecycle.ApplyEvent(session, XRSessionEvent::BecameFocused).HasValue());
                capabilities.values[static_cast<std::size_t>(XRTrackedDeviceCapability::GripPose)] = true;
            }

            ~PublicationFixture() {
                lifecycle.Shutdown();
            }

            TrackingPublicationRequest Request(std::span<const XRTrackedActionRecord> actions = {}) const {
                return {{Value<XRRuntimeTime>(100), Value<XRSimulationTime>(80), {}},
                        Value<XRWorldOriginRevision>(1),
                        XRTrackingFocusState::Focused,
                        XRTrackingVisibilityState::Visible,
                        XRTrackingState::Active,
                        1,
                        {},
                        actions};
            }

            TrackingSourceEvidence Source(const XRTrackedDeviceRole role, const bool active = true) const {
                const bool head = role == XRTrackedDeviceRole::Head;
                XRPoseDescriptor pose{session,
                                      {{session, {head ? 1U : 2U, 1}},
                                       head ? XRSpaceKind::View : XRSpaceKind::Action,
                                       Value<XRWorldOriginRevision>(1)},
                                      {{session, {0, 1}}, XRSpaceKind::Local, Value<XRWorldOriginRevision>(1)},
                                      XRPosePurpose::SimulationInput,
                                      Request().time,
                                      {}};
                if (active) {
                    pose.components.positionMeters = {Math::Vec3{1.0F, 2.0F, 3.0F}, XRPoseComponentValidity::Tracked};
                    pose.components.orientation = {Math::Quaternion::Identity(), XRPoseComponentValidity::Tracked};
                    pose.components.confidence = XRTrackingConfidence::High;
                    pose.components.loss = XRTrackingLossState::None;
                }
                return {role, head ? XRTrackedPoseKind::Head : XRTrackedPoseKind::Grip, active, pose};
            }
        };
    }  // namespace

    TEST_CASE("XRRuntime joins native source observations to the sole device issuer without allocation", "[xr][tracking-owner]") {
        PublicationFixture fixture;
        const std::array sources{fixture.Source(XRTrackedDeviceRole::Head), fixture.Source(XRTrackedDeviceRole::LeftController)};
        const auto before = Tests::AllocationProbe::Count();
        const auto observed = fixture.owner.ObserveSources(sources);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(observed.HasValue());
        CHECK(after == before);
        REQUIRE(fixture.owner.Devices().size() == 2);
        const auto head = fixture.owner.Devices()[0];
        const auto left = fixture.owner.Devices()[1];
        CHECK(head.id.session == fixture.session);
        CHECK(head.role == XRTrackedDeviceRole::Head);
        CHECK(head.capabilities.Has(XRTrackedDeviceCapability::HeadPose));
        CHECK(left.role == XRTrackedDeviceRole::LeftController);
        CHECK(left.capabilities.Has(XRTrackedDeviceCapability::GripPose));
        CHECK_FALSE(left.capabilities.Has(XRTrackedDeviceCapability::AimPose));
        const std::array poses{TrackingPoseEvidence{head.id, sources[0].kind, sources[0].pose},
                               TrackingPoseEvidence{left.id, sources[1].kind, sources[1].pose}};
        const std::array actions{XRTrackedActionRecord{left.id, 0, XRTrackedActionKind::Boolean, true, 1.0F, 0.0F}};
        auto request = fixture.Request(actions);
        request.poses = poses;
        REQUIRE(fixture.owner.Publish(request).HasValue());
        REQUIRE(fixture.owner.Current() != nullptr);
        CHECK(fixture.owner.Current()->Devices()[1].id == left.id);
        CHECK(fixture.owner.Current()->Actions().front().device == left.id);
    }

    TEST_CASE("XRRuntime absent and inactive sources do not fabricate presence or reconnect generations", "[xr][tracking-owner]") {
        PublicationFixture fixture;
        const std::array absent{fixture.Source(XRTrackedDeviceRole::Head, false),
                                fixture.Source(XRTrackedDeviceRole::LeftController, false)};
        REQUIRE(fixture.owner.ObserveSources(absent).HasValue());
        CHECK(fixture.owner.Devices().empty());
        const std::array active{fixture.Source(XRTrackedDeviceRole::LeftController)};
        REQUIRE(fixture.owner.ObserveSources(active).HasValue());
        REQUIRE(fixture.owner.Devices().size() == 1);
        const auto identity = fixture.owner.Devices().front().id;
        REQUIRE(fixture.owner.ObserveSources(absent).HasValue());
        REQUIRE(fixture.owner.Devices().size() == 1);
        CHECK(fixture.owner.Devices().front().id == identity);
        CHECK(fixture.owner.Devices().front().state == XRTrackedDeviceState::TrackingLost);
        CHECK(fixture.owner.Devices().front().capabilities.Has(XRTrackedDeviceCapability::GripPose));
        REQUIRE(fixture.owner.ObserveSources(active).HasValue());
        CHECK(fixture.owner.Devices().front().id == identity);
        CHECK(fixture.owner.Devices().front().state == XRTrackedDeviceState::Tracked);
    }

    TEST_CASE("XRRuntime invalid observation packets revoke publication without partially replacing source generations",
              "[xr][tracking-owner]") {
        PublicationFixture fixture;
        const std::array original{fixture.Source(XRTrackedDeviceRole::LeftController)};
        REQUIRE(fixture.owner.ObserveSources(original).HasValue());
        const auto device = fixture.owner.Devices().front();
        REQUIRE(fixture.owner.Publish(fixture.Request()).HasValue());
        std::array candidate{fixture.Source(XRTrackedDeviceRole::Head), fixture.Source(XRTrackedDeviceRole::LeftController)};
        SECTION("foreign session after valid first source") {
            ++candidate[1].pose.session.slot.generation;
        }
        SECTION("stale origin") {
            candidate[1].pose.source.worldOriginRevision = Value<XRWorldOriginRevision>(2);
        }
        SECTION("duplicate observation") {
            candidate[1] = candidate[0];
        }
        SECTION("wrong role and pose meaning") {
            candidate[1].kind = XRTrackedPoseKind::Head;
        }
        SECTION("inactive source cannot carry valid stale components") {
            candidate[1].active = false;
        }
        SECTION("presentation time cannot enter the Input cutoff") {
            candidate[1].pose.time.simulation.reset();
            candidate[1].pose.time.renderPrediction = Value<XRRenderPredictionTime>(120);
        }
        CHECK(fixture.owner.ObserveSources(candidate).HasError());
        CHECK(fixture.owner.Current() == nullptr);
        REQUIRE(fixture.owner.Devices().size() == 1);
        CHECK(fixture.owner.Devices().front() == device);
    }

    TEST_CASE("XRRuntime source observations reject foreign lanes and lost canonical session admission", "[xr][tracking-owner]") {
        PublicationFixture fixture;
        const std::array source{fixture.Source(XRTrackedDeviceRole::LeftController)};
        REQUIRE(fixture.owner.ObserveSources(source).HasValue());
        const auto device = fixture.owner.Devices().front();
        bool rejected = false;
        std::thread foreign{[&] {
            rejected = fixture.owner.ObserveSources(source).HasError();
        }};
        foreign.join();
        CHECK(rejected);
        CHECK(fixture.owner.Devices().front() == device);
        REQUIRE(fixture.lifecycle.ApplyEvent(fixture.session, XRSessionEvent::SessionLost).HasValue());
        CHECK(fixture.owner.ObserveSources(source).HasError());
        CHECK(fixture.owner.Current() == nullptr);
    }

    TEST_CASE("XRRuntime tracking owner publishes exact source generations atomically without frame allocations", "[xr][tracking-owner]") {
        PublicationFixture fixture;
        const auto device =
            fixture.owner.ObserveDevice(XRTrackedDeviceRole::LeftController, XRTrackedDeviceState::TrackingLost, fixture.capabilities);
        REQUIRE(device.HasValue());
        const std::array actions{XRTrackedActionRecord{device.Value(), 0, XRTrackedActionKind::Boolean, true, 1.0F, 0.0F}};
        const auto request = fixture.Request(actions);
        const auto allocations = Tests::AllocationProbe::Count();
        const auto published = fixture.owner.Publish(request);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(published.HasValue());
        CHECK(after == allocations);
        REQUIRE(fixture.owner.Current() != nullptr);
        CHECK(fixture.owner.Current()->Sequence() == 1);
        CHECK(fixture.owner.Current()->Actions().front().device == device.Value());
        auto stale = request;
        stale.origin = Value<XRWorldOriginRevision>(2);
        CHECK(fixture.owner.Publish(stale).HasError());
        CHECK(fixture.owner.Current() == nullptr);
        REQUIRE(fixture.owner.Publish(request).HasValue());
        CHECK(fixture.owner.Current()->Sequence() == 2);
    }

    TEST_CASE("XRRuntime tracking device reconnect retires old source evidence without replacing the canonical session",
              "[xr][tracking-owner]") {
        PublicationFixture fixture;
        auto device =
            fixture.owner.ObserveDevice(XRTrackedDeviceRole::LeftController, XRTrackedDeviceState::TrackingLost, fixture.capabilities);
        REQUIRE(device.HasValue());
        const auto lost =
            fixture.owner.ObserveDevice(XRTrackedDeviceRole::LeftController, XRTrackedDeviceState::Disconnected, fixture.capabilities);
        REQUIRE(lost.HasValue());
        CHECK(lost.Value() == device.Value());
        const auto connected =
            fixture.owner.ObserveDevice(XRTrackedDeviceRole::LeftController, XRTrackedDeviceState::TrackingLost, fixture.capabilities);
        REQUIRE(connected.HasValue());
        CHECK(connected.Value().slot.generation == device.Value().slot.generation + 1);
        CHECK(connected.Value().session == fixture.session);
        const std::array stale{XRTrackedActionRecord{device.Value(), 0, XRTrackedActionKind::Boolean, true, 1.0F, 0.0F}};
        CHECK(fixture.owner.Publish(fixture.Request(stale)).HasError());
        CHECK(fixture.owner.Current() == nullptr);
    }

    TEST_CASE("XRRuntime tracking publication closes on actual focus loss and rejects wrong-thread mutation", "[xr][tracking-owner]") {
        PublicationFixture fixture;
        REQUIRE(fixture.owner.Publish(fixture.Request()).HasValue());
        REQUIRE(fixture.lifecycle.ApplyEvent(fixture.session, XRSessionEvent::FocusLost).HasValue());
        CHECK(fixture.owner.Current() == nullptr);
        CHECK(fixture.owner.Publish(fixture.Request()).HasError());
        auto neutral = fixture.Request();
        neutral.focus = XRTrackingFocusState::Unfocused;
        REQUIRE(fixture.owner.Publish(neutral).HasValue());
        REQUIRE(fixture.owner.Current() != nullptr);
        bool rejected{}, foreignBorrowEmpty{};
        std::thread foreign{[&] {
            rejected =
                fixture.owner.ObserveDevice(XRTrackedDeviceRole::LeftController, XRTrackedDeviceState::TrackingLost, fixture.capabilities)
                    .HasError();
            foreignBorrowEmpty = fixture.owner.Current() == nullptr;
        }};
        foreign.join();
        CHECK(rejected);
        CHECK(foreignBorrowEmpty);
        auto stale = fixture.session;
        ++stale.slot.generation;
        fixture.owner.Close(stale);
        CHECK(fixture.owner.Current() != nullptr);
        fixture.lifecycle.Shutdown();
        CHECK(fixture.owner.Current() == nullptr);
        CHECK(fixture.owner.Devices().empty());
    }
}  // namespace Horo::XR::RuntimeInternal
