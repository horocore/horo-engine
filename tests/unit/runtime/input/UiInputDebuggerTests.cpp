#include "UiInputDebuggerTestFixture.h"

#include <thread>

namespace Horo::Runtime::Ui::InputDebugTest {
    namespace {
        constexpr UiRoutedEvent Event{UiEventKind::Submit, 1};

        TEST_CASE("UI input debugger validates finite storage and actual scope", "[runtime_ui][input_debug][edge]") {
            Fixture f;
            auto descriptor = UiInputDebuggerDescriptor{Owner(), 4, 4, 8, 4};
            descriptor.historyCapacity = 0;
            CHECK_FALSE(descriptor.IsValid());
            CHECK(UiInputDebugger::Create(descriptor, f.focus).HasError());
            descriptor.historyCapacity = MaximumUiInputDebugHistory + 1;
            CHECK_FALSE(descriptor.IsValid());
            descriptor = {Owner(), MaximumUiInputDebugHistory, MaximumUiTreeElements, 2 * MaximumUiTreeDepth, MaximumUiPointerCaptures};
            CHECK_FALSE(descriptor.IsValid());
            descriptor = {Owner(), 4, 4, 8, 4};
            descriptor.owner.scope.player->slot++;
            CHECK(UiInputDebugger::Create(descriptor, f.focus).HasError());
        }

        TEST_CASE("UI debugger observes actual targeting without allocating or changing selection", "[runtime_ui][input_debug][normal]") {
            Fixture f;
            auto hit = f.Publish();
            const auto ordinary = hit.HitTestScreen(f.Query(), f.presented);
            const auto before = ::Horo::Tests::AllocationProbe::Count();
            const auto observed = f.debugger->HitTestScreen(hit, f.Query(), f.presented);
            const auto after = ::Horo::Tests::AllocationProbe::Count();
            REQUIRE(observed.HasValue());
            REQUIRE(observed.Value());
            REQUIRE(ordinary.HasValue());
            REQUIRE(ordinary.Value());
            CHECK(observed.Value()->element == ordinary.Value()->element);
            CHECK(after == before);
            auto snapshot = f.debugger->Recent().Value();
            REQUIRE(snapshot.Candidates().size() == 4);
            CHECK(snapshot.Candidates()[0].decision == UiHitTestDecision::Selected);
            CHECK(snapshot.Candidates()[1].decision == UiHitTestDecision::Occluded);
            CHECK(snapshot.Record().screenQuery.has_value());
            CHECK(snapshot.Record().hitSource.has_value());
            CHECK(snapshot.Record().omittedCandidates == 0);
        }

        TEST_CASE("UI debugger records actual hidden and disabled targeting decisions", "[runtime_ui][input_debug][edge]") {
            Fixture f;
            auto hit = f.Publish(true, true);
            auto result = f.debugger->HitTestScreen(hit, f.Query(), f.presented);
            REQUIRE(result.HasValue());
            auto snapshot = f.debugger->Recent().Value();
            REQUIRE(snapshot.Candidates().size() == 4);
            CHECK(snapshot.Candidates()[0].decision == UiHitTestDecision::Hidden);
            CHECK(snapshot.Candidates()[1].decision == UiHitTestDecision::Disabled);
            CHECK(snapshot.Candidates()[2].decision == UiHitTestDecision::Selected);
        }

        TEST_CASE("UI debugger traces actual propagation and default decisions", "[runtime_ui][input_debug][normal]") {
            Fixture f;
            Handler handler;
            handler.response.handled = true;
            const auto before = ::Horo::Tests::AllocationProbe::Count();
            auto result = f.debugger->Dispatch(f.dispatcher, f.tree, f.Route(), Event, handler);
            const auto after = ::Horo::Tests::AllocationProbe::Count();
            REQUIRE(result.HasValue());
            CHECK(after == before);
            CHECK(handler.calls == 5);
            CHECK(handler.defaults == 1);
            auto snapshot = f.debugger->Recent().Value();
            REQUIRE(snapshot.Phases().size() == 5);
            CHECK(snapshot.Phases()[0].phase == UiEventPhase::Capture);
            CHECK(snapshot.Phases()[2].phase == UiEventPhase::Target);
            CHECK(snapshot.Phases()[4].phase == UiEventPhase::Bubble);
            CHECK(snapshot.Record().dispatch.handled);
            CHECK(snapshot.Record().dispatch.defaultApplied);
            CHECK(snapshot.Record().defaultAttempted);
        }

        TEST_CASE("UI debugger retains failed callback and default outcomes", "[runtime_ui][input_debug][edge]") {
            Fixture f;
            Handler handler;
            SECTION("handler failure") {
                handler.fail = true;
                CHECK(f.debugger->Dispatch(f.dispatcher, f.tree, f.Route(), Event, handler).HasError());
                auto snapshot = f.debugger->Recent().Value();
                REQUIRE(snapshot.Phases().size() == 1);
                CHECK(snapshot.Phases()[0].failed);
                CHECK_FALSE(snapshot.Phases()[0].responded);
                CHECK_FALSE(snapshot.Record().defaultAttempted);
            }
            SECTION("handler exception") {
                handler.throwHandler = true;
                CHECK(f.debugger->Dispatch(f.dispatcher, f.tree, f.Route(), Event, handler).HasError());
                auto snapshot = f.debugger->Recent().Value();
                REQUIRE(snapshot.Phases().size() == 1);
                CHECK(snapshot.Phases()[0].failed);
                CHECK_FALSE(snapshot.Phases()[0].responded);
            }
            SECTION("default failure") {
                handler.failDefault = true;
                CHECK(f.debugger->Dispatch(f.dispatcher, f.tree, f.Route(), Event, handler).HasError());
                auto snapshot = f.debugger->Recent().Value();
                CHECK(snapshot.Phases().size() == 5);
                CHECK(snapshot.Record().defaultAttempted);
                CHECK(snapshot.Record().defaultFailed);
                CHECK_FALSE(snapshot.Record().dispatch.defaultApplied);
            }
            CHECK(f.debugger->Recent().Value().Record().failed);
        }

