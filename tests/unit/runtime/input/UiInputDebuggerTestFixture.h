#pragma once
#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiInputDebugger.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <stdexcept>

namespace Horo::Runtime::Ui::InputDebugTest {
    template <class T> T Id(std::uint8_t marker) {
        SerializedUiId bytes{};
        bytes.back() = marker;
        return T::Create(bytes).Value();
    }

    template <class T> T Rev(std::uint64_t value = 1) {
        return T::Create(value).Value();
    }

    inline UiFocusOwnerContext Owner() {
        const auto own = UiOwnershipGeneration::Create(822).Value();
        return {{own, 1, 1},
                {own, 2, 1},
                Id<UiDocumentId>(1),
                Rev<UiDocumentRevision>(),
                Rev<UiRuntimeTreeRevision>(),
                Rev<UiInteractionRevision>(),
                {{UiFocusPlayerId{own, 3, 1}}, {own, 4, 1}}};
    }

    /** @brief Minimal synchronous measurement policy; immutable publications still come from the real layout owner. */
    class Evaluator final : public UiLayoutEvaluator {
    public:
        Result<void> ResolveChildConstraints(const UiLayoutChildConstraintRequest &, std::span<UiLayoutConstraints> output) const override {
            for (auto &value : output)
                value = {{0, 0}, {200, 200}};
            return Result<void>::Success();
        }

        Result<UiLayoutMeasurement> Measure(const UiLayoutMeasureRequest &) const override {
            return Result<UiLayoutMeasurement>::Success({{100, 100}, false, false});
        }

        Result<UiLayoutArrangement> Arrange(const UiLayoutArrangeRequest &request, std::span<UiLogicalRect> children) const override {
            for (auto &value : children)
                value = {{0, 0}, {100, 100}};
            const auto rect = request.assignedContent;
            return Result<UiLayoutArrangement>::Success({rect, rect, rect, rect, rect, rect, NoUiBaseline});
        }
    };

    inline UiElementTree Tree() {
        const auto owner = Owner();
        auto allocator = UiElementSlotAllocator::Create(owner.instance.ownership).Value();
        const auto root = Id<UiElementId>(1), middle = Id<UiElementId>(2);
        const std::array source{UiElementDescriptor{root, {}}, UiElementDescriptor{middle, root},
                                UiElementDescriptor{Id<UiElementId>(3), middle}, UiElementDescriptor{Id<UiElementId>(4), root}};
        return UiElementTree::Create(allocator,
                                     {owner.instance, owner.canvas, owner.document, owner.documentRevision, owner.treeRevision, {4, 4, 4}},
                                     source)
            .Value();
    }

    inline std::array<UiFocusNodeDescriptor, 4> Nodes(const UiElementTree &tree) {
        std::array<UiFocusNodeDescriptor, 4> nodes{};
        for (std::uint8_t i = 0; i < nodes.size(); ++i) {
            auto &node = nodes[i];
            node.id = Id<UiElementId>(i + 1);
            node.element = tree.Find(node.id).Value();
            if (i)
                node.parent = Id<UiElementId>(i == 2 ? 2 : 1);
            node.focusable = i != 0;
            node.links.targets.fill(Id<UiElementId>(i == 1 ? 3 : 2));
        }
        return nodes;
    }

    inline UiFocusGraph Graph(const UiElementTree &tree) {
        return UiFocusGraph::Create({Owner(), Id<UiElementId>(2), UiFocusRecoveryPolicy::DefaultThenFirst, 4, 4, 4}, Nodes(tree)).Value();
    }

    class Handler final : public UiEventHandler {
    public:
        std::size_t calls{}, defaults{};
        UiEventResponse response;
        bool fail{}, failDefault{}, throwHandler{};

        Result<UiEventResponse> Handle(UiElementHandle, UiEventPhase, const UiRoutedEvent &) override {
            ++calls;
            if (throwHandler)
                throw std::runtime_error("debugger fixture handler exception");
            if (fail)
                return Result<UiEventResponse>::Failure(MakeError(UiErrors::EventDispatchHandlerFailed));
            return Result<UiEventResponse>::Success(response);
        }

        Result<void> ApplyDefault(UiElementHandle, const UiRoutedEvent &) override {
            ++defaults;
            return failDefault ? Result<void>::Failure(MakeError(UiErrors::EventDispatchHandlerFailed)) : Result<void>::Success();
        }
    };

