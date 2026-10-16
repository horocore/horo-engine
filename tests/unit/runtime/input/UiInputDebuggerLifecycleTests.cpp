#include "UiInputDebuggerTestFixture.h"

namespace Horo::Runtime::Ui::InputDebugTest {
    namespace {
        TEST_CASE("UI debugger follows actual structural reload and retains old route evidence", "[runtime_ui][input_debug][reload]") {
            Fixture f;
            Handler handler;
            const UiRoutedEvent event{UiEventKind::Submit, 1};
            REQUIRE(f.debugger->Dispatch(f.dispatcher, f.tree, f.Route(), event, handler).HasValue());
            auto historical = f.debugger->Recent().Value();
            auto commands =
                UiStructuralCommandBuffer::Create(Owner().instance, Owner().canvas, Owner().documentRevision, f.tree.Revision(), 1).Value();
            REQUIRE(
                commands.Add(UiReparentElementCommand{f.tree.Find(Id<UiElementId>(3)).Value(), f.tree.Find(Id<UiElementId>(1)).Value(), 0})
                    .HasValue());
            REQUIRE(f.tree.CommitDeferred(commands, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
            auto owner = Owner();
            owner.treeRevision = f.tree.Revision();
            owner.interaction = Rev<UiInteractionRevision>(2);
            auto nodes = Nodes(f.tree);
            nodes[2].parent = Id<UiElementId>(1);
            REQUIRE(f.focus.Reload({owner, Id<UiElementId>(2), UiFocusRecoveryPolicy::DefaultThenFirst, 4, 4, 4}, nodes).HasValue());
            REQUIRE(f.debugger->Rebind(f.focus).HasValue());
            auto route = f.Route();
            route.tree = owner.treeRevision;
            route.interaction = owner.interaction;
            REQUIRE(f.debugger->Dispatch(f.dispatcher, f.tree, route, event, handler).HasValue());
            auto current = f.debugger->Recent().Value();
            CHECK(current.Record().owner == owner);
            CHECK(current.Phases().size() == 3);
            CHECK(historical.Phases().size() == 5);
            CHECK(historical.Record().owner == Owner());
            CHECK(f.debugger->CaptureState(f.focus, f.captures, f.Context(), *f.navigation).HasError());
            auto oldGraph = Graph(Tree());
            CHECK(f.debugger->Rebind(oldGraph).HasError());
        }

        TEST_CASE("UI debugger world projection exposes real ray geometry and target decisions", "[runtime_ui][input_debug][world]") {
            Fixture f;
            auto store =
                UiHitTestStore::Create({Owner().instance, Owner().canvas, Owner().document, UiRenderMode::WorldSpace, {200, 200}, 4, 2})
                    .Value();
            std::array<UiHitTestElement, 4> projection{};
            for (std::size_t i = 0; i < projection.size(); ++i) {
                projection[i].element = f.layout.Records()[i].element;
                projection[i].zOrder = static_cast<std::int32_t>(i);
            }
            auto hit = store.Publish(f.layout, projection).Value();
            const UiWorldRayQuery query{f.view,
                                        Owner().canvas,
                                        {{0.5F, -0.5F, 1.0F}, {0.0F, 0.0F, -1.0F}, 0.0F, 2.0F},
                                        {{0.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F}, {0.0F, -2.0F, 0.0F}}};
            auto ordinary = hit.HitTestWorld(query, f.presented);
            auto observed = f.debugger->HitTestWorld(hit, query, f.presented);
            REQUIRE(ordinary.HasValue());
            REQUIRE(ordinary.Value());
            REQUIRE(observed.HasValue());
            REQUIRE(observed.Value());
            CHECK(observed.Value()->element == ordinary.Value()->element);
            CHECK(observed.Value()->rayDistance == ordinary.Value()->rayDistance);
            auto snapshot = f.debugger->Recent().Value();
            REQUIRE(snapshot.Record().worldQuery);
            CHECK(snapshot.Record().kind == UiInputDebugRecordKind::WorldHitTest);
            CHECK(snapshot.Candidates().size() == 4);
            CHECK(snapshot.Candidates()[0].decision == UiHitTestDecision::Selected);
        }

        TEST_CASE("UI observer reports clipping and geometric rejection in actual paint order", "[runtime_ui][input_debug][edge]") {
            Fixture f;
            std::array<UiHitTestElement, 4> projection{};
            for (std::size_t i = 0; i < projection.size(); ++i) {
                projection[i].element = f.layout.Records()[i].element;
                projection[i].zOrder = static_cast<std::int32_t>(i);
            }
            projection[3].hasClip = true;
            projection[3].clip = {{150, 150}, {10, 10}};
            projection[2].transform.values[4] = 150.0F;
            auto hit = f.hits.Publish(f.layout, projection).Value();
            REQUIRE(f.debugger->HitTestScreen(hit, f.Query(), f.presented).HasValue());
            auto snapshot = f.debugger->Recent().Value();
            REQUIRE(snapshot.Candidates().size() == 4);
            CHECK(snapshot.Candidates()[0].decision == UiHitTestDecision::OutsideClip);
            CHECK(snapshot.Candidates()[1].decision == UiHitTestDecision::OutsideGeometry);
            CHECK(snapshot.Candidates()[2].decision == UiHitTestDecision::Selected);
            CHECK(snapshot.Candidates()[0].hasClip);
            CHECK(snapshot.Candidates()[1].canvasToLocal[4] != 0.0);
        }

        TEST_CASE("UI debugger projects canonical gamepad modality and rejects retired state", "[runtime_ui][input_debug][lifecycle]") {
            Fixture f;
            f.input.BeginFrame(1);
            REQUIRE(f.input.Collector().SetGamepadButton(f.pad, Input::GamepadButton::DPadDown, true));
            static_cast<void>(f.input.CommitFrame());
            REQUIRE(f.navigation->Pump(f.input.Router(), f.token, f.focus, f.actions, 1).HasValue());
            auto state = f.debugger->CaptureState(f.focus, f.captures, f.Context(), *f.navigation);
            REQUIRE(state.HasValue());
            CHECK(state.Value().Record().modality == Input::InputModality::Gamepad);
            CHECK(state.Value().Record().modalityDevice == f.pad);
            CHECK(state.Value().Record().inputPlayer == std::optional<Input::PlayerId>{0});
            f.navigation->Shutdown();
            CHECK(f.debugger->CaptureState(f.focus, f.captures, f.Context(), *f.navigation).HasError());
            f.focus.Shutdown();
            CHECK(f.debugger->Rebind(f.focus).HasError());
            CHECK(state.Value().Record().owner == Owner());
        }

        class ReentrantHandler final : public UiEventHandler {
        public:
            explicit ReentrantHandler(Fixture &value) : fixture(value) {}

            Fixture &fixture;
            bool rejected{};
            bool nestedRejected{};
            bool attempted{};

            Result<UiEventResponse> Handle(UiElementHandle, UiEventPhase, const UiRoutedEvent &event) override {
                if (!attempted) {
                    attempted = true;
                    rejected = fixture.debugger->SetEnabled(false).HasError();
                    nestedRejected = fixture.debugger->Dispatch(fixture.dispatcher, fixture.tree, fixture.Route(), event, *this).HasError();
                }
                return Result<UiEventResponse>::Success({});
            }

            Result<void> ApplyDefault(UiElementHandle, const UiRoutedEvent &) override {
                return Result<void>::Success();
            }
        };

        TEST_CASE("UI debugger rejects reentrant control without corrupting the actual outer route", "[runtime_ui][input_debug][edge]") {
            Fixture f;
            ReentrantHandler handler{f};
            auto result = f.debugger->Dispatch(f.dispatcher, f.tree, f.Route(), {UiEventKind::Submit, 1}, handler);
            REQUIRE(result.HasValue());
            CHECK(result.Value().phasesVisited == 5);
            CHECK(handler.rejected);
            CHECK(handler.nestedRejected);
            CHECK(f.debugger->Dropped() == 1);
            auto snapshot = f.debugger->Recent().Value();
            CHECK(snapshot.Phases().size() == 5);
            CHECK_FALSE(snapshot.Record().failed);
        }

        TEST_CASE("UI dispatcher observation preserves propagation and default suppression", "[runtime_ui][input_debug][edge]") {
            Fixture f;
            Handler handler;
            handler.response = {true, true, true};
            auto result = f.debugger->Dispatch(f.dispatcher, f.tree, f.Route(), {UiEventKind::Submit, 1}, handler);
            REQUIRE(result.HasValue());
            CHECK(handler.calls == 1);
            CHECK(handler.defaults == 0);
            auto snapshot = f.debugger->Recent().Value();
            REQUIRE(snapshot.Phases().size() == 1);
            CHECK(snapshot.Phases()[0].response.stopPropagation);
            CHECK(snapshot.Record().dispatch.propagationStopped);
            CHECK(snapshot.Record().dispatch.defaultPrevented);
            CHECK_FALSE(snapshot.Record().defaultAttempted);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui::InputDebugTest