        TEST_CASE("UI debugger truncation cannot truncate actual routing or targeting", "[runtime_ui][input_debug][edge]") {
            Fixture f;
            auto debugger = UiInputDebugger::Create({Owner(), 2, 1, 1, 1}, f.focus).Value();
            REQUIRE(debugger.SetEnabled(true).HasValue());
            auto hit = f.Publish();
            Handler handler;
            REQUIRE(debugger.HitTestScreen(hit, f.Query(), f.presented).HasValue());
            auto targeting = debugger.Recent().Value();
            CHECK(targeting.Candidates().size() == 1);
            CHECK(targeting.Record().omittedCandidates == 3);
            REQUIRE(debugger.Dispatch(f.dispatcher, f.tree, f.Route(), Event, handler).HasValue());
            auto route = debugger.Recent().Value();
            CHECK(route.Phases().size() == 1);
            CHECK(route.Record().omittedPhases == 4);
            CHECK(handler.calls == 5);
            CHECK(handler.defaults == 1);
            CHECK(route.Record().dispatch.phasesVisited == 5);
        }

        TEST_CASE("Pinned UI history drops observation and preserves immutable leases", "[runtime_ui][input_debug][lifecycle]") {
            std::optional<UiInputDebugSnapshot> retained;
            {
                Fixture f;
                Handler handler;
                std::array<std::optional<UiInputDebugSnapshot>, 4> pinned;
                for (auto &entry : pinned) {
                    REQUIRE(f.debugger->Dispatch(f.dispatcher, f.tree, f.Route(), Event, handler).HasValue());
                    entry.emplace(f.debugger->Recent().Value());
                }
                retained = pinned[0];
                REQUIRE(f.debugger->Dispatch(f.dispatcher, f.tree, f.Route(), Event, handler).HasValue());
                CHECK(handler.calls == 25);
                CHECK(f.debugger->Dropped() == 1);
                CHECK(retained->Record().serial == 1);
                pinned[1].reset();
                REQUIRE(f.debugger->Dispatch(f.dispatcher, f.tree, f.Route(), Event, handler).HasValue());
                CHECK(f.debugger->Recent().Value().Record().serial == 5);
                f.debugger->Shutdown();
                CHECK(f.debugger->SetEnabled(true).HasError());
            }
            REQUIRE(retained);
            CHECK(retained->Record().serial == 1);
            CHECK(retained->Phases().size() == 5);
        }

        TEST_CASE("UI state inspection borrows actual capture and focus without mutation", "[runtime_ui][input_debug][normal]") {
            Fixture f;
            const auto context = f.Context();
            auto captured =
                f.captures.Capture({context.context, UiPointerId::Create(1).Value(), UiPointerButton::Primary, f.view, f.Route()}, f.tree,
                                   f.presented);
            REQUIRE(captured.HasValue());
            auto token = std::move(captured).Value();
            const auto beforeFocus = f.focus.Snapshot().Value();
            const auto before = ::Horo::Tests::AllocationProbe::Count();
            auto state = f.debugger->CaptureState(f.focus, f.captures, context, *f.navigation);
            const auto after = ::Horo::Tests::AllocationProbe::Count();
            REQUIRE(state.HasValue());
            CHECK(after == before);
            REQUIRE(state.Value().Captures().size() == 1);
            CHECK(state.Value().Captures()[0].route.target == f.Route().target);
            CHECK(token.IsActive());
            CHECK(state.Value().Record().inputPlayer == std::optional<Input::PlayerId>{0});
            CHECK(state.Value().Record().owner.scope == Owner().scope);
            CHECK(f.focus.Snapshot().Value().focused == beforeFocus.focused);
            token.Release();
            CHECK(f.captures.ActiveCount() == 0);
            CHECK(state.Value().Captures().size() == 1);
            const auto preview = f.focus.PreviewMove(UiNavigationDirection::Next).Value();
            const auto moved = f.focus.Move(UiNavigationDirection::Next);
            REQUIRE(moved.HasValue());
            CHECK(f.focus.Snapshot().Value().focused == preview.target);
        }

        TEST_CASE("Disabled or wrong-thread UI debugging leaves input authority unchanged", "[runtime_ui][input_debug][lifecycle]") {
            Fixture f;
            Handler handler;
            REQUIRE(f.debugger->SetEnabled(false).HasValue());
            REQUIRE(f.debugger->Dispatch(f.dispatcher, f.tree, f.Route(), Event, handler).HasValue());
            CHECK(handler.calls == 5);
            CHECK(f.debugger->Recent().HasError());
            bool rejected = false;
            std::thread other{[&] {
                rejected = f.debugger->SetEnabled(true).HasError();
            }};
            other.join();
            CHECK(rejected);
            CHECK(f.debugger->CaptureState(f.focus, f.captures, f.Context(), *f.navigation).HasError());
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui::InputDebugTest
