#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiEventDispatch.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Id> Id AuthoredId(const std::uint8_t marker) {
            return Id::Create(SerializedUiId{marker}).Value();
        }

        UiOwnershipGeneration Ownership() {
            return UiOwnershipGeneration::Create(73).Value();
        }

        template <typename Revision> Revision RevisionValue(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        UiElementTreeDescriptor Descriptor() {
            return {{Ownership(), 1, 1},
                    {Ownership(), 2, 1},
                    AuthoredId<UiDocumentId>(1),
                    RevisionValue<UiDocumentRevision>(1),
                    RevisionValue<UiRuntimeTreeRevision>(1),
                    {8, 4, 4}};
        }

        UiElementTree Tree() {
            auto slots = std::move(UiElementSlotAllocator::Create(Ownership())).Value();
            const auto root = AuthoredId<UiElementId>(1);
            const auto modal = AuthoredId<UiElementId>(2);
            const std::array elements{UiElementDescriptor{root, {}}, UiElementDescriptor{modal, root},
                                      UiElementDescriptor{AuthoredId<UiElementId>(3), modal},
                                      UiElementDescriptor{AuthoredId<UiElementId>(4), root}};
            auto tree = UiElementTree::Create(slots, Descriptor(), elements);
            REQUIRE(tree.HasValue());
            return std::move(tree).Value();
        }

        UiElementHandle Find(const UiElementTree &tree, const std::uint8_t marker) {
            return tree.Find(AuthoredId<UiElementId>(marker)).Value();
        }

        UiEventRoute Route(const UiElementTree &tree, const UiElementHandle target,
                           const std::optional<UiElementHandle> modal = std::nullopt) {
            const auto descriptor = Descriptor();
            return {descriptor.instance,
                    descriptor.canvas,
                    descriptor.document,
                    tree.Revision(),
                    RevisionValue<UiInteractionRevision>(9),
                    target,
                    modal};
        }

        UiRoutedEvent SubmitEvent(const std::uint64_t sequence = 1) {
            return {UiEventKind::Submit, sequence, {}, false};
        }

        template <typename T> void ExpectError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        struct Invocation final {
            UiElementHandle element;
            UiEventPhase phase{};

            bool operator==(const Invocation &) const noexcept = default;
        };

        class RecordingHandler final : public UiEventHandler {
        public:
            Result<UiEventResponse> Handle(const UiElementHandle element, const UiEventPhase phase, const UiRoutedEvent &event) override {
                calls[callCount++] = {element, phase};
                if (throwAt && element == *throwAt)
                    throw std::runtime_error{"injected event handler failure"};
                if (nestedDispatcher && element == nestedRoute->target && phase == UiEventPhase::Target)
                    nestedResult.emplace(nestedDispatcher->Dispatch(*nestedTree, *nestedRoute, event, *this));
                if (mutatingTree && element == mutationTarget && phase == UiEventPhase::Target) {
                    auto committed = mutatingTree->CommitDeferred(*mutation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands);
                    if (committed.HasError())
                        return Result<UiEventResponse>::Failure(committed.ErrorValue());
                }
                return Result<UiEventResponse>::Success(
                    {element == handledAt, element == stopAt && phase == stopPhase, element == preventAt});
            }

            Result<void> ApplyDefault(const UiElementHandle target, const UiRoutedEvent &) override {
                defaultTarget = target;
                ++defaultCount;
                return Result<void>::Success();
            }

            std::array<Invocation, 16> calls{};
            std::size_t callCount{};
            std::size_t defaultCount{};
            UiElementHandle defaultTarget;
            UiElementHandle handledAt;
            UiElementHandle stopAt;
            UiEventPhase stopPhase{UiEventPhase::Count};
            UiElementHandle preventAt;
            std::optional<UiElementHandle> throwAt;
            UiEventDispatcher *nestedDispatcher{};
            const UiElementTree *nestedTree{};
            const UiEventRoute *nestedRoute{};
            std::optional<Result<UiEventDispatchResult>> nestedResult;
            UiElementTree *mutatingTree{};
            const UiStructuralCommandBuffer *mutation{};
            UiElementHandle mutationTarget;
        };

        UiEventDispatcher Dispatcher(const std::uint32_t depth = 4) {
            const auto descriptor = Descriptor();
            auto dispatcher = UiEventDispatcher::Create({descriptor.instance, descriptor.canvas, descriptor.document, depth});
            REQUIRE(dispatcher.HasValue());
            return std::move(dispatcher).Value();
        }

        TEST_CASE("Physical and accessible gestures share routed default prevention", "[runtime_ui][event_dispatch][gesture]") {
            auto tree = Tree();
            auto dispatcher = Dispatcher();
            const auto target = Find(tree, 3);
            UiRoutedEvent event{UiEventKind::Gesture, 1, {32, 64}, true, UiGestureEvent{UiGestureKind::Drop, 1, Find(tree, 4)}};
            RecordingHandler handler;
            handler.preventAt = Find(tree, 2);
            const auto prevented = dispatcher.Dispatch(tree, Route(tree, target), event, handler);
            REQUIRE(prevented.HasValue());
            CHECK(prevented.Value().defaultPrevented);
            CHECK_FALSE(prevented.Value().defaultApplied);
            CHECK(handler.defaultCount == 0);

            event.sequence = 2;
            event.hasLogicalPosition = false;
            event.logicalPosition = {};
            event.gesture->pointer = 0;
            event.gesture->accessible = true;
            RecordingHandler accessibleHandler;
            const auto applied = dispatcher.Dispatch(tree, Route(tree, target), event, accessibleHandler);
            REQUIRE(applied.HasValue());
            CHECK(applied.Value().defaultApplied);
            CHECK(accessibleHandler.defaultTarget == target);
            CHECK(accessibleHandler.defaultCount == 1);
        }

        TEST_CASE("Gesture malformed geometry and foreign source reject before any handler", "[runtime_ui][event_dispatch][gesture]") {
            auto tree = Tree();
            auto dispatcher = Dispatcher();
            const auto target = Find(tree, 3);
            UiRoutedEvent event{UiEventKind::Gesture, 1, {}, true, UiGestureEvent{UiGestureKind::PinchRotate, 1, target}};
            RecordingHandler handler;
            event.gesture->scale = std::numeric_limits<double>::quiet_NaN();
            ExpectError(dispatcher.Dispatch(tree, Route(tree, target), event, handler), UiErrors::EventDispatchInvalid);
            event.gesture->scale = 1.0;
            event.gesture->rotation = std::numeric_limits<double>::infinity();
            ExpectError(dispatcher.Dispatch(tree, Route(tree, target), event, handler), UiErrors::EventDispatchInvalid);
            event.gesture->rotation = 0.0;
            ++event.gesture->source.generation;
            ExpectError(dispatcher.Dispatch(tree, Route(tree, target), event, handler), UiErrors::EventDispatchSourceStale);
            event.gesture.reset();
            ExpectError(dispatcher.Dispatch(tree, Route(tree, target), event, handler), UiErrors::EventDispatchInvalid);
            CHECK(handler.callCount == 0);
            CHECK(handler.defaultCount == 0);
        }

        TEST_CASE("UI events follow frozen capture target bubble order without dispatch allocation",
                  "[runtime_ui][event_dispatch][phases]") {
            auto tree = Tree();
            auto dispatcher = Dispatcher();
            const auto root = Find(tree, 1);
            const auto parent = Find(tree, 2);
            const auto target = Find(tree, 3);
            const auto route = Route(tree, target);
            RecordingHandler handler;
            handler.handledAt = parent;

            const auto allocationsBefore = ::Horo::Tests::AllocationProbe::Count();
            const auto dispatched = dispatcher.Dispatch(tree, route, SubmitEvent(), handler);
            const auto allocationsAfter = ::Horo::Tests::AllocationProbe::Count();

            REQUIRE(dispatched.HasValue());
            CHECK(allocationsAfter == allocationsBefore);
            CHECK(dispatched.Value().phasesVisited == 5);
            CHECK(dispatched.Value().handled);
            CHECK(dispatched.Value().defaultApplied);
            REQUIRE(handler.callCount == 5);
            CHECK(handler.calls[0] == Invocation{root, UiEventPhase::Capture});
            CHECK(handler.calls[1] == Invocation{parent, UiEventPhase::Capture});
            CHECK(handler.calls[2] == Invocation{target, UiEventPhase::Target});
            CHECK(handler.calls[3] == Invocation{parent, UiEventPhase::Bubble});
            CHECK(handler.calls[4] == Invocation{root, UiEventPhase::Bubble});
            CHECK(handler.defaultTarget == target);
        }

        TEST_CASE("Handled propagation and default action controls remain independent", "[runtime_ui][event_dispatch][semantics]") {
            auto tree = Tree();
            auto dispatcher = Dispatcher();
            const auto parent = Find(tree, 2);
            const auto target = Find(tree, 3);
            const auto route = Route(tree, target);

            RecordingHandler prevented;
            prevented.handledAt = target;
            prevented.preventAt = parent;
            const auto first = dispatcher.Dispatch(tree, route, SubmitEvent(), prevented);
            REQUIRE(first.HasValue());
            CHECK(first.Value().handled);
            CHECK(first.Value().defaultPrevented);
            CHECK_FALSE(first.Value().defaultApplied);

            RecordingHandler stopped;
            stopped.stopAt = parent;
            stopped.stopPhase = UiEventPhase::Capture;
            const auto second = dispatcher.Dispatch(tree, route, SubmitEvent(2), stopped);
            REQUIRE(second.HasValue());
            CHECK(second.Value().phasesVisited == 2);
            CHECK(second.Value().propagationStopped);
            CHECK(second.Value().defaultApplied);
        }

        TEST_CASE("Modal roots bound routes and reject targets outside the modal subtree", "[runtime_ui][event_dispatch][modal]") {
            auto tree = Tree();
            auto dispatcher = Dispatcher();
            const auto modal = Find(tree, 2);
            RecordingHandler handler;
            const auto admitted = dispatcher.Dispatch(tree, Route(tree, Find(tree, 3), modal), SubmitEvent(), handler);
            REQUIRE(admitted.HasValue());
            REQUIRE(handler.callCount == 3);
            CHECK(handler.calls[0] == Invocation{modal, UiEventPhase::Capture});
            CHECK(handler.calls[2] == Invocation{modal, UiEventPhase::Bubble});

            ExpectError(dispatcher.Dispatch(tree, Route(tree, Find(tree, 4), modal), SubmitEvent(2), handler),
                        UiErrors::EventDispatchModalBoundaryViolation);
        }

        TEST_CASE("Structural mutation aborts a frozen route before later callbacks or default action",
                  "[runtime_ui][event_dispatch][mutation]") {
            auto tree = Tree();
            auto dispatcher = Dispatcher();
            const auto target = Find(tree, 3);
            auto mutation = std::move(UiStructuralCommandBuffer::Create(tree.Instance(), tree.Canvas(), tree.SourceDocumentRevision(),
                                                                        tree.Revision(), 1))
                                .Value();
            REQUIRE(mutation.Add(UiRemoveElementCommand{target}).HasValue());
            RecordingHandler handler;
            handler.mutatingTree = &tree;
            handler.mutation = &mutation;
            handler.mutationTarget = target;

            ExpectError(dispatcher.Dispatch(tree, Route(tree, target), SubmitEvent(), handler), UiErrors::EventDispatchRouteInvalidated);
            CHECK(handler.callCount == 3);
            CHECK(handler.defaultCount == 0);
        }

        TEST_CASE("Reentrancy exceptions capacity and lifecycle fail without corrupting later dispatch",
                  "[runtime_ui][event_dispatch][failures]") {
            auto tree = Tree();
            auto dispatcher = Dispatcher();
            const auto target = Find(tree, 3);
            const auto route = Route(tree, target);
            RecordingHandler rejected;
            auto staleRoute = route;
            ++staleRoute.target.generation;
            ExpectError(dispatcher.Dispatch(tree, staleRoute, SubmitEvent(), rejected), UiErrors::EventDispatchSourceStale);
            auto malformedEvent = SubmitEvent();
            malformedEvent.kind = UiEventKind::PointerPress;
            ExpectError(dispatcher.Dispatch(tree, route, malformedEvent, rejected), UiErrors::EventDispatchInvalid);

            RecordingHandler nested;
            nested.nestedDispatcher = &dispatcher;
            nested.nestedTree = &tree;
            nested.nestedRoute = &route;
            REQUIRE(dispatcher.Dispatch(tree, route, SubmitEvent(), nested).HasValue());
            REQUIRE(nested.nestedResult.has_value());
            ExpectError(*nested.nestedResult, UiErrors::EventDispatchReentrant);

            RecordingHandler throwing;
            throwing.throwAt = target;
            ExpectError(dispatcher.Dispatch(tree, route, SubmitEvent(2), throwing), UiErrors::EventDispatchHandlerFailed);
            RecordingHandler recovered;
            REQUIRE(dispatcher.Dispatch(tree, route, SubmitEvent(3), recovered).HasValue());

            auto shallow = Dispatcher(2);
            RecordingHandler bounded;
            ExpectError(shallow.Dispatch(tree, route, SubmitEvent(4), bounded), UiErrors::EventDispatchCapacityExceeded);

            REQUIRE(dispatcher.BeginRetirement().HasValue());
            ExpectError(dispatcher.Dispatch(tree, route, SubmitEvent(5), recovered), UiErrors::EventDispatchLifecycleUnavailable);
            dispatcher.Shutdown();
            dispatcher.Shutdown();
            CHECK(dispatcher.State() == UiEventDispatcherState::Stopped);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
