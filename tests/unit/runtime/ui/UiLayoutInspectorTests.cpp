#include "Horo/Runtime/Ui/UiLayoutInspector.h"
#include "UiHotReloadTestFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime::Ui {
    namespace {
        using ReloadTests::Generation;
        using ReloadTests::Revision;
        using ReloadTests::Stable;

        UiLayoutInspector Inspector(const UiReloadGeneration &generation, const std::uint32_t elements = 8) {
            auto result = UiLayoutInspector::Create(generation, Stable<UiCanvasId>(2), {elements, 8, 8, 2});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        TEST_CASE("Layout inspection copies actual hierarchy constraints dirty work clipping and selection", "[runtime_ui][inspection]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(ReloadTests::Owner())).Value();
            auto generation = Generation(allocator, 1);
            const auto &canvas = generation.Canvases().front();
            auto inspector = Inspector(generation);
            REQUIRE_FALSE(inspector.Latest().has_value());
            REQUIRE(inspector.Capture(canvas.tree, *canvas.layout, &*canvas.clipped).status == UiLayoutInspectionStatus::Disabled);
            auto table = UiPresentationProfileTable::Create(generation, canvas.id, Revision<UiPresentationProfileRevision>(1), {});
            REQUIRE(table.HasValue());
            const UiPresentationProfileEvidence evidence{canvas.id,
                                                         canvas.tree.Instance(),
                                                         canvas.tree.Canvas(),
                                                         canvas.tree.Revision(),
                                                         {ReloadTests::Owner(), 20, 1},
                                                         Revision<UiPresentationEvidenceRevision>(1),
                                                         {100, 100}};
            const auto selection = table.Value().Select(canvas.tree, evidence);
            REQUIRE(selection.HasValue());
            inspector.Enable(true);
            REQUIRE(inspector.Capture(canvas.tree, *canvas.layout, &*canvas.clipped, &selection.Value()).status ==
                    UiLayoutInspectionStatus::Captured);
            const auto snapshot = inspector.Latest();
            REQUIRE(snapshot.has_value());
            REQUIRE(snapshot->Descriptor().layout.sources == canvas.layout->Descriptor().sources);
            REQUIRE(snapshot->Descriptor().selection == selection.Value());
            REQUIRE(snapshot->Descriptor().clipping);
            REQUIRE(snapshot->Records().size() == canvas.layout->Records().size());
            REQUIRE(snapshot->Records().front().id == canvas.tree.Root().Value().id);
            REQUIRE_FALSE(snapshot->Records().front().parent.IsValid());
            REQUIRE(snapshot->Records().back().parent == canvas.tree.Root().Value().handle);
            REQUIRE(snapshot->Records().back().layout.constraints == canvas.layout->Records().back().constraints);
            REQUIRE(snapshot->Records().back().layout.work == canvas.layout->Records().back().work);
            REQUIRE(snapshot->Clipping().size() == canvas.clipped->Records().size());
            REQUIRE(snapshot->Clips().size() == canvas.clipped->Clips().size());
            REQUIRE(snapshot->Scrolls().size() == canvas.clipped->Scrolls().size());
            REQUIRE(snapshot->Scrolls().front() == canvas.clipped->Scrolls().front());
        }

        TEST_CASE("Inspection slot exhaustion drops only optional copied evidence", "[runtime_ui][inspection]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(ReloadTests::Owner())).Value();
            auto generation = Generation(allocator, 1);
            auto &canvas = *generation.Canvas(Stable<UiCanvasId>(2));
            auto inspector = Inspector(generation);
            inspector.Enable(true);
            REQUIRE(inspector.Capture(canvas.tree, *canvas.layout).status == UiLayoutInspectionStatus::Captured);
            auto first = inspector.Latest();
            REQUIRE(inspector.Capture(canvas.tree, *canvas.layout).status == UiLayoutInspectionStatus::Captured);
            const auto second = inspector.Latest();
            REQUIRE(inspector.Capture(canvas.tree, *canvas.layout).status == UiLayoutInspectionStatus::DroppedStorage);
            std::array descriptors{UiLayoutElementDescriptor{canvas.layout->Records()[0].element, {}, {}},
                                   UiLayoutElementDescriptor{canvas.layout->Records()[1].element, {}, {}}};
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors);
            REQUIRE(evaluator.HasValue());
            auto sources = canvas.layout->Descriptor().sources;
            sources.content = Revision<UiLayoutContentRevision>(2);
            auto updated =
                canvas.layoutEngine->Update(canvas.tree, {sources, {{0, 0}, {100, 100}}, {{0, 0}, {100, 100}}, &evaluator.Value()});
            REQUIRE(updated.HasValue());
            REQUIRE(updated.Value().Descriptor().interaction > first->Descriptor().layout.interaction);
            REQUIRE(inspector.Capture(canvas.tree, updated.Value()).status == UiLayoutInspectionStatus::DroppedStorage);
            first.reset();
            REQUIRE(inspector.Capture(canvas.tree, updated.Value()).status == UiLayoutInspectionStatus::Captured);
            REQUIRE(second->Descriptor().layout.interaction == Revision<UiInteractionRevision>(1));
        }

        TEST_CASE("Inspection rejects cross-generation component lineage without replacing latest", "[runtime_ui][inspection]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(ReloadTests::Owner())).Value();
            auto first = Generation(allocator, 1);
            auto second = Generation(allocator, 2);
            const auto &canvas = first.Canvases().front();
            const auto &replacement = second.Canvases().front();
            auto inspector = Inspector(first);
            inspector.Enable(true);
            REQUIRE(inspector.Capture(canvas.tree, *canvas.layout, &*canvas.clipped).status == UiLayoutInspectionStatus::Captured);
            const auto before = inspector.Latest();
            auto rebuilt = Generation(allocator, 1);
            const auto &sameVersion = rebuilt.Canvases().front();
            REQUIRE(sameVersion.tree.Root().Value().handle != canvas.tree.Root().Value().handle);
            REQUIRE(inspector.Capture(sameVersion.tree, *sameVersion.layout).rejection == UiLayoutInspectionRejection::Tree);
            auto foreignAllocator = std::move(UiElementSlotAllocator::Create(ReloadTests::Owner())).Value();
            auto foreign = Generation(foreignAllocator, 1);
            const auto &foreignCanvas = foreign.Canvases().front();
            REQUIRE(foreignCanvas.tree.Root().Value().handle == canvas.tree.Root().Value().handle);
            REQUIRE(inspector.Capture(foreignCanvas.tree, *foreignCanvas.layout).rejection == UiLayoutInspectionRejection::Tree);
            REQUIRE(inspector.Capture(replacement.tree, *replacement.layout).rejection == UiLayoutInspectionRejection::Tree);
            REQUIRE(inspector.Capture(canvas.tree, *replacement.layout).rejection == UiLayoutInspectionRejection::LayoutLineage);
            REQUIRE(inspector.Capture(canvas.tree, *canvas.layout, &*replacement.clipped).rejection ==
                    UiLayoutInspectionRejection::ClipLineage);
            UiPresentationProfileSelection malformed;
            REQUIRE(inspector.Capture(canvas.tree, *canvas.layout, nullptr, &malformed).rejection ==
                    UiLayoutInspectionRejection::SelectionLineage);
            REQUIRE(inspector.Latest()->Descriptor().layout.interaction == before->Descriptor().layout.interaction);
            auto tooSmall = Inspector(first, 1);
            tooSmall.Enable(true);
            REQUIRE(tooSmall.Capture(canvas.tree, *canvas.layout).status == UiLayoutInspectionStatus::DroppedCapacity);
            REQUIRE_FALSE(tooSmall.Latest().has_value());
        }

        TEST_CASE("Copied inspection remains readable after actual source generation destruction", "[runtime_ui][inspection]") {
            std::optional<UiLayoutInspectionSnapshot> retained;
            UiElementHandle capturedRoot;
            UiLayoutWork capturedWork;
            {
                auto allocator = std::move(UiElementSlotAllocator::Create(ReloadTests::Owner())).Value();
                auto generation = Generation(allocator, 1);
                const auto &canvas = generation.Canvases().front();
                auto inspector = Inspector(generation);
                inspector.Enable(true);
                REQUIRE(inspector.Capture(canvas.tree, *canvas.layout, &*canvas.clipped).status == UiLayoutInspectionStatus::Captured);
                retained = inspector.Latest();
                capturedRoot = canvas.tree.Root().Value().handle;
                capturedWork = canvas.layout->Records().front().work;
            }
            REQUIRE(retained.has_value());
            REQUIRE(retained->Descriptor().layout.sources.document == Revision<UiDocumentRevision>(1));
            REQUIRE(retained->Records().front().layout.element == capturedRoot);
            REQUIRE(retained->Records().front().layout.work == capturedWork);
            REQUIRE(retained->Descriptor().clipping);
            REQUIRE_FALSE(retained->Clipping().empty());
            REQUIRE_FALSE(retained->Scrolls().empty());
        }

        TEST_CASE("Inspection rebind is transactional and retained copies survive reload and shutdown", "[runtime_ui][inspection]") {
            auto allocator = std::move(UiElementSlotAllocator::Create(ReloadTests::Owner())).Value();
            auto first = Generation(allocator, 1);
            auto second = Generation(allocator, 2);
            const auto &canvas = first.Canvases().front();
            const auto &replacement = second.Canvases().front();
            std::optional<UiLayoutInspectionSnapshot> retained;
            {
                auto inspector = Inspector(first);
                inspector.Enable(true);
                REQUIRE(inspector.Capture(canvas.tree, *canvas.layout).status == UiLayoutInspectionStatus::Captured);
                retained = inspector.Latest();
                REQUIRE(inspector.Rebind(second, Stable<UiCanvasId>(99)).HasError());
                REQUIRE(inspector.Capture(canvas.tree, *canvas.layout).status == UiLayoutInspectionStatus::Captured);
                REQUIRE(inspector.Rebind(second, Stable<UiCanvasId>(2)).HasValue());
                REQUIRE(inspector.Rebind(first, Stable<UiCanvasId>(2)).HasError());
                REQUIRE(inspector.Capture(replacement.tree, *replacement.layout).status == UiLayoutInspectionStatus::Captured);
                REQUIRE(inspector.Latest()->Descriptor().layout.sources.document == Revision<UiDocumentRevision>(2));
                inspector.Enable(false);
                REQUIRE(inspector.Capture(replacement.tree, *replacement.layout).status == UiLayoutInspectionStatus::Disabled);
                inspector.Shutdown();
                REQUIRE_FALSE(inspector.Latest().has_value());
                REQUIRE(inspector.Capture(replacement.tree, *replacement.layout).status == UiLayoutInspectionStatus::Stopped);
            }
            REQUIRE(retained->Descriptor().layout.sources.document == Revision<UiDocumentRevision>(1));
            REQUIRE(retained->Records().front().layout.element == canvas.tree.Root().Value().handle);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
