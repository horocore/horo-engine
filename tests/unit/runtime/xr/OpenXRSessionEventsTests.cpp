#include "OpenXRSessionEvents.h"
#include "support/OpenXREventScriptTestSupport.h"
#include "support/OpenXRNativeTestSupport.h"

#include <algorithm>
#include <bit>
#include <span>
#include <type_traits>

namespace Horo::Tests::OpenXR {
    namespace {
        class EventResources final : public IXRSessionResources {
        public:
            explicit EventResources(Fixture &native) : native_(native) {}

            Result<void> Prepare(XRSessionPreparation stage, const XRSessionId &candidate, const XRFeaturePlan &) override {
                if (stage != XRSessionPreparation::NativeSessionAndSwapchains)
                    return Result<void>::Success();
                auto request = native_.Request();
                request.candidate = candidate;
                return native_.owner.Create(request);
            }

            void Release(XRSessionPreparation stage, const XRSessionId &) noexcept override {
                if (stage == XRSessionPreparation::NativeSessionAndSwapchains)
                    CHECK(native_.owner.Close().HasValue());
            }

        private:
            Fixture &native_;
        };

        struct EventFixture final {
            Fixture native;
            EventResources resources{native};
            XRSessionLifecycle sessions{resources};
            XRFrameLifecycle frames{sessions};
            EventScript script;
            OpenXRSessionEvents events{native.owner, sessions, frames};
            XRSessionId session;

            EventFixture() {
                REQUIRE(EventScript::active == nullptr);
                EventScript::active = &script;
                auto created =
                    sessions.Activate(native.capabilities, native.capabilities.System(), native.capabilities.Revision(), native.plan);
                REQUIRE(created.HasValue());
                session = created.Value();
                auto borrowed = native.owner.Borrow(session);
                REQUIRE(borrowed.HasValue());
                script.session = borrowed.Value().session;
                REQUIRE(events.Bind({&EventScript::GetProc}, {}).HasValue());
                REQUIRE(frames.BindConfiguration({session, {3, 1}}, native.capabilities, {2, 2, 1}) == XRFrameStatus::Ok);
            }

            ~EventFixture() {
                EventScript::active = nullptr;
            }
        };

        TEST_CASE("OpenXR native event pump begins and ends two epochs on the same session", "[xr][native][events]") {
            EventFixture f;
            for (unsigned cycle = 0; cycle < 2; ++cycle) {
                f.script.Push(XR_SESSION_STATE_READY);
                REQUIRE(f.events.Poll(true).HasValue());
                REQUIRE(f.events.Running());
                REQUIRE(f.script.begins == cycle + 1);
                REQUIRE(f.sessions.Snapshot().session == f.session);
                f.script.Push(XR_SESSION_STATE_VISIBLE);
                f.script.Push(XR_SESSION_STATE_FOCUSED);
                REQUIRE(f.events.Poll(true).HasValue());
                REQUIRE(f.sessions.Admit(f.session, true).HasValue());
                REQUIRE(f.events.Poll(false).HasValue());
                REQUIRE(f.script.exits == cycle + 1);
                REQUIRE(f.events.Poll(false).HasValue());
                REQUIRE(f.script.exits == cycle + 1);
                f.script.Push(XR_SESSION_STATE_STOPPING);
                REQUIRE(f.events.Poll(false).HasValue());
                REQUIRE_FALSE(f.events.Running());
                REQUIRE(f.script.ends == cycle + 1);
                REQUIRE(f.sessions.Snapshot().state == XRSessionState::Inactive);
                REQUIRE_FALSE(f.sessions.AcceptedPlan());
                REQUIRE(f.sessions.Admit(f.session, false).HasError());
            }
            REQUIRE(f.native.script.liveSessions == 1);
            REQUIRE(std::ranges::count(f.native.script.calls, "session") == 1);
        }

