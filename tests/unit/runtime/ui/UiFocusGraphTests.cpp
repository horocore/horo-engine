#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiFocusGraph.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Identity> Identity Stable(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            const auto result = Identity::Create(bytes);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        UiOwnershipGeneration Owner(const std::uint64_t value = 73) {
            const auto result = UiOwnershipGeneration::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        template <typename Revision> Revision RevisionValue(const std::uint64_t value) {
            const auto result = Revision::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        UiFocusOwnerContext Context(const std::uint64_t ownerValue = 73, const std::uint64_t interaction = 3,
                                    const std::uint32_t layerSlot = 4) {
            const UiOwnershipGeneration owner = Owner(ownerValue);
            return {RuntimeUiInstanceId{owner, 1, 1},
                    UiCanvasInstanceId{owner, 2, 1},
                    Stable<UiDocumentId>(90),
                    RevisionValue<UiDocumentRevision>(1),
                    RevisionValue<UiRuntimeTreeRevision>(1),
                    RevisionValue<UiInteractionRevision>(interaction),
                    UiFocusScope{UiFocusPlayerId{owner, 3, 1}, UiFocusPresentationLayerId{owner, layerSlot, 1}}};
        }

        UiFocusGraphDescriptor Descriptor(const UiFocusOwnerContext &owner = Context(), const UiElementId defaultFocus = {}) {
            return {owner, defaultFocus, UiFocusRecoveryPolicy::AncestorThenDefaultThenFirst, 8, 4, 4};
        }

        UiFocusNodeDescriptor Node(const UiOwnershipGeneration owner, const std::uint32_t slot, const std::uint8_t id,
                                   const UiElementId parent = {}, const bool focusable = true) {
            return {UiElementHandle{owner, slot, 1},
                    Stable<UiElementId>(id),
                    parent,
                    {},
                    UiFocusBringIntoViewPolicy::Nearest,
                    focusable,
                    true,
                    true};
        }

        UiFocusGraph CreateGraph(const UiFocusGraphDescriptor &descriptor, const std::span<const UiFocusNodeDescriptor> nodes) {
            auto result = UiFocusGraph::Create(descriptor, nodes);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        std::array<UiFocusNodeDescriptor, 3> BasicNodes(const UiOwnershipGeneration owner) {
            const UiElementId root = Stable<UiElementId>(1);
            const UiElementId first = Stable<UiElementId>(2);
            const UiElementId second = Stable<UiElementId>(3);
            std::array nodes{Node(owner, 10, 1, {}, false), Node(owner, 11, 2, root), Node(owner, 12, 3, root)};
            nodes[1].links.targets[0] = second;
            nodes[2].links.targets[1] = first;
            return nodes;
        }

        std::array<UiFocusNodeDescriptor, 6> ModalNodes(const UiOwnershipGeneration owner) {
            const UiElementId root = Stable<UiElementId>(1);
            const UiElementId modalRoot = Stable<UiElementId>(4);
            const UiElementId modalFirst = Stable<UiElementId>(5);
            const UiElementId modalSecond = Stable<UiElementId>(6);
            std::array nodes{Node(owner, 10, 1, {}, false),   Node(owner, 11, 2, root),      Node(owner, 12, 3, root),
                             Node(owner, 13, 4, root, false), Node(owner, 14, 5, modalRoot), Node(owner, 15, 6, modalRoot)};
            nodes[4].links.targets[0] = modalSecond;
            nodes[5].links.targets[1] = modalFirst;
            return nodes;
        }

        void ExpectError(const Result<void> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        template <typename T> void ExpectError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        TEST_CASE("Focus graph scopes defaults and explicit links without frame-hot allocation", "[runtime_ui][focus]") {
            const UiOwnershipGeneration owner = Owner();
            auto nodes = BasicNodes(owner);
            auto graph = CreateGraph(Descriptor(Context(), Stable<UiElementId>(2)), nodes);

            const auto initial = graph.CurrentFocus();
            REQUIRE(initial.HasValue());
            REQUIRE(initial.Value().has_value());
            CHECK(initial.Value()->id == Stable<UiElementId>(2));
            CHECK(graph.Owner().scope.player.has_value());
            CHECK(graph.State() == UiFocusGraphState::Active);

            const auto allocationsBefore = ::Horo::Tests::AllocationProbe::Count();
            const auto moved = graph.Move(UiNavigationDirection::Next);
            const auto allocationsAfter = ::Horo::Tests::AllocationProbe::Count();
            REQUIRE(moved.HasValue());
            CHECK(allocationsAfter == allocationsBefore);
            CHECK(moved.Value().kind == UiFocusChangeKind::FocusMoved);
            CHECK(moved.Value().reason == UiFocusChangeReason::Link);
            REQUIRE(moved.Value().current.has_value());
            CHECK(moved.Value().current->id == Stable<UiElementId>(3));
            REQUIRE(moved.Value().bringIntoView.has_value());
            CHECK(moved.Value().bringIntoView->target == *moved.Value().current);
            CHECK(moved.Value().IsValid());

            const auto noTarget = graph.Move(UiNavigationDirection::Up);
            REQUIRE(noTarget.HasValue());
            CHECK(noTarget.Value().kind == UiFocusChangeKind::NoTarget);
            CHECK(noTarget.Value().current == moved.Value().current);
            CHECK(noTarget.Value().IsValid());

            ExpectError(graph.Move(UiNavigationDirection::Submit), UiErrors::FocusInvalid);
            ExpectError(graph.SetFocus(UiElementHandle{Owner(74), 11, 1}), UiErrors::FocusSourceStale);
        }

        TEST_CASE("Focus graph traps modal scopes and restores newest valid focus", "[runtime_ui][focus][modal]") {
            const UiOwnershipGeneration owner = Owner();
            auto nodes = ModalNodes(owner);
            auto graph = CreateGraph(Descriptor(Context(), Stable<UiElementId>(2)), nodes);

            const UiFocusModalDescriptor modalDescriptor{nodes[3].element, nodes[3].id, nodes[4].id};
            const auto opened = graph.PushModal(modalDescriptor);
            REQUIRE(opened.HasValue());
            CHECK(opened.Value().IsValid());
            CHECK(opened.Value().change.reason == UiFocusChangeReason::ModalOpened);
            REQUIRE(opened.Value().change.current.has_value());
            CHECK(opened.Value().change.current->id == nodes[4].id);

            const auto snapshot = graph.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().activeModal.has_value());
            CHECK(snapshot.Value().activeModal.value() == opened.Value().modal);
            CHECK(snapshot.Value().modalDepth == 1);

            ExpectError(graph.SetFocus(nodes[1].element), UiErrors::FocusModalBoundaryViolation);
            const auto nestedOutside = graph.PushModal({nodes[1].element, nodes[1].id, nodes[1].id});
            ExpectError(nestedOutside, UiErrors::FocusModalBoundaryViolation);

            const auto modalMove = graph.Move(UiNavigationDirection::Next);
            REQUIRE(modalMove.HasValue());
            REQUIRE(modalMove.Value().current.has_value());
            CHECK(modalMove.Value().current->id == nodes[5].id);

            const auto closed = graph.PopModal(opened.Value().modal);
            REQUIRE(closed.HasValue());
            CHECK(closed.Value().reason == UiFocusChangeReason::ModalClosed);
            CHECK(closed.Value().kind == UiFocusChangeKind::FocusRecovered);
            REQUIRE(closed.Value().current.has_value());
            CHECK(closed.Value().current->id == nodes[1].id);
            ExpectError(graph.PopModal(opened.Value().modal), UiErrors::FocusModalStale);
        }

        TEST_CASE("Runtime UI modal focus stays within its player presentation scope", "[runtime_ui][focus][modal]") {
            const UiOwnershipGeneration owner = Owner();
            auto nodes = ModalNodes(owner);
            auto first = CreateGraph(Descriptor(Context(73, 3, 1), Stable<UiElementId>(2)), nodes);
            auto second = CreateGraph(Descriptor(Context(73, 3, 2), Stable<UiElementId>(3)), nodes);

            const auto opened = first.PushModal({nodes[3].element, nodes[3].id, nodes[4].id});
            REQUIRE(opened.HasValue());
            REQUIRE(first.CurrentFocus().HasValue());
            CHECK(first.CurrentFocus().Value()->id == nodes[4].id);
            REQUIRE(second.CurrentFocus().HasValue());
            CHECK(second.CurrentFocus().Value()->id == nodes[2].id);
            CHECK_FALSE(second.Snapshot().Value().activeModal.has_value());

            REQUIRE(first.PopModal(opened.Value().modal).HasValue());
            CHECK(first.CurrentFocus().Value()->id == nodes[1].id);
            CHECK(second.CurrentFocus().Value()->id == nodes[2].id);
        }

        TEST_CASE("Focus graph reload reconciles stable IDs transactionally and fences stale revisions", "[runtime_ui][focus][reload]") {
            const UiOwnershipGeneration owner = Owner();
            auto nodes = BasicNodes(owner);
            auto graph = CreateGraph(Descriptor(Context(), Stable<UiElementId>(2)), nodes);
            REQUIRE(graph.SetFocus(nodes[2].element).HasValue());

            auto replacement = BasicNodes(owner);
            replacement[2].element.slot = 22;
            replacement[1].element.slot = 21;
            auto reloadDescriptor = Descriptor(Context(73, 4), Stable<UiElementId>(2));
            const auto reloaded = graph.Reload(reloadDescriptor, replacement);
            REQUIRE(reloaded.HasValue());
            CHECK(reloaded.Value().reason == UiFocusChangeReason::Reload);
            CHECK(reloaded.Value().kind == UiFocusChangeKind::FocusRecovered);
            REQUIRE(reloaded.Value().current.has_value());
            CHECK(reloaded.Value().current->id == Stable<UiElementId>(3));
            CHECK(reloaded.Value().current->element.slot == 22);

            auto missing = BasicNodes(owner);
            missing[2].id = Stable<UiElementId>(7);
            missing[2].parent = missing[0].id;
            const auto recovered = graph.Reload(Descriptor(Context(73, 5), Stable<UiElementId>(2)), missing);
            REQUIRE(recovered.HasValue());
            REQUIRE(recovered.Value().current.has_value());
            CHECK(recovered.Value().current->id == Stable<UiElementId>(2));

            const auto beforeFailure = graph.CurrentFocus();
            REQUIRE(beforeFailure.HasValue());
            auto malformed = missing;
            malformed[2].id = malformed[1].id;
            ExpectError(graph.Reload(Descriptor(Context(73, 6), Stable<UiElementId>(2)), malformed), UiErrors::FocusInvalid);
            const auto afterFailure = graph.CurrentFocus();
            REQUIRE(afterFailure.HasValue());
            CHECK(afterFailure.Value() == beforeFailure.Value());

            ExpectError(graph.Reload(Descriptor(Context(73, 4), Stable<UiElementId>(2)), missing), UiErrors::FocusSourceStale);
        }

        TEST_CASE("Focus graph closes admission and releases retained candidate state", "[runtime_ui][focus][lifecycle]") {
            const UiOwnershipGeneration owner = Owner();
            auto nodes = BasicNodes(owner);
            auto graph = CreateGraph(Descriptor(Context(), Stable<UiElementId>(2)), nodes);
            REQUIRE(graph.BeginRetirement().HasValue());
            CHECK(graph.State() == UiFocusGraphState::Retiring);
            ExpectError(graph.CurrentFocus(), UiErrors::FocusLifecycleUnavailable);
            ExpectError(graph.SetFocus(nodes[1].element), UiErrors::FocusLifecycleUnavailable);
            graph.Shutdown();
            graph.Shutdown();
            CHECK(graph.State() == UiFocusGraphState::Stopped);
            ExpectError(graph.Snapshot(), UiErrors::FocusLifecycleUnavailable);
        }

        TEST_CASE("Focus graph rejects malformed and over-capacity candidates", "[runtime_ui][focus][validation]") {
            const UiOwnershipGeneration owner = Owner();
            auto nodes = BasicNodes(owner);
            auto descriptor = Descriptor(Context(), Stable<UiElementId>(2));
            descriptor.nodeCapacity = 2;
            ExpectError(UiFocusGraph::Create(descriptor, nodes), UiErrors::FocusCapacityExceeded);

            auto invalid = BasicNodes(owner);
            invalid[2].parent = Stable<UiElementId>(99);
            ExpectError(UiFocusGraph::Create(Descriptor(Context(), Stable<UiElementId>(2)), invalid), UiErrors::FocusInvalid);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
