#include "Horo/Runtime/Ui/UiAccessibility.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiAccessibilityTestUtils.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>

void *operator new(const std::size_t size) {
    accessibilityAllocations.fetch_add(1, std::memory_order_relaxed);
    void *memory = std::malloc(size);
    if (memory == nullptr)
        throw std::bad_alloc{};
    return memory;
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, const std::size_t) noexcept {
    operator delete(memory);
}

namespace Horo::Runtime::Ui {
    namespace {
        TEST_CASE("Runtime UI accessibility snapshots own typed core and contributed semantics", "[runtime_ui][accessibility]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree);
            ProjectionFixture fixture;
            const auto projection = fixture.View();
            const auto snapshotResult = extractor.Extract(tree, Descriptor(tree), projection);
            REQUIRE(snapshotResult.HasValue());
            auto snapshot = std::move(snapshotResult).Value();

            REQUIRE(snapshot.Nodes().size() == 4);
            REQUIRE(snapshot.Nodes()[2].role == UiAccessibilityRole::Slider);
            REQUIRE(snapshot.Nodes()[2].source == UiAccessibilityControlSource::Core);
            REQUIRE(snapshot.Nodes()[3].source == UiAccessibilityControlSource::Contributed);
            REQUIRE(snapshot.Nodes()[3].contributor == Contributor(9));
            REQUIRE(snapshot.Text(snapshot.Nodes()[0].name) == "Main menu");
            REQUIRE(snapshot.Text(snapshot.Nodes()[2].description).empty());
            REQUIRE(snapshot.Nodes()[2].value.kind == UiAccessibilityValueKind::Number);
            REQUIRE(snapshot.Nodes()[2].range.current == 0.5);
            REQUIRE(snapshot.Nodes()[2].relationCount == 1);
            REQUIRE(snapshot.Nodes()[2].actionCount == 3);
            REQUIRE(snapshot.Relations(snapshot.Nodes()[2])[0].target == snapshot.Nodes()[1].id);
            REQUIRE(snapshot.Actions(snapshot.Nodes()[2])[2].argumentKind == UiAccessibilityActionValueKind::Number);
            REQUIRE(snapshot.Nodes()[2].parent == snapshot.Nodes()[0].id);
            REQUIRE(snapshot.Nodes()[3].parent == snapshot.Nodes()[0].id);

            const auto slider = snapshot.Find(Stable<UiElementId>(3));
            REQUIRE(slider.HasValue());
            REQUIRE(slider.Value() == snapshot.Nodes()[2].id);
            REQUIRE(snapshot.Get(slider.Value()).Value().elementId == Stable<UiElementId>(3));

            fixture.nodes[0].name = {"Mutated source", UiAccessibilityTextSource::ResolvedMessage};
            fixture.nodes[2].value.number = 0.9;
            REQUIRE(snapshot.Text(snapshot.Nodes()[0].name) == "Main menu");
            REQUIRE(snapshot.Nodes()[2].value.number == 0.5);
        }

