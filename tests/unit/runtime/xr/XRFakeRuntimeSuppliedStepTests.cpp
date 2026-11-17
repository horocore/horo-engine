#include "support/AllocationProbe.h"
#include "support/xr/SimulatorContractTestSupport.h"

#include <limits>

namespace Horo::XR {
    namespace {
        using namespace SimulatorTests;

        XRSessionId Activate(XRFakeRuntime &runtime) {
            const auto capabilities = Capabilities();
            const auto active = runtime.Activate(capabilities, Plan(capabilities, false), {2, 1, 1});
            REQUIRE(active.HasValue());
            return active.Value();
        }
    }  // namespace

    TEST_CASE("Supplied fake steps share scripted frame failure and cleanup semantics", "[xr][fake][interactive]") {
        for (const auto failure : {XRFakeFailure::Wait, XRFakeFailure::Begin, XRFakeFailure::Locate, XRFakeFailure::Acquire,
                                   XRFakeFailure::Submit, XRFakeFailure::Release, XRFakeFailure::End}) {
            XRFakeRuntime scripted;
            XRFakeRuntime supplied;
            const auto first = Activate(scripted);
            const auto second = Activate(supplied);
            auto failed = Frame(first);
            failed.failure = failure;
            const std::array script{Event(XRSessionEvent::Started), failed, Frame(first, 200)};
            REQUIRE(scripted.LoadScript(first, script) == XRFrameStatus::Ok);
            REQUIRE(scripted.Step(first).status == XRFrameStatus::Ok);
            REQUIRE(supplied.Step(second, Event(XRSessionEvent::Started)).status == XRFrameStatus::Ok);
            auto interactiveFailure = Frame(second);
            interactiveFailure.failure = failure;
            const auto original = scripted.Step(first);
            const auto before = Tests::AllocationProbe::Count();
            const auto interactive = supplied.Step(second, interactiveFailure);
            const auto after = Tests::AllocationProbe::Count();
            CHECK(before == after);
            CHECK(interactive.consumed == original.consumed);
            CHECK(interactive.status == original.status);
            CHECK(interactive.failure == original.failure);
            CHECK(interactive.frame.phase == original.frame.phase);
            CHECK(interactive.frame.acquiredImages == original.frame.acquiredImages);
            CHECK(interactive.frame.releasedImages == original.frame.releasedImages);
            CHECK(scripted.Step(first).status == XRFrameStatus::Ok);
            CHECK(supplied.Step(second, Frame(second, 200)).status == XRFrameStatus::Ok);
        }
    }

    TEST_CASE("Supplied fake steps reject malformed view action time and stale owner before consumption", "[xr][fake][interactive]") {
        XRFakeRuntime runtime;
        const auto session = Activate(runtime);
        REQUIRE(runtime.Step(session, Event(XRSessionEvent::Started)).status == XRFrameStatus::Ok);
        auto invalid = Frame(session);
        invalid.viewCount = 3;
        const auto unsupported = runtime.Step(session, invalid);
        CHECK(unsupported.status == XRFrameStatus::CapacityExceeded);
        CHECK_FALSE(unsupported.consumed);
        CHECK(unsupported.frame.acquiredImages == 0);
        invalid = Frame(session);
        invalid.actionCount = XRFakeLimits::MaximumActions + 1;
        CHECK(runtime.Step(session, invalid).status == XRFrameStatus::CapacityExceeded);
        invalid = Frame(session);
        invalid.actionCount = 1;
        invalid.actions[0] = {{session, {1, 1}}, XRFakeActionKind::Float, {std::numeric_limits<float>::infinity(), 0}, true};
        CHECK(runtime.Step(session, invalid).status == XRFrameStatus::InvalidInput);
        REQUIRE(runtime.Step(session, Frame(session)).status == XRFrameStatus::Ok);
        const auto repeat = runtime.Step(session, Frame(session));
        CHECK(repeat.status == XRFrameStatus::InvalidInput);
        CHECK_FALSE(repeat.consumed);
        const auto replacement = Activate(runtime);
        CHECK(runtime.Step(session, Frame(session, 200)).status == XRFrameStatus::StaleSession);
        REQUIRE(runtime.Step(replacement, Event(XRSessionEvent::Started)).status == XRFrameStatus::Ok);
        REQUIRE(runtime.Step(replacement, Frame(replacement, 200)).status == XRFrameStatus::Ok);
        runtime.Shutdown();
        CHECK(runtime.Step(replacement, Frame(replacement, 300)).status == XRFrameStatus::Shutdown);
    }

    TEST_CASE("Fake script admission cannot rewind prediction after supplied or scripted consumption", "[xr][fake][interactive]") {
        XRFakeRuntime runtime;
        const auto session = Activate(runtime);
        REQUIRE(runtime.Step(session, Event(XRSessionEvent::Started)).status == XRFrameStatus::Ok);
        const auto first = runtime.Step(session, Frame(session, 200));
        REQUIRE(first.status == XRFrameStatus::Ok);
        auto backward = Frame(session, 100);
        backward.event = XRSessionEvent::BecameVisible;
        const std::array rejected{backward};
        CHECK(runtime.LoadScript(session, rejected) == XRFrameStatus::InvalidInput);
        CHECK(runtime.Snapshot().session == session);
        CHECK(runtime.Snapshot().state == XRSessionState::Running);
        const auto cursor = runtime.Step(session);
        CHECK_FALSE(cursor.consumed);
        CHECK(cursor.nextStep == first.nextStep);
        const auto forward = runtime.Step(session, Frame(session, 250));
        REQUIRE(forward.status == XRFrameStatus::Ok);
        CHECK(forward.frame.frame.sequence == first.frame.frame.sequence + 1);
        auto future = Frame(session, 300);
        future.event = XRSessionEvent::BecameVisible;
        const std::array accepted{future};
        REQUIRE(runtime.LoadScript(session, accepted) == XRFrameStatus::Ok);
        const auto scripted = runtime.Step(session);
        REQUIRE(scripted.status == XRFrameStatus::Ok);
        CHECK(scripted.frame.frame.sequence == forward.frame.frame.sequence + 1);
        CHECK(scripted.session.state == XRSessionState::Visible);
        CHECK(runtime.LoadScript(session, rejected) == XRFrameStatus::InvalidInput);
        CHECK(runtime.Snapshot().state == XRSessionState::Visible);
    }

    TEST_CASE("Loaded fake scripts and interactive steps cannot silently share a cursor", "[xr][fake][interactive]") {
        XRFakeRuntime runtime;
        const auto session = Activate(runtime);
        const std::array script{Event(XRSessionEvent::Started), Frame(session)};
        REQUIRE(runtime.LoadScript(session, script) == XRFrameStatus::Ok);
        const auto rejected = runtime.Step(session, Event(XRSessionEvent::Started));
        CHECK(rejected.status == XRFrameStatus::OutOfOrder);
        CHECK_FALSE(rejected.consumed);
        CHECK(rejected.nextStep == 0);
        REQUIRE(runtime.Step(session).status == XRFrameStatus::Ok);
        REQUIRE(runtime.Step(session).status == XRFrameStatus::Ok);
    }
}  // namespace Horo::XR