    /** @brief Actual Input, tree, layout, presentation, hit, focus and capture owners sharing one explicit scope. */
    struct Fixture {
        UiElementTree tree{Tree()};
        UiFocusGraph focus{Graph(tree)};
        UiLayoutEngine layoutEngine{
            UiLayoutEngine::Create({Owner().instance, Owner().canvas, Owner().document, 4, 4, 4, Rev<UiInteractionRevision>()}).Value()};
        Evaluator evaluator;
        UiLayoutSnapshot layout{layoutEngine.Update(tree, Request()).Value()};
        UiHitTestStore hits{
            UiHitTestStore::Create({Owner().instance, Owner().canvas, Owner().document, UiRenderMode::ScreenSpaceOverlay, {200, 200}, 4, 4})
                .Value()};
        UiRenderViewId view{Owner().instance.ownership, 9, 1};
        UiPresentedInteractionState presented{UiPresentedInteractionState::Create(view, Owner().canvas).Value()};
        UiPointerCaptureStore captures{UiPointerCaptureStore::Create({Owner().instance.ownership, 4}).Value()};
        UiEventDispatcher dispatcher{UiEventDispatcher::Create({Owner().instance, Owner().canvas, Owner().document, 4}).Value()};
        Input::InputService input;
        Input::InputContextId inputId{"runtime.ui.debug"};
        Input::InputContextToken token{input.Router().PushContext(inputId, Input::InputContextKind::FocusedGuiWidget)};
        UiActionRouter actions{UiActionRouter::Create({{Owner().instance, Owner().canvas, Owner().document, Owner().documentRevision,
                                                        Owner().treeRevision, Owner().interaction},
                                                       8})
                                   .Value()};
        Input::GamepadDeviceId pad;
        std::optional<UiNavigationInput> navigation;
        std::optional<UiInputDebugger> debugger;

        Fixture() {
            REQUIRE(presented
                        .Apply({view, Owner().canvas, Owner().interaction, Rev<UiRenderSnapshotRevision>(),
                                UiPresentationOutcome::Presented, UiPresentationReason::None})
                        .HasValue());
            auto map = DefaultUiNavigationActions(inputId);
            UiNavigationInputDescriptor config;
            for (std::size_t i = 0; i < map.size() && i < UiNavigationActionCount; ++i)
                config.actions[i] = map[i].id;
            config.player = 0;
            config.modalityHysteresisMilliseconds = 0;
            REQUIRE(input.Router().SetActionMap(std::move(map)).HasValue());
            input.BeginFrame(0);
            pad = input.Collector().ConnectGamepad("debug-fixture");
            static_cast<void>(input.CommitFrame());
            REQUIRE(input.Router().AssignGamepad(0, pad));
            navigation.emplace(UiNavigationInput::Create(config, input.Router(), token, focus, actions).Value());
            REQUIRE(navigation->Pump(input.Router(), token, focus, actions, 0).HasValue());
            debugger.emplace(UiInputDebugger::Create({Owner(), 4, 4, 8, 4}, focus).Value());
            REQUIRE(debugger->SetEnabled(true).HasValue());
        }

        UiLayoutUpdateRequest Request() const {
            return {{Owner().documentRevision, Owner().treeRevision, Rev<UiLayoutContentRevision>(), Rev<UiLayoutStyleRevision>(),
                     Rev<UiLayoutIntrinsicRevision>(), Rev<UiLayoutCanvasRevision>(), Rev<UiLayoutPolicyRevision>()},
                    {{0, 0}, {200, 200}},
                    {{0, 0}, {200, 200}},
                    &evaluator};
        }

        UiEventRoute Route() const {
            const auto owner = Owner();
            return {owner.instance,
                    owner.canvas,
                    owner.document,
                    owner.treeRevision,
                    owner.interaction,
                    tree.Find(Id<UiElementId>(3)).Value(),
                    {}};
        }

        UiPointerCaptureContext Context() const {
            const auto owner = Owner();
            return {{owner.instance.ownership, 7, 1},
                    view,
                    owner.instance,
                    owner.canvas,
                    owner.document,
                    owner.treeRevision,
                    owner.interaction};
        }

        UiHitTestSnapshot Publish(bool hideTop = false, bool disableMiddle = false) {
            std::array<UiHitTestElement, 4> projection{};
            for (std::size_t i = 0; i < projection.size(); ++i) {
                projection[i].element = layout.Records()[i].element;
                projection[i].zOrder = static_cast<std::int32_t>(i);
            }
            projection[3].visible = !hideTop;
            projection[2].enabled = !disableMiddle;
            return hits.Publish(layout, projection).Value();
        }

        UiScreenPointerQuery Query() const {
            return {view, Owner().canvas, {{200, 200}, {400, 400}, {128, 1}}, 20, 20};
        }
    };
}  // namespace Horo::Runtime::Ui::InputDebugTest