        TEST_CASE("Runtime UI accessibility validation rejects malformed role state relations and actions",
                  "[runtime_ui][accessibility][validation]") {
            auto tree = MakeTree();
            ProjectionFixture fixture;

            SECTION("missing name and invalid role state") {
                fixture.nodes[3].name = {};
                fixture.nodes[3].relations = {};
                RequireError(MakeExtractor(tree).Extract(tree, Descriptor(tree), fixture.View()), UiErrors::AccessibilityNameMissing);

                fixture.nodes[3].name = {"Button", {}};
                fixture.nodes[3].state = UiAccessibilityState{UiAccessibilityStateFlag::Checked};
                RequireError(MakeExtractor(tree).Extract(tree, Descriptor(tree), fixture.View()), UiErrors::AccessibilityStateInvalid);
            }

            SECTION("invalid range selection and contributor ownership") {
                fixture.nodes[2].range.maximum = -1.0;
                RequireError(MakeExtractor(tree).Extract(tree, Descriptor(tree), fixture.View()), UiErrors::AccessibilityRangeInvalid);

                fixture.nodes[2].range = {0.0, 1.0, 0.5, 0.1};
                fixture.nodes[2].hasSelection = true;
                fixture.nodes[2].selection = {UiAccessibilitySelectionMode::Single, true, 3, 2};
                RequireError(MakeExtractor(tree).Extract(tree, Descriptor(tree), fixture.View()), UiErrors::AccessibilitySelectionInvalid);

                fixture.nodes[2].hasSelection = false;
                fixture.nodes[3].contributor = {};
                RequireError(MakeExtractor(tree).Extract(tree, Descriptor(tree), fixture.View()),
                             UiErrors::AccessibilityContributorInvalid);
            }

            SECTION("dangling duplicate and cyclic relationships") {
                const std::array dangling{UiAccessibilityRelationInput{UiAccessibilityRelationKind::DescribedBy, Stable<UiElementId>(99)}};
                fixture.nodes[2].relations = dangling;
                RequireError(MakeExtractor(tree).Extract(tree, Descriptor(tree), fixture.View()), UiErrors::AccessibilityRelationInvalid);

                const std::array duplicate{UiAccessibilityRelationInput{UiAccessibilityRelationKind::LabelledBy, Stable<UiElementId>(2)},
                                           UiAccessibilityRelationInput{UiAccessibilityRelationKind::LabelledBy, Stable<UiElementId>(2)}};
                fixture.nodes[2].relations = duplicate;
                RequireError(MakeExtractor(tree).Extract(tree, Descriptor(tree), fixture.View()), UiErrors::AccessibilityRelationInvalid);

                std::array<UiAccessibilityRelationInput, 1> cycleToSlider{
                    UiAccessibilityRelationInput{UiAccessibilityRelationKind::DescribedBy, Stable<UiElementId>(3)}};
                fixture.nodes[1].relations = cycleToSlider;
                const std::array cycleToLabel{
                    UiAccessibilityRelationInput{UiAccessibilityRelationKind::DescribedBy, Stable<UiElementId>(2)}};
                fixture.nodes[2].relations = cycleToLabel;
                RequireError(MakeExtractor(tree).Extract(tree, Descriptor(tree), fixture.View()), UiErrors::AccessibilityRelationInvalid);
            }

            SECTION("conflicting focus and action declaration") {
                fixture.nodes[0].state = UiAccessibilityState{UiAccessibilityStateFlag::Focusable | UiAccessibilityStateFlag::Focused};
                fixture.nodes[2].state = UiAccessibilityState{UiAccessibilityStateFlag::Focusable | UiAccessibilityStateFlag::Focused};
                RequireError(MakeExtractor(tree).Extract(tree, Descriptor(tree), fixture.View()), UiErrors::AccessibilityFocusConflict);

                fixture.nodes[0].state = {};
                fixture.nodes[2].state = UiAccessibilityState{UiAccessibilityStateFlag::Focusable};
                fixture.sliderActions[0].id = Action(2);
                RequireError(MakeExtractor(tree).Extract(tree, Descriptor(tree), fixture.View()), UiErrors::AccessibilityActionInvalid);
            }
        }

