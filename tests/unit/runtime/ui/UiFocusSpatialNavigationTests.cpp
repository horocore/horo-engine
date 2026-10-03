#include "Horo/Runtime/Ui/UiFocusGraph.h"
#include "Horo/Runtime/Ui/UiLayout.h"
#include "UiTestUtils.h"
#include "support/AllocationProbe.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Runtime::Ui {
    namespace {
        using Test::Stable;
        using enum UiNavigationDirection;

        template <typename T> T Revision(const std::uint64_t value = 1) {
            return T::Create(value).Value();
        }

        class SpatialEvaluator final : public UiLayoutEvaluator {
        public:
            std::array<UiLogicalRect, 8> boxes{};

            Result<void> ResolveChildConstraints(const UiLayoutChildConstraintRequest &request,
                                                 const std::span<UiLayoutConstraints> output) const override {
                std::ranges::fill(output, request.constraints);
                return Result<void>::Success();
            }

            Result<UiLayoutMeasurement> Measure(const UiLayoutMeasureRequest &) const override {
                return Result<UiLayoutMeasurement>::Success({{10, 10}, false, false});
            }

            Result<UiLayoutArrangement> Arrange(const UiLayoutArrangeRequest &request,
                                                const std::span<UiLogicalRect> childContent) const override {
                for (std::size_t index = 0; index < request.children.size(); ++index)
                    childContent[index] = boxes[request.children[index].element.slot - 1];
                const auto box = boxes[request.element.slot - 1];
                return Result<UiLayoutArrangement>::Success({box, box, box, box, box, box, NoUiBaseline});
            }
        };

        struct Fixture final {
            UiOwnershipGeneration ownership = UiOwnershipGeneration::Create(741).Value();
            UiFocusOwnerContext owner{{ownership, 1, 1},
                                      {ownership, 2, 1},
                                      Stable<UiDocumentId>(90),
                                      Revision<UiDocumentRevision>(),
                                      Revision<UiRuntimeTreeRevision>(),
                                      Revision<UiInteractionRevision>(),
                                      {std::nullopt, {ownership, 3, 1}}};
            UiElementSlotAllocator slots = std::move(UiElementSlotAllocator::Create(ownership)).Value();
            std::array<UiElementDescriptor, 6> elements{UiElementDescriptor{Stable<UiElementId>(1), {}},
                                                        UiElementDescriptor{Stable<UiElementId>(2), Stable<UiElementId>(1)},
                                                        UiElementDescriptor{Stable<UiElementId>(3), Stable<UiElementId>(1)},
                                                        UiElementDescriptor{Stable<UiElementId>(4), Stable<UiElementId>(1)},
                                                        UiElementDescriptor{Stable<UiElementId>(5), Stable<UiElementId>(4)},
                                                        UiElementDescriptor{Stable<UiElementId>(6), Stable<UiElementId>(4)}};
            UiElementTree tree = std::move(UiElementTree::Create(slots,
                                                                 {owner.instance,
                                                                  owner.canvas,
                                                                  owner.document,
                                                                  owner.documentRevision,
                                                                  owner.treeRevision,
                                                                  {8, 8, 8}},
                                                                 elements))
                                     .Value();
            UiLayoutEngine layout =
                std::move(UiLayoutEngine::Create({owner.instance, owner.canvas, owner.document, 8, 8, 3, owner.interaction})).Value();
            SpatialEvaluator evaluator;
            std::array<UiFocusNodeDescriptor, 6> nodes{};
            UiFocusGraphDescriptor descriptor{owner, Stable<UiElementId>(2), UiFocusRecoveryPolicy::AncestorThenDefaultThenFirst, 8, 4, 4};

            Fixture() {
                for (std::size_t index = 0; index < nodes.size(); ++index) {
                    nodes[index].id = elements[index].id;
                    nodes[index].parent = elements[index].parent;
                    nodes[index].element = tree.Find(elements[index].id).Value();
                    nodes[index].focusable = index != 0 && index != 3;
                    evaluator.boxes[index] = {{static_cast<std::int32_t>(index * 100), 0}, {20, 20}};
                }
            }

            UiFocusGraph Graph() const {
                auto result = UiFocusGraph::Create(descriptor, nodes);
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            Result<UiLayoutSnapshot> Publish(const std::uint64_t content = 1) {
                return layout.Update(tree, {{owner.documentRevision, owner.treeRevision, Revision<UiLayoutContentRevision>(content),
                                             Revision<UiLayoutStyleRevision>(), Revision<UiLayoutIntrinsicRevision>(),
                                             Revision<UiLayoutCanvasRevision>(), Revision<UiLayoutPolicyRevision>()},
                                            {{0, 0}, {1000, 1000}},
                                            {{0, 0}, {1000, 1000}},
                                            &evaluator});
            }

            void Apply(UiFocusGraph &graph, const std::uint64_t content = 1) {
                auto snapshot = Publish(content);
                REQUIRE(snapshot.HasValue());
                const auto before = ::Horo::Tests::AllocationProbe::Count();
                const auto updated = graph.UpdateLayout(snapshot.Value());
                const auto after = ::Horo::Tests::AllocationProbe::Count();
                REQUIRE(updated.HasValue());
                CHECK(after == before);
            }

            void MovesTo(UiFocusGraph &graph, const UiNavigationDirection direction, const std::size_t index) const {
                const auto before = ::Horo::Tests::AllocationProbe::Count();
                const auto moved = graph.Move(direction);
                const auto after = ::Horo::Tests::AllocationProbe::Count();
                REQUIRE(moved.HasValue());
                CHECK(after == before);
                REQUIRE(moved.Value().current.has_value());
                CHECK(moved.Value().current->element == nodes[index].element);
                CHECK(moved.Value().IsValid());
            }
        };

        TEST_CASE("Spatial focus ranks actual layout beams and stable identities", "[runtime_ui][focus][spatial]") {
            Fixture f;
            f.evaluator.boxes[1] = {{0, 0}, {20, 20}};
            f.evaluator.boxes[2] = {{100, 0}, {20, 20}};
            f.evaluator.boxes[4] = f.evaluator.boxes[2];
            f.evaluator.boxes[5] = {{30, 100}, {20, 20}};
            auto graph = f.Graph();
            f.Apply(graph);
            f.MovesTo(graph, Right, 2);
            f.MovesTo(graph, Left, 1);
            std::swap(f.nodes[2], f.nodes[4]);
            f.descriptor.owner.interaction = Revision<UiInteractionRevision>(2);
            REQUIRE(graph.Reload(f.descriptor, f.nodes).HasValue());
            CHECK(graph.Move(Right).Value().kind == UiFocusChangeKind::NoTarget);
            f.Apply(graph, 2);
            f.MovesTo(graph, Right, 4);
            CHECK(graph.Move(Next).Value().kind == UiFocusChangeKind::NoTarget);
        }

        TEST_CASE("Spatial focus updates moving layout and returns current bring-into-view evidence", "[runtime_ui][focus][spatial]") {
            Fixture f;
            auto graph = f.Graph();
            auto old = f.Publish();
            REQUIRE(old.HasValue());
            REQUIRE(graph.UpdateLayout(old.Value()).HasValue());
            f.MovesTo(graph, Right, 2);
            REQUIRE(graph.SetFocus(f.nodes[1].element).HasValue());
            f.evaluator.boxes[2].origin.x = 700;
            f.Apply(graph, 2);
            f.MovesTo(graph, Right, 4);
            CHECK(graph.Owner().interaction == Revision<UiInteractionRevision>(2));
            const auto stale = graph.UpdateLayout(old.Value());
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == UiErrors::FocusSourceStale.code.Value());
            REQUIRE(graph.SetFocus(f.nodes[1].element).HasValue());
            const auto moved = graph.Move(Right).Value();
            REQUIRE(moved.bringIntoView.has_value());
            CHECK(moved.bringIntoView->owner.interaction == graph.Owner().interaction);
            CHECK(moved.current->id == f.nodes[4].id);
        }

        TEST_CASE("Spatial focus honors explicit overrides and typed axis wrap", "[runtime_ui][focus][spatial]") {
            Fixture f;
            f.descriptor.wrap = UiFocusWrapPolicy::Horizontal;
            f.nodes[1].links.targets[5] = f.nodes[5].id;
            auto graph = f.Graph();
            f.Apply(graph);
            f.MovesTo(graph, Right, 5);
            f.MovesTo(graph, Right, 1);
            CHECK(graph.Move(Up).Value().kind == UiFocusChangeKind::NoTarget);
            f.descriptor.owner.interaction = Revision<UiInteractionRevision>(2);
            f.descriptor.wrap = UiFocusWrapPolicy::Vertical;
            f.nodes[1].links.targets[5] = {};
            f.evaluator.boxes[5].origin.y = 100;
            REQUIRE(graph.Reload(f.descriptor, f.nodes).HasValue());
            f.Apply(graph, 2);
            REQUIRE(graph.SetFocus(f.nodes[5].element).HasValue());
            f.MovesTo(graph, Down, 4);
            CHECK(graph.Move(Left).HasValue());
            f.descriptor.wrap = UiFocusWrapPolicy::Count;
            CHECK_FALSE(f.descriptor.IsValid());
        }

        TEST_CASE("Spatial focus excludes hidden disabled empty and recycled geometry", "[runtime_ui][focus][spatial]") {
            Fixture f;
            f.nodes[2].enabled = false;
            f.nodes[3].visible = false;
            auto graph = f.Graph();
            f.Apply(graph);
            CHECK(graph.Move(Right).Value().kind == UiFocusChangeKind::NoTarget);
            f.nodes[2].enabled = true;
            ++f.nodes[2].element.generation;
            f.nodes[3].visible = true;
            f.evaluator.boxes[4].extent.width = 0;
            f.evaluator.boxes[5].extent.height = 0;
            f.descriptor.owner.interaction = Revision<UiInteractionRevision>(2);
            REQUIRE(graph.Reload(f.descriptor, f.nodes).HasValue());
            f.Apply(graph, 2);
            CHECK(graph.Move(Right).Value().kind == UiFocusChangeKind::NoTarget);
            CHECK(graph.SetFocus({f.ownership, f.nodes[2].element.slot, 1}).HasError());
        }

        TEST_CASE("Spatial focus traps the active modal including wrap and overrides", "[runtime_ui][focus][spatial][modal]") {
            Fixture f;
            f.descriptor.wrap = UiFocusWrapPolicy::Both;
            f.nodes[4].links.targets[4] = f.nodes[1].id;
            auto graph = f.Graph();
            f.Apply(graph);
            auto modal = graph.PushModal({f.nodes[3].element, f.nodes[3].id, f.nodes[4].id});
            REQUIRE(modal.HasValue());
            CHECK(graph.Move(Left).Value().kind == UiFocusChangeKind::NoTarget);
            f.MovesTo(graph, Right, 5);
            f.MovesTo(graph, Right, 4);
            REQUIRE(graph.PopModal(modal.Value().modal).HasValue());
            CHECK(graph.CurrentFocus().Value()->id == f.nodes[1].id);
            f.MovesTo(graph, Right, 2);
        }

        TEST_CASE("Spatial focus preserves geometry on invalid publication and foreign layout", "[runtime_ui][focus][spatial]") {
            Fixture f;
            auto graph = f.Graph();
            f.Apply(graph);
            f.evaluator.boxes[2].extent.width = -1;
            CHECK(f.Publish(2).HasError());
            f.MovesTo(graph, Right, 2);
            Fixture foreign;
            auto foreignDescriptor = foreign.descriptor;
            foreignDescriptor.owner.canvas.generation = 2;
            auto foreignGraph = std::move(UiFocusGraph::Create(foreignDescriptor, foreign.nodes)).Value();
            auto snapshot = foreign.Publish();
            REQUIRE(snapshot.HasValue());
            REQUIRE(foreignGraph.UpdateLayout(snapshot.Value()).HasError());
            CHECK(foreignGraph.UpdateLayout(snapshot.Value()).ErrorValue().code.Value() == UiErrors::FocusScopeMismatch.code.Value());
            graph.Shutdown();
            CHECK(graph.UpdateLayout(snapshot.Value()).ErrorValue().code.Value() == UiErrors::FocusLifecycleUnavailable.code.Value());
            CHECK(graph.Move(Right).HasError());
        }

        TEST_CASE("Spatial focus handles signed coordinate extremes and empty scope reload", "[runtime_ui][focus][spatial]") {
            Fixture f;
            f.evaluator.boxes[1] = {{std::numeric_limits<std::int32_t>::min(), 0}, {20, 20}};
            f.evaluator.boxes[2] = {{std::numeric_limits<std::int32_t>::max() - 20, 0}, {20, 20}};
            f.nodes[3].enabled = false;
            auto graph = f.Graph();
            f.Apply(graph);
            f.MovesTo(graph, Right, 2);
            f.MovesTo(graph, Left, 1);
            f.descriptor.owner.interaction = Revision<UiInteractionRevision>(2);
            REQUIRE(graph.Reload(f.descriptor, {}).HasValue());
            f.Apply(graph, 2);
            CHECK(graph.Move(Right).Value().kind == UiFocusChangeKind::NoTarget);
            auto empty = UiFocusGraph::Create(f.descriptor, {});
            REQUIRE(empty.HasValue());
            CHECK_FALSE(empty.Value().CurrentFocus().Value().has_value());
        }

        TEST_CASE("Spatial participation changes are fenced and allocation-free", "[runtime_ui][focus][spatial]") {
            Fixture f;
            auto graph = f.Graph();
            f.Apply(graph);
            const auto before = ::Horo::Tests::AllocationProbe::Count();
            const auto changed = graph.SetParticipation(graph.Owner(), {f.nodes[2].element, true, false, true});
            const auto after = ::Horo::Tests::AllocationProbe::Count();
            REQUIRE(changed.HasValue());
            CHECK(after == before);
            f.MovesTo(graph, Right, 4);
            const auto hidden = graph.SetParticipation(graph.Owner(), {f.nodes[3].element, false, true, false});
            REQUIRE(hidden.HasValue());
            REQUIRE(hidden.Value().current.has_value());
            CHECK(hidden.Value().current->id == f.nodes[1].id);
            CHECK(hidden.Value().kind == UiFocusChangeKind::FocusRecovered);
            CHECK(graph.Move(Right).Value().kind == UiFocusChangeKind::NoTarget);
            REQUIRE(graph.SetParticipation(graph.Owner(), {f.nodes[3].element, false, false, true}).HasValue());
            CHECK(graph.Move(Right).Value().kind == UiFocusChangeKind::NoTarget);
            auto stale = graph.Owner();
            f.Apply(graph, 2);
            CHECK(graph.SetParticipation(stale, {f.nodes[2].element}).ErrorValue().code.Value() == UiErrors::FocusSourceStale.code.Value());
            auto foreign = graph.Owner();
            foreign.scope.presentationLayer.generation = 2;
            CHECK(graph.SetParticipation(foreign, {f.nodes[2].element}).ErrorValue().code.Value() ==
                  UiErrors::FocusScopeMismatch.code.Value());
            auto recycled = f.nodes[2].element;
            ++recycled.generation;
            CHECK(graph.SetParticipation(graph.Owner(), {recycled}).ErrorValue().code.Value() ==
                  UiErrors::FocusTargetUnavailable.code.Value());
            REQUIRE(graph.SetParticipation(graph.Owner(), {f.nodes[2].element, false, true, true}).HasValue());
            CHECK(graph.Move(Right).Value().kind == UiFocusChangeKind::NoTarget);
            REQUIRE(graph.BeginRetirement().HasValue());
            CHECK(graph.SetParticipation(graph.Owner(), {f.nodes[2].element}).ErrorValue().code.Value() ==
                  UiErrors::FocusLifecycleUnavailable.code.Value());
        }

        TEST_CASE("Spatial focus covers vertical directions and preserves unavailable override behavior", "[runtime_ui][focus][spatial]") {
            Fixture f;
            f.evaluator.boxes[1] = {{0, 100}, {20, 20}};
            f.evaluator.boxes[2] = {{0, 200}, {20, 20}};
            f.evaluator.boxes[4] = {{0, 0}, {20, 20}};
            f.nodes[5].focusable = false;
            auto graph = f.Graph();
            f.Apply(graph);
            f.MovesTo(graph, Up, 4);
            f.MovesTo(graph, Down, 1);
            f.MovesTo(graph, Down, 2);
            auto edge = graph.Move(Down).Value();
            CHECK(edge.kind == UiFocusChangeKind::NoTarget);
            CHECK(edge.reason == UiFocusChangeReason::InvalidTarget);
            f.nodes[1].links.targets[3] = Stable<UiElementId>(99);
            f.descriptor.owner.interaction = Revision<UiInteractionRevision>(2);
            REQUIRE(graph.Reload(f.descriptor, f.nodes).HasValue());
            f.Apply(graph, 2);
            REQUIRE(graph.SetFocus(f.nodes[1].element).HasValue());
            edge = graph.Move(Down).Value();
            CHECK(edge.current->id == f.nodes[1].id);
            CHECK(edge.reason == UiFocusChangeReason::InvalidTarget);
            CHECK(graph.Move(static_cast<UiNavigationDirection>(255)).HasError());
        }

        TEST_CASE("Spatial layout rejects mismatched document and tree revisions without mutation", "[runtime_ui][focus][spatial]") {
            Fixture f;
            auto graph = f.Graph();
            f.Apply(graph);
            auto snapshot = f.Publish();
            REQUIRE(snapshot.HasValue());
            auto wrong = f.descriptor;
            wrong.owner.documentRevision = Revision<UiDocumentRevision>(2);
            auto wrongDocument = std::move(UiFocusGraph::Create(wrong, f.nodes)).Value();
            CHECK(wrongDocument.UpdateLayout(snapshot.Value()).ErrorValue().code.Value() == UiErrors::FocusSourceStale.code.Value());
            wrong = f.descriptor;
            wrong.owner.treeRevision = Revision<UiRuntimeTreeRevision>(2);
            auto wrongTree = std::move(UiFocusGraph::Create(wrong, f.nodes)).Value();
            CHECK(wrongTree.UpdateLayout(snapshot.Value()).ErrorValue().code.Value() == UiErrors::FocusSourceStale.code.Value());
            auto invalid = f.nodes;
            invalid[2].id = invalid[1].id;
            wrong = f.descriptor;
            wrong.owner.interaction = Revision<UiInteractionRevision>(2);
            CHECK(graph.Reload(wrong, invalid).HasError());
            f.MovesTo(graph, Right, 2);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