        TEST_CASE("OpenXR native end waits for the canonical frame obligation and rejects reentry", "[xr][native][events]") {
            EventFixture f;
            f.script.Push(XR_SESSION_STATE_READY);
            REQUIRE(f.events.Poll(true).HasValue());
            REQUIRE(f.frames.ReserveWait(f.session).status == XRFrameStatus::Ok);
            f.script.Push(XR_SESSION_STATE_STOPPING);
            f.script.reenter = &f.events;
            REQUIRE(f.events.Poll(false).HasValue());
            REQUIRE(f.script.reentryRejected);
            REQUIRE(f.script.ends == 0);
            REQUIRE(f.events.Running());
            REQUIRE_FALSE(f.sessions.Snapshot().admitsFrames);
            // This fixture's wait never entered native dispatch: cancellation retires that exact reservation.
            REQUIRE(f.frames.CancelWait() == XRFrameStatus::Ok);
            REQUIRE(f.events.Poll(false).HasValue());
            REQUIRE(f.script.ends == 1);
            REQUIRE_FALSE(f.events.Running());
        }

        TEST_CASE("OpenXR lost events revoke admission without retiring native frame debt", "[xr][native][events]") {
            EventFixture f;
            f.script.Push(XR_SESSION_STATE_READY);
            REQUIRE(f.events.Poll(true).HasValue());
            REQUIRE(f.frames.ReserveWait(f.session).status == XRFrameStatus::Ok);
            f.script.loseEvents = true;
            const auto observed = f.events.Poll(true);
            REQUIRE(observed.HasValue());
            CHECK(observed.Value().loss == NativeSessionEventLoss::EventHistory);
            REQUIRE(f.sessions.Snapshot().state == XRSessionState::Lost);
            REQUIRE_FALSE(f.sessions.Snapshot().admitsFrames);
            REQUIRE_FALSE(f.sessions.Snapshot().admitsActions);
            REQUIRE_FALSE(f.sessions.AcceptedPlan());
            REQUIRE(f.sessions.ApplyEvent(f.session, XRSessionEvent::Ready).HasError());
            REQUIRE(f.events.Running());
            REQUIRE(f.script.ends == 0);
            REQUIRE(f.native.script.liveSessions == 1);
            REQUIRE(f.frames.Snapshot().phase != XRFramePhase::Idle);
            REQUIRE(f.frames.CancelWait() == XRFrameStatus::Ok);
        }

        TEST_CASE("OpenXR focus and visibility remain independent of running frame admission", "[xr][native][events]") {
            EventFixture f;
            f.script.Push(XR_SESSION_STATE_READY);
            f.script.Push(XR_SESSION_STATE_VISIBLE);
            f.script.Push(XR_SESSION_STATE_FOCUSED);
            REQUIRE(f.events.Poll(true).HasValue());
            REQUIRE(f.sessions.Admit(f.session, true).HasValue());

            f.script.Push(XR_SESSION_STATE_VISIBLE);
            REQUIRE(f.events.Poll(true).HasValue());
            CHECK(f.sessions.Snapshot().state == XRSessionState::Visible);
            CHECK(f.sessions.Admit(f.session, true).HasError());
            CHECK(f.sessions.Admit(f.session, false).HasValue());

            f.script.Push(XR_SESSION_STATE_SYNCHRONIZED);
            REQUIRE(f.events.Poll(true).HasValue());
            CHECK(f.sessions.Snapshot().state == XRSessionState::Running);
            CHECK(f.sessions.Admit(f.session, true).HasError());
            CHECK(f.sessions.Admit(f.session, false).HasValue());
            CHECK(f.script.begins == 1);
            CHECK(f.script.ends == 0);
        }