        TEST_CASE("Runtime UI accessibility action requests are revision checked and typed", "[runtime_ui][accessibility][actions]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree);
            ProjectionFixture fixture;
            const auto snapshot = std::move(extractor.Extract(tree, Descriptor(tree), fixture.View())).Value();
            const auto slider = snapshot.Find(Stable<UiElementId>(3)).Value();

            const UiAccessibilityActionRequest increment{snapshot.Descriptor().instance,
                                                         snapshot.Descriptor().canvas,
                                                         snapshot.Descriptor().document,
                                                         snapshot.Descriptor().documentRevision,
                                                         snapshot.Descriptor().treeRevision,
                                                         snapshot.Descriptor().interactionRevision,
                                                         snapshot.Descriptor().semanticRevision,
                                                         slider,
                                                         Action(1),
                                                         {}};
            REQUIRE(ValidateUiAccessibilityActionRequest(snapshot, increment).HasValue());

            auto setValue = increment;
            setValue.action = Action(3);
            setValue.argument = {UiAccessibilityValueKind::Number, false, 0, 0.75, {}};
            REQUIRE(ValidateUiAccessibilityActionRequest(snapshot, setValue).HasValue());

            setValue.argument = {UiAccessibilityValueKind::Text, false, 0, 0.0, {"wrong", {}}};
            RequireError(ValidateUiAccessibilityActionRequest(snapshot, setValue), UiErrors::AccessibilityActionRejected);

            auto stale = increment;
            stale.semanticRevision = UiAccessibilitySemanticRevision::Create(99).Value();
            RequireError(ValidateUiAccessibilityActionRequest(snapshot, stale), UiErrors::AccessibilityActionStale);

            stale.semanticRevision = snapshot.Descriptor().semanticRevision;
            stale.action = Action(99);
            RequireError(ValidateUiAccessibilityActionRequest(snapshot, stale), UiErrors::AccessibilityActionStale);
        }

        TEST_CASE("Runtime UI accessibility reload and shutdown retain last-good leases", "[runtime_ui][accessibility][lifecycle]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree, 2);
            ProjectionFixture fixture;
            auto firstResult = extractor.Extract(tree, Descriptor(tree, 1), fixture.View());
            REQUIRE(firstResult.HasValue());
            std::optional<UiAccessibilitySnapshot> first{std::move(firstResult).Value()};

            auto stale = extractor.Extract(tree, Descriptor(tree, 1), fixture.View());
            RequireError(stale, UiErrors::AccessibilitySnapshotSourceStale);

            auto commandsResult =
                UiStructuralCommandBuffer::Create(tree.Instance(), tree.Canvas(), tree.SourceDocumentRevision(), tree.Revision(), 2);
            REQUIRE(commandsResult.HasValue());
            auto commands = std::move(commandsResult).Value();
            REQUIRE(commands.Add(UiInsertElementCommand{Stable<UiElementId>(6), tree.Root().Value().handle, 0}).HasValue());
            REQUIRE(tree.CommitDeferred(commands, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
            auto reloadedResult = extractor.Extract(tree, Descriptor(tree, 2), fixture.View());
            REQUIRE(reloadedResult.HasValue());
            std::optional<UiAccessibilitySnapshot> reloaded{std::move(reloadedResult).Value()};
            REQUIRE(reloaded->Descriptor().treeRevision == tree.Revision());
            REQUIRE(first->Descriptor().treeRevision != reloaded->Descriptor().treeRevision);

            extractor.Close();
            REQUIRE(extractor.State() == UiAccessibilityExtractorState::Closed);
            RequireError(extractor.Extract(tree, Descriptor(tree, 3), fixture.View()), UiErrors::AccessibilityLifecycleUnavailable);
            REQUIRE(!extractor.IsDrained());
            reloaded.reset();
            first.reset();
            REQUIRE(extractor.IsDrained());
            extractor.Close();
        }

        TEST_CASE("Runtime UI accessibility extraction uses only preallocated frame-hot storage",
                  "[runtime_ui][accessibility][performance]") {
            auto tree = MakeTree();
            auto extractor = MakeExtractor(tree);
            ProjectionFixture fixture;
            accessibilityAllocations.store(0, std::memory_order_relaxed);
            const auto snapshot = extractor.Extract(tree, Descriptor(tree), fixture.View());
            REQUIRE(snapshot.HasValue());
            REQUIRE(accessibilityAllocations.load(std::memory_order_relaxed) == 0);
        }

    }  // namespace
}  // namespace Horo::Runtime::Ui
