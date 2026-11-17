#include "XRFakeRuntimeHostComposition.h"
#include "support/AllocationProbe.h"
#include "support/xr/SimulatorContractTestSupport.h"

#include <functional>
#include <stdexcept>
#include <thread>

namespace Horo::XR::RuntimeInternal {
    namespace {
        using namespace SimulatorTests;

        struct Children final : IXRSessionResources {
            std::optional<XRSessionPreparation> fail;
            bool throws{};
            bool nonstandard{};
            std::array<std::array<std::size_t, static_cast<std::size_t>(XRSessionPreparation::Count)>, 16> released{};
            std::function<void(const XRSessionId &)> callback;

            Result<void> Prepare(const XRSessionPreparation stage, const XRSessionId &session, const XRFeaturePlan &) override {
                if (callback)
                    callback(session);
                if (fail != stage)
                    return Result<void>::Success();
                if (throws) {
                    if (nonstandard)
                        throw 17;
                    throw std::runtime_error{"PRIVATE_SIMULATOR_PROVIDER_SENTINEL"};
                }
                return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
            }

            void Release(const XRSessionPreparation stage, const XRSessionId &session) noexcept override {
                ++released[session.slot.generation][static_cast<std::size_t>(stage)];
            }
        };

        XRSessionId Activate(XRFakeRuntimeHostComposition &host, const bool hands = true,
                             const std::optional<XRSessionPreparation> fail = {}) {
            const auto caps = Capabilities();
            auto active = host.Activate(caps, Plan(caps, hands), {2, 1, 1}, {5, 7, 0, hands ? XRHandTrackingHardLimits::MaximumJoints : 0},
                                        Origin(), fail);
            if (active.HasError())
                UNSCOPED_INFO(active.ErrorValue().domain.Value()
                              << ':' << active.ErrorValue().code.Value() << ':' << active.ErrorValue().message);
            REQUIRE(active.HasValue());
            return active.Value();
        }

        void Focus(XRFakeRuntimeHostComposition &host, const XRSessionId &session) {
            for (const auto event : {XRSessionEvent::Started, XRSessionEvent::BecameVisible, XRSessionEvent::BecameFocused}) {
                const auto stepped = host.Step(session, Event(event));
                REQUIRE(stepped.HasValue());
                REQUIRE(stepped.Value().status == XRFrameStatus::Ok);
            }
        }

        void PublishHand(XRFakeRuntimeHostComposition &host, const XRSessionId &session, const std::int64_t time = 100) {
            const auto frame = host.Step(session, Frame(session, time));
            REQUIRE(frame.HasValue());
            REQUIRE(frame.Value().status == XRFrameStatus::Ok);
            const std::array sources{
                TrackingSourceEvidence{XRTrackedDeviceRole::LeftHand, XRTrackedPoseKind::Palm, true, Pose(session, time)}};
            REQUIRE(host.ObserveSources(session, sources).HasValue());
            REQUIRE(host.Devices().size() == 1);
            const auto hand = host.Devices()[0].id;
            const std::array poses{TrackingPoseEvidence{hand, XRTrackedPoseKind::Palm, sources[0].pose}};
            const std::array joints{Joint(session, hand, XRHandedness::Left, XRHandJoint::Palm, time)};
            const auto &pose = sources[0].pose;
            const TrackingPublicationRequest request{pose.time,
                                                     Origin(),
                                                     XRTrackingFocusState::Focused,
                                                     XRTrackingVisibilityState::Visible,
                                                     XRTrackingState::Active,
                                                     0,
                                                     poses,
                                                     {},
                                                     joints};
            const auto before = Tests::AllocationProbe::Count();
            const auto published = host.Publish(session, request);
            const auto after = Tests::AllocationProbe::Count();
            REQUIRE(published.HasValue());
            CHECK(before == after);
        }
    }  // namespace

