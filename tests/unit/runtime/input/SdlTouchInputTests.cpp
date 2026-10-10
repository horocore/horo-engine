#include "SdlInputBackend.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

namespace {
    using namespace Horo::Input;

    void BindSurface(SdlInputBackend &backend) {
        backend.BindWindow(7);
        backend.BeginFrame(1);
        SDL_Event resize{};
        resize.type = SDL_EVENT_WINDOW_RESIZED;
        resize.window.windowID = 7;
        resize.window.data1 = 200;
        resize.window.data2 = 100;
        backend.ProcessEvent(resize);
        (void)backend.Commit();
        backend.BeginFrame(2);
    }

    SDL_Event Finger(const SDL_EventType type, const SDL_FingerID id, const std::uint64_t time, const float x = 0.25F) {
        SDL_Event event{};
        event.type = type;
        event.tfinger.windowID = 7;
        event.tfinger.touchID = 11;
        event.tfinger.fingerID = id;
        event.tfinger.timestamp = time;
        event.tfinger.x = x;
        event.tfinger.y = 0.5F;
        return event;
    }

    TEST_CASE("SDL fingers use window coordinates and burn fresh contact incarnations", "[unit][input][sdl][touch]") {
        SdlInputBackend backend;
        BindSurface(backend);
        backend.ProcessEvent(Finger(SDL_EVENT_FINGER_DOWN, 1, 100));
        backend.ProcessEvent(Finger(SDL_EVENT_FINGER_DOWN, 2, 101, 0.75F));
        backend.ProcessEvent(Finger(SDL_EVENT_FINGER_UP, 1, 102));
        auto foreign = Finger(SDL_EVENT_FINGER_DOWN, 3, 103);
        foreign.tfinger.windowID = 8;
        backend.ProcessEvent(foreign);
        const auto &first = backend.Commit();
        CHECK(first.touches[0].x == 50);
        CHECK(first.touches[0].y == 50);
        CHECK(first.touches[0].contact.pressed);
        CHECK(first.touches[0].contact.released);
        CHECK(first.touches[1].contact.down);
        CHECK_FALSE(first.touches[2].id.IsValid());
        const auto old = first.touches[0].id;
        backend.BeginFrame(3);
        backend.ProcessEvent(Finger(SDL_EVENT_FINGER_DOWN, 1, 104));
        const auto &next = backend.Commit();
        CHECK(next.touches[0].id != old);
        CHECK(first.touches[0].id == old);
        CHECK_FALSE(next.touches[1].contact.pressed);
    }

    TEST_CASE("SDL cancelled or out-of-order touches cannot revive an active gesture", "[unit][input][sdl][touch]") {
        SdlInputBackend backend;
        BindSurface(backend);
        backend.ProcessEvent(Finger(SDL_EVENT_FINGER_DOWN, 1, 100));
        backend.ProcessEvent(Finger(SDL_EVENT_FINGER_CANCELED, 1, 101));
        backend.ProcessEvent(Finger(SDL_EVENT_FINGER_MOTION, 1, 102, 0.9F));
        const auto &cancelled = backend.Commit();
        CHECK(cancelled.touches[0].cancelled);
        CHECK_FALSE(cancelled.touches[0].contact.down);
        CHECK(cancelled.touches[0].x == 50);
        backend.BeginFrame(3);
        backend.ProcessEvent(Finger(SDL_EVENT_FINGER_DOWN, 1, 200));
        backend.ProcessEvent(Finger(SDL_EVENT_FINGER_MOTION, 1, 199));
        backend.ProcessEvent(Finger(SDL_EVENT_FINGER_DOWN, 2, 201));
        const auto &stale = backend.Commit();
        CHECK(stale.touches[0].cancelled);
        CHECK_FALSE(stale.touches[1].id.IsValid());
    }

    TEST_CASE("SDL malformed and capacity-overflow batches cancel instead of dropping contacts", "[unit][input][sdl][touch]") {
        SdlInputBackend backend;
        BindSurface(backend);
        for (std::size_t index = 0; index <= MaximumTouchContacts; ++index)
            backend.ProcessEvent(Finger(SDL_EVENT_FINGER_DOWN, index + 1, 100 + index));
        const auto &overflow = backend.Commit();
        CHECK(overflow.touchOverflow);
        for (const auto &touch : overflow.touches) {
            CHECK(touch.cancelled);
            CHECK_FALSE(touch.contact.down);
        }
        backend.BeginFrame(3);
        backend.ProcessEvent(Finger(SDL_EVENT_FINGER_DOWN, 1, 200));
        backend.ProcessEvent(Finger(SDL_EVENT_FINGER_MOTION, 1, 201, std::numeric_limits<float>::infinity()));
        CHECK(backend.Commit().touches[0].cancelled);
    }

    TEST_CASE("SDL touch-emulated mouse events do not duplicate finger activation", "[unit][input][sdl][touch]") {
        SdlInputBackend backend;
        BindSurface(backend);
        backend.ProcessEvent(Finger(SDL_EVENT_FINGER_DOWN, 1, 100));
        SDL_Event mouse{};
        mouse.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        mouse.button.windowID = 7;
        mouse.button.which = SDL_TOUCH_MOUSEID;
        mouse.button.button = SDL_BUTTON_LEFT;
        mouse.button.down = true;
        backend.ProcessEvent(mouse);
        const auto &snapshot = backend.Commit();
        CHECK(snapshot.touches[0].contact.down);
        CHECK_FALSE(snapshot.State(PointerButton::Primary).pressed);
    }

    TEST_CASE("SDL focus loss and window unbinding cancel held touches without reviving their lineage",
              "[unit][input][sdl][touch][lifecycle]") {
        const bool unbind = GENERATE(false, true);
        RawInputSnapshot retained;
        {
            SdlInputBackend backend;
            BindSurface(backend);
            backend.ProcessEvent(Finger(SDL_EVENT_FINGER_DOWN, 1, 100));
            retained = backend.Commit();
            REQUIRE(retained.touches[0].contact.down);
            backend.BeginFrame(3);
            if (unbind)
                backend.BindWindow(0);
            else {
                SDL_Event lost{};
                lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
                lost.window.windowID = 7;
                backend.ProcessEvent(lost);
            }
            backend.ProcessEvent(Finger(SDL_EVENT_FINGER_MOTION, 1, 101, 0.9F));
            backend.ProcessEvent(Finger(SDL_EVENT_FINGER_DOWN, 2, 102));
            const auto &cancelled = backend.Commit();
            CHECK(cancelled.touches[0].cancelled);
            CHECK_FALSE(cancelled.touches[0].contact.down);
            CHECK(cancelled.touches[0].id == retained.touches[0].id);
            CHECK_FALSE(cancelled.touches[1].id.IsValid());
        }
        // Owned observation survives native collector teardown; no borrowed snapshot is retained.
        CHECK(retained.touches[0].contact.down);
        CHECK_FALSE(retained.touches[0].cancelled);
        CHECK(retained.touches[0].x == 50);
    }
}  // namespace
