#include "OpenXRSessionEvents.h"
#include "support/OpenXRNativeTestSupport.h"

#include <algorithm>
#include <bit>
#include <span>
#include <type_traits>

namespace Horo::Tests::OpenXR {
    namespace {
        struct EventScript final {
            static inline thread_local EventScript *active{};
            std::array<XrSessionState, 16> states{};
            std::size_t count{};
            std::size_t cursor{};
            XrSession session{};
            unsigned begins{};
            unsigned exits{};
            unsigned ends{};
            bool running{};
            OpenXRSessionEvents *reenter{};
            bool reentryRejected{};
            bool loseEvents{};

            template <typename Event> static void CopyEvent(XrEventDataBuffer &buffer, const Event &event) {
                static_assert(std::is_trivially_copyable_v<Event> && sizeof(Event) <= sizeof(XrEventDataBuffer));
                const auto source = std::as_bytes(std::span{&event, 1});
                const auto destination = std::as_writable_bytes(std::span{&buffer, 1});
                std::ranges::copy(source, destination.begin());
            }

            void Push(XrSessionState state) {
                REQUIRE(count < states.size());
                states[count++] = state;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Poll(XrInstance, XrEventDataBuffer *buffer) {
                if (active->loseEvents) {
                    active->loseEvents = false;
                    const XrEventDataEventsLost event{XR_TYPE_EVENT_DATA_EVENTS_LOST, nullptr, 1};
                    CopyEvent(*buffer, event);
                    return XR_SUCCESS;
                }
                if (active->reenter) {
                    auto *owner = active->reenter;
                    active->reenter = nullptr;
                    active->reentryRejected = owner->Poll(true).HasError();
                }
                if (active->cursor == active->count)
                    return XR_EVENT_UNAVAILABLE;
                const XrEventDataSessionStateChanged event{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED, nullptr, active->session,
                                                           active->states[active->cursor++], 1};
                CopyEvent(*buffer, event);
                return XR_SUCCESS;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Begin(XrSession session, const XrSessionBeginInfo *info) {
                REQUIRE(session == active->session);
                REQUIRE(info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
                REQUIRE_FALSE(active->running);
                active->running = true;
                ++active->begins;
                return XR_SUCCESS;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Exit(XrSession session) {
                REQUIRE(session == active->session);
                REQUIRE(active->running);
                ++active->exits;
                return XR_SUCCESS;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL End(XrSession session) {
                REQUIRE(session == active->session);
                REQUIRE(active->running);
                active->running = false;
                ++active->ends;
                return XR_SUCCESS;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL GetProc(XrInstance, const char *name, PFN_xrVoidFunction *output) {
                *output = nullptr;
                const std::string_view symbol{name};
                if (symbol == "xrPollEvent")
                    *output = std::bit_cast<PFN_xrVoidFunction>(&Poll);
                if (symbol == "xrBeginSession")
                    *output = std::bit_cast<PFN_xrVoidFunction>(&Begin);
                if (symbol == "xrRequestExitSession")
                    *output = std::bit_cast<PFN_xrVoidFunction>(&Exit);
                if (symbol == "xrEndSession")
                    *output = std::bit_cast<PFN_xrVoidFunction>(&End);
                return *output ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
            }
        };

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
                REQUIRE(events.Bind(&EventScript::GetProc, {}).HasValue());
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
            REQUIRE(f.events.Poll(true).HasError());
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
    }  // namespace
}  // namespace Horo::Tests::OpenXR