    TEST_CASE("Fake tracking composition publishes real candidate hand evidence through the same session gates", "[xr][fake][hands]") {
        Children children;
        XRFakeRuntimeHostComposition host{&children};
        const auto session = Activate(host);
        CHECK_FALSE(host.Current());
        Focus(host, session);
        PublishHand(host, session);
        const auto *snapshot = host.Current();
        REQUIRE(snapshot);
        REQUIRE(snapshot->HandJoints().size() == 1);
        CHECK(snapshot->Session() == host.Snapshot().session);
        CHECK(snapshot->HandJoints()[0].device.session == session);
        CHECK(snapshot->Devices()[0].role == XRTrackedDeviceRole::LeftHand);
        CHECK(snapshot->Devices()[0].capabilities.Has(XRTrackedDeviceCapability::HandJoints));
        host.Shutdown();
        host.Shutdown();
        CHECK_FALSE(host.Current());
        for (const auto count : children.released[session.slot.generation])
            CHECK(count == 1);
    }

    TEST_CASE("Fake tracking candidate failure and exception rollback preserve prior active publication", "[xr][fake][rollback]") {
        for (const int failureMode : {0, 1, 2}) {
            Children children;
            children.throws = failureMode != 0;
            children.nonstandard = failureMode == 2;
            XRFakeRuntimeHostComposition host{&children};
            const auto session = Activate(host);
            Focus(host, session);
            PublishHand(host, session);
            const auto *prior = host.Current();
            const auto caps = Capabilities();
            children.fail = XRSessionPreparation::InputAndUi;
            const auto failed = host.Activate(caps, Plan(caps), {2, 1, 1}, {5, 7, 0, 52}, Origin());
            REQUIRE(failed.HasError());
            CHECK(failed.ErrorValue().message.find("PRIVATE_SIMULATOR_PROVIDER_SENTINEL") == std::string::npos);
            CHECK(host.Snapshot().session == session);
            CHECK(host.Current() == prior);
            CHECK(host.Current()->HandJoints().size() == 1);
            for (const auto count : children.released[session.slot.generation])
                CHECK(count == 0);
            children.fail.reset();
            const auto replacement = Activate(host);
            CHECK(replacement.slot.generation > session.slot.generation);
            for (const auto count : children.released[session.slot.generation])
                CHECK(count == 1);
            Focus(host, replacement);
            PublishHand(host, replacement, 200);
            const auto *fresh = host.Current();
            CHECK(host.ObserveSources(session, {}).HasError());
            CHECK(host.Current() == fresh);
        }
    }