        TEST_CASE("OpenXR event polls bound backlog work and resume from the exact unread event", "[xr][native][events]") {
            EventFixture f;
            f.script.Push(XR_SESSION_STATE_READY);
            f.script.Push(XR_SESSION_STATE_VISIBLE);
            f.script.Push(XR_SESSION_STATE_FOCUSED);
            REQUIRE(f.events.Poll(true).HasValue());
            const auto polled = f.script.polls;
            for (std::uint32_t index = 0; index < OpenXRSessionEvents::MaximumEventsPerPoll; ++index)
                f.script.Push(XR_SESSION_STATE_FOCUSED);
            f.script.Push(XR_SESSION_STATE_VISIBLE);
            const auto first = f.events.Poll(true);
            REQUIRE(first.HasValue());
            CHECK(first.Value().morePending);
            CHECK(f.script.polls - polled == OpenXRSessionEvents::MaximumEventsPerPoll);
            CHECK(f.sessions.Snapshot().admitsActions);
            const auto second = f.events.Poll(true);
            REQUIRE(second.HasValue());
            CHECK_FALSE(second.Value().morePending);
            CHECK_FALSE(f.sessions.Snapshot().admitsActions);
        }

        TEST_CASE("OpenXR instance loss stops event dispatch and preserves debt even after a stopping event", "[xr][native][events]") {
            EventFixture f;
            f.script.Push(XR_SESSION_STATE_READY);
            REQUIRE(f.events.Poll(true).HasValue());
            REQUIRE(f.frames.ReserveWait(f.session).status == XRFrameStatus::Ok);
            f.script.Push(XR_SESSION_STATE_STOPPING);
            REQUIRE(f.events.Poll(false).HasValue());
            REQUIRE(f.script.ends == 0);
            REQUIRE(f.frames.CancelWait() == XRFrameStatus::Ok);

            f.script.loseInstance = true;
            f.script.Push(XR_SESSION_STATE_READY);
            const auto observed = f.events.Poll(false);
            REQUIRE(observed.HasValue());
            CHECK(observed.Value().loss == NativeSessionEventLoss::Instance);
            CHECK(f.sessions.Snapshot().state == XRSessionState::Lost);
            CHECK(f.events.Running());
            CHECK(f.script.ends == 0);
            CHECK(f.script.cursor < f.script.count);
            const auto polled = f.script.polls;
            const auto retained = f.events.Poll(false);
            REQUIRE(retained.HasValue());
            CHECK(retained.Value().loss == NativeSessionEventLoss::Instance);
            CHECK(f.script.polls == polled);
        }

        TEST_CASE("OpenXR native loss results become terminal observations without reporting native progress", "[xr][native][events]") {
            EventFixture f;
            f.script.Push(XR_SESSION_STATE_READY);
            bool wantsPlay = true;
            auto reason = NativeSessionEventLoss::Session;
            SECTION("poll reports instance loss") {
                f.script.pollResult = XR_ERROR_INSTANCE_LOST;
                reason = NativeSessionEventLoss::Instance;
            }
            SECTION("begin reports a pending session loss") {
                f.script.beginResult = XR_SESSION_LOSS_PENDING;
            }
            SECTION("request exit reports session loss") {
                REQUIRE(f.events.Poll(true).HasValue());
                f.script.exitResult = XR_ERROR_SESSION_LOST;
                wantsPlay = false;
            }
            SECTION("end reports a pending session loss") {
                REQUIRE(f.events.Poll(true).HasValue());
                f.script.Push(XR_SESSION_STATE_STOPPING);
                f.script.endResult = XR_SESSION_LOSS_PENDING;
            }
            const auto observed = f.events.Poll(wantsPlay);
            REQUIRE(observed.HasValue());
            CHECK(observed.Value().loss == reason);
            CHECK(f.sessions.Snapshot().state == XRSessionState::Lost);
            CHECK_FALSE(f.sessions.Snapshot().admitsFrames);
            CHECK_FALSE(f.sessions.Snapshot().admitsActions);
            CHECK(f.events.Running() == f.script.running);
            CHECK(f.native.script.liveSessions == 1);
        }
    }  // namespace
}  // namespace Horo::Tests::OpenXR