    TEST_CASE("Fake tracking activation rejects every failed stage without unhooking current owner", "[xr][fake][rollback]") {
        XRFakeRuntimeHostComposition host;
        const auto session = Activate(host);
        Focus(host, session);
        PublishHand(host, session);
        const auto *prior = host.Current();
        const auto caps = Capabilities();
        for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(XRSessionPreparation::Count); ++index) {
            REQUIRE(
                host.Activate(caps, Plan(caps), {2, 1, 1}, {5, 7, 0, 52}, Origin(), static_cast<XRSessionPreparation>(index)).HasError());
            CHECK(host.Snapshot().session == session);
            CHECK(host.Current() == prior);
        }
    }

    TEST_CASE("Fake tracking composition rejects unplanned hands and native defaults stay empty", "[xr][fake][hands]") {
        XRFakeRuntimeHostComposition host;
        const auto caps = Capabilities();
        CHECK(host.Activate(caps, Plan(caps, false), {2, 1, 1}, {5, 7, 0, 52}, Origin()).HasError());
        const auto session = Activate(host, false);
        Focus(host, session);
        REQUIRE(host.Step(session, Frame(session)).Value().status == XRFrameStatus::Ok);
        const std::array hands{TrackingSourceEvidence{XRTrackedDeviceRole::LeftHand, XRTrackedPoseKind::Palm, true, Pose(session)}};
        CHECK(ErrorIs(host.ObserveSources(session, hands), XRErrors::OperationUnsupported));
        CHECK(host.Devices().empty());
        const TrackingPublicationRequest request{Pose(session).time,
                                                 Origin(),
                                                 XRTrackingFocusState::Focused,
                                                 XRTrackingVisibilityState::Visible,
                                                 XRTrackingState::Active,
                                                 0,
                                                 {},
                                                 {}};
        REQUIRE(host.Publish(session, request).HasValue());
        CHECK(host.Current()->HandJoints().empty());
    }

    TEST_CASE("Fake tracking focus loss cutoff errors and session loss revoke stale hand evidence", "[xr][fake][hands]") {
        XRFakeRuntimeHostComposition host;
        const auto session = Activate(host);
        Focus(host, session);
        PublishHand(host, session);
        REQUIRE(host.Step(session, Event(XRSessionEvent::FocusLost)).Value().status == XRFrameStatus::Ok);
        CHECK_FALSE(host.Current());
        REQUIRE(host.Step(session, Frame(session, 200)).Value().status == XRFrameStatus::Ok);
        const std::array deniedSources{
            TrackingSourceEvidence{XRTrackedDeviceRole::LeftHand, XRTrackedPoseKind::Palm, false, Pose(session, 200, false)}};
        const auto denied = host.ObserveSources(session, deniedSources);
        if (denied.HasError())
            UNSCOPED_INFO(denied.ErrorValue().domain.Value()
                          << ':' << denied.ErrorValue().code.Value() << ':' << denied.ErrorValue().message);
        REQUIRE(ErrorIs(denied, XRErrors::OperationUnavailable));
        CHECK_FALSE(host.Current());
        CHECK(host.Snapshot().state == XRSessionState::Visible);
        REQUIRE(host.Step(session, Event(XRSessionEvent::BecameFocused)).Value().status == XRFrameStatus::Ok);
        REQUIRE(host.Step(session, Frame(session, 300)).Value().status == XRFrameStatus::Ok);
        auto request = TrackingPublicationRequest{Pose(session, 300).time,
                                                  Origin(),
                                                  XRTrackingFocusState::Focused,
                                                  XRTrackingVisibilityState::Visible,
                                                  XRTrackingState::TrackingLost,
                                                  0,
                                                  {},
                                                  {}};
        const std::array sources{
            TrackingSourceEvidence{XRTrackedDeviceRole::LeftHand, XRTrackedPoseKind::Palm, false, Pose(session, 300, false)}};
        const auto observed = host.ObserveSources(session, sources);
        if (observed.HasError())
            UNSCOPED_INFO(observed.ErrorValue().domain.Value()
                          << ':' << observed.ErrorValue().code.Value() << ':' << observed.ErrorValue().message);
        REQUIRE(observed.HasValue());
        REQUIRE(host.Publish(session, request).HasValue());
        REQUIRE(host.Current());
        CHECK(host.Current()->HandJoints().empty());
        request.time = Pose(session, 301).time;
        CHECK(ErrorIs(host.Publish(session, request), XRErrors::TimeDomainIncompatible));
        CHECK_FALSE(host.Current());
        REQUIRE(host.Step(session, Event(XRSessionEvent::SessionLost)).Value().status == XRFrameStatus::Ok);
        CHECK_FALSE(host.Current());
        CHECK(host.Publish(session, request).HasError());
    }

    TEST_CASE("Fake composition rejects callback reentrancy and foreign lane control without mutation", "[xr][fake][lifecycle]") {
        Children children;
        XRFakeRuntimeHostComposition host{&children};
        std::size_t rejections{};
        children.callback = [&](const XRSessionId &candidate) {
            rejections += host.Step(candidate, Event(XRSessionEvent::Started)).HasError();
        };
        const auto session = Activate(host);
        CHECK(rejections == static_cast<std::size_t>(XRSessionPreparation::Count));
        bool rejected{};
        std::thread foreign{[&] {
            rejected = host.Step(session, Event(XRSessionEvent::Started)).HasError();
        }};
        foreign.join();
        CHECK(rejected);
        CHECK(host.Snapshot().state == XRSessionState::Ready);
        children.callback = {};
        Focus(host, session);
        const auto prepared = Frame(session);
        const auto before = Tests::AllocationProbe::Count();
        const auto result = host.Step(session, prepared);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(result.HasValue());
        CHECK(result.Value().status == XRFrameStatus::Ok);
        CHECK(before == after);
    }
}  // namespace Horo::XR::RuntimeInternal
