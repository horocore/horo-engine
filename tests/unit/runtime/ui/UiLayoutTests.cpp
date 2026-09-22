#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiLayout.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>

namespace {
    std::atomic<std::size_t> layoutAllocations{};

    void FreeLayoutAllocation(void *memory) noexcept {
        std::free(memory);
    }
}  // namespace

void *operator new(const std::size_t size) {
    layoutAllocations.fetch_add(1, std::memory_order_relaxed);
    void *memory = std::malloc(size);
    return memory != nullptr ? memory : throw std::bad_alloc{};
}

void operator delete(void *memory) noexcept {
    FreeLayoutAllocation(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    FreeLayoutAllocation(memory);
}

namespace Horo::Runtime::Ui {
    namespace {
        SerializedUiId StableBytes(const std::uint8_t marker) {
            SerializedUiId bytes{};
            *bytes.rbegin() = marker;
            return bytes;
        }

        template <typename Id> Id Stable(const std::uint8_t marker) {
            return Id::Create(StableBytes(marker)).Value();
        }

        UiOwnershipGeneration Owner(const std::uint64_t value = 31) {
            return UiOwnershipGeneration::Create(value).Value();
        }

        template <typename Revision> Revision Rev(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        UiElementTreeDescriptor TreeDescriptor(const UiRuntimeTreeRevision revision = Rev<UiRuntimeTreeRevision>(3)) {
            return {.instance = {Owner(), 1, 1},
                    .canvas = {Owner(), 2, 1},
                    .document = Stable<UiDocumentId>(1),
                    .documentRevision = Rev<UiDocumentRevision>(2),
                    .treeRevision = revision,
                    .limits = {8, 8, 8}};
        }

        const std::array<UiElementDescriptor, 4> &Elements() {
            static const std::array elements{
                UiElementDescriptor{Stable<UiElementId>(1), {}},
                UiElementDescriptor{Stable<UiElementId>(2), Stable<UiElementId>(1)},
                UiElementDescriptor{Stable<UiElementId>(3), Stable<UiElementId>(2)},
                UiElementDescriptor{Stable<UiElementId>(4), Stable<UiElementId>(1)},
            };
            return elements;
        }

        UiElementTree Tree() {
            auto allocatorResult = UiElementSlotAllocator::Create(Owner());
            REQUIRE(allocatorResult.HasValue());
            auto allocator = std::move(allocatorResult).Value();
            auto result = UiElementTree::Create(allocator, TreeDescriptor(), Elements());
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        UiLayoutEngine Engine(const std::uint32_t snapshots = 3) {
            const auto tree = TreeDescriptor();
            auto result = UiLayoutEngine::Create(
                {tree.instance, tree.canvas, tree.document, tree.limits.elements, 8, snapshots, Rev<UiInteractionRevision>(1)});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        UiElementTree FlatTree() {
            auto allocatorResult = UiElementSlotAllocator::Create(Owner());
            REQUIRE(allocatorResult.HasValue());
            auto allocator = std::move(allocatorResult).Value();
            const std::array elements{
                UiElementDescriptor{Stable<UiElementId>(1), {}},
                UiElementDescriptor{Stable<UiElementId>(2), Stable<UiElementId>(1)},
                UiElementDescriptor{Stable<UiElementId>(3), Stable<UiElementId>(1)},
                UiElementDescriptor{Stable<UiElementId>(4), Stable<UiElementId>(1)},
            };
            auto result = UiElementTree::Create(allocator, TreeDescriptor(), elements);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        UiLayoutSourceRevisions Sources(const std::uint64_t content = 1) {
            return {Rev<UiDocumentRevision>(2),    Rev<UiRuntimeTreeRevision>(3),     Rev<UiLayoutContentRevision>(content),
                    Rev<UiLayoutStyleRevision>(1), Rev<UiLayoutIntrinsicRevision>(1), Rev<UiLayoutCanvasRevision>(1),
                    Rev<UiLayoutPolicyRevision>(1)};
        }

        UiLayoutArrangement Arrangement(const UiLogicalRect rect) {
            return {rect, rect, rect, rect, rect, rect, NoUiBaseline};
        }

        class CountingEvaluator : public UiLayoutEvaluator {
        public:
            mutable std::array<std::uint32_t, 32> constraints{};
            mutable std::array<std::uint32_t, 32> measures{};
            mutable std::array<std::uint32_t, 32> arranges{};
            mutable bool failMeasure{};
            mutable bool alternateAssignments{};
            mutable UiCanvasScaleFactor lastFontScale{};

            Result<void> ResolveChildConstraints(const UiLayoutChildConstraintRequest &request,
                                                 const std::span<UiLayoutConstraints> output) const override {
                ++constraints[request.element.slot];
                if (output.size() != request.children.size())
                    return Result<void>::Failure(MakeError(UiErrors::LayoutInvalid));
                for (auto &item : output)
                    item = request.constraints;
                return Result<void>::Success();
            }

            Result<UiLayoutMeasurement> Measure(const UiLayoutMeasureRequest &request) const override {
                ++measures[request.element.slot];
                lastFontScale = request.fontScale;
                if (failMeasure)
                    return Result<UiLayoutMeasurement>::Failure(MakeError(UiErrors::LayoutInvalid));
                const auto size = static_cast<std::int32_t>(request.element.slot * 8);
                return Result<UiLayoutMeasurement>::Success({{size, size}, false, false});
            }

            Result<UiLayoutArrangement> Arrange(const UiLayoutArrangeRequest &request,
                                                const std::span<UiLogicalRect> childContent) const override {
                const auto call = ++arranges[request.element.slot];
                for (std::uint32_t index = 0; index < childContent.size(); ++index) {
                    const auto childSize = request.children[index].measurement.desired;
                    const std::int32_t shift = alternateAssignments && request.remeasure ? 1 : 0;
                    childContent[index] = {{request.assignedContent.origin.x + static_cast<std::int32_t>(index * 64),
                                            request.assignedContent.origin.y},
                                           {childSize.width + shift, childSize.height}};
                }
                (void)call;
                return Result<UiLayoutArrangement>::Success(Arrangement(request.assignedContent));
            }

            void ResetCounts() const {
                constraints.fill(0);
                measures.fill(0);
                arranges.fill(0);
            }
        };

        class DependentEvaluator final : public CountingEvaluator {
        public:
            Result<UiLayoutMeasurement> Measure(const UiLayoutMeasureRequest &request) const override {
                auto result = CountingEvaluator::Measure(request);
                if (result.HasError())
                    return result;
                auto measurement = result.Value();
                if (request.element.slot != 1)
                    measurement.dependsOnParentWidth = true;
                return Result<UiLayoutMeasurement>::Success(measurement);
            }
        };

        class IntrinsicProvider final : public UiLayoutIntrinsicProvider {
        public:
            mutable std::uint32_t textCalls{};
            mutable std::uint32_t imageCalls{};
            bool failText{};
            bool failImage{};

            Result<UiLayoutIntrinsicMeasurement> MeasureText(const UiLayoutIntrinsicRequest &request) const override {
                ++textCalls;
                if (failText)
                    return Result<UiLayoutIntrinsicMeasurement>::Failure(MakeError(UiErrors::LayoutIntrinsicUnavailable));
                return Result<UiLayoutIntrinsicMeasurement>::Success({{128, 32}, 24, true, false});
            }

            Result<UiLayoutIntrinsicMeasurement> MeasureImage(const UiLayoutIntrinsicRequest &request) const override {
                ++imageCalls;
                if (failImage)
                    return Result<UiLayoutIntrinsicMeasurement>::Failure(MakeError(UiErrors::LayoutIntrinsicUnavailable));
                return Result<UiLayoutIntrinsicMeasurement>::Success({{96, 64}, NoUiBaseline, false, false});
            }
        };

        UiLayoutElementDescriptor Descriptor(const UiElementHandle element, const UiLayoutStyle style,
                                             const UiLayoutIntrinsicSource intrinsic = {}) {
            return {element, style, intrinsic};
        }

        UiLayoutUpdateRequest Request(const UiLayoutEvaluator &evaluator, const std::uint64_t content = 1) {
            return {Sources(content), {{0, 0}, {1024, 768}}, {{0, 0}, {1024, 768}}, &evaluator};
        }

        template <typename Value> void RequireError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE_FALSE(result.HasValue());
            const auto &error = result.ErrorValue();
            REQUIRE(error.code.Value() == expected.code.Value());
        }

        void PublishBaseline(UiLayoutEngine &engine, UiElementTree &tree, CountingEvaluator &evaluator) {
            const auto baseline = engine.Update(tree, Request(evaluator));
            REQUIRE(baseline.HasValue());
            evaluator.ResetCounts();
        }

        TEST_CASE("Incremental layout publishes immutable authored-order geometry", "[runtime_ui][layout]") {
            auto tree = Tree();
            auto engine = Engine();
            CountingEvaluator evaluator;
            auto published = engine.Update(tree, Request(evaluator));
            REQUIRE(published.HasValue());
            auto snapshot = std::move(published).Value();
            REQUIRE(snapshot.Records().size() == 4);
            REQUIRE(snapshot.Descriptor().sources == Sources());
            REQUIRE(snapshot.Descriptor().interaction == Rev<UiInteractionRevision>(1));
            REQUIRE(snapshot.Records()[0].element == tree.Root().Value().handle);
            REQUIRE(snapshot.Records()[1].element == tree.Find(Stable<UiElementId>(2)).Value());
            const auto last = tree.Find(Stable<UiElementId>(4)).Value();
            REQUIRE(snapshot.Get(last).HasValue());
            auto stale = last;
            ++stale.generation;
            RequireError(snapshot.Get(stale), UiErrors::HandleStale);

            const auto retained = snapshot.Records()[0].arrangement.contentBox;
            evaluator.ResetCounts();
            const auto unchanged = engine.Update(tree, Request(evaluator));
            REQUIRE(unchanged.HasValue());
            REQUIRE(unchanged.Value().Descriptor().interaction == snapshot.Descriptor().interaction);
            REQUIRE(evaluator.measures == std::array<std::uint32_t, 32>{});
            REQUIRE(evaluator.arranges == std::array<std::uint32_t, 32>{});
            tree.Shutdown();
            engine.Shutdown();
            REQUIRE(snapshot.Records()[0].arrangement.contentBox == retained);
        }

        TEST_CASE("Measure dirtiness recomputes ancestors but skips an unaffected sibling", "[runtime_ui][layout][incremental]") {
            auto tree = Tree();
            auto engine = Engine();
            CountingEvaluator evaluator;
            PublishBaseline(engine, tree, evaluator);

            const auto leaf = tree.Find(Stable<UiElementId>(3)).Value();
            const auto sibling = tree.Find(Stable<UiElementId>(4)).Value();
            REQUIRE(engine.Invalidate({leaf, tree.Revision(), UiLayoutDirtyKind::Measure}).HasValue());
            REQUIRE(engine.Invalidate({leaf, tree.Revision(), UiLayoutDirtyKind::Measure}).HasValue());
            auto request = Request(evaluator, 2);
            auto updated = engine.Update(tree, request);
            REQUIRE(updated.HasValue());
            REQUIRE(updated.Value().Descriptor().interaction == Rev<UiInteractionRevision>(2));
            REQUIRE(evaluator.measures[leaf.slot] == 1);
            REQUIRE(evaluator.measures[tree.Find(Stable<UiElementId>(2)).Value().slot] == 1);
            REQUIRE(evaluator.measures[tree.Root().Value().handle.slot] == 1);
            REQUIRE(evaluator.measures[sibling.slot] == 0);
            REQUIRE(evaluator.arranges[sibling.slot] == 0);
        }

        TEST_CASE("Arrange-only dirtiness does not measure clean elements", "[runtime_ui][layout][incremental]") {
            CountingEvaluator evaluator;
            auto engine = Engine();
            auto tree = Tree();
            PublishBaseline(engine, tree, evaluator);
            const auto leaf = tree.Find(Stable<UiElementId>(3)).Value();
            REQUIRE(engine.Invalidate({leaf, tree.Revision(), UiLayoutDirtyKind::Arrange}).HasValue());
            REQUIRE(engine.Update(tree, Request(evaluator, 2)).HasValue());
            REQUIRE(evaluator.measures == std::array<std::uint32_t, 32>{});
            REQUIRE(evaluator.arranges[leaf.slot] == 1);
        }

        TEST_CASE("Structural revisions rebuild bounded topology transactionally", "[runtime_ui][layout][structure]") {
            auto tree = Tree();
            auto engine = Engine();
            CountingEvaluator evaluator;
            REQUIRE(engine.Update(tree, Request(evaluator)).HasValue());

            auto commandsResult =
                UiStructuralCommandBuffer::Create(tree.Instance(), tree.Canvas(), tree.SourceDocumentRevision(), tree.Revision(), 1);
            REQUIRE(commandsResult.HasValue());
            auto commands = std::move(commandsResult).Value();
            const auto root = tree.Root().Value().handle;
            REQUIRE(commands.Add(UiInsertElementCommand{Stable<UiElementId>(5), root, 2}).HasValue());
            const auto committed = tree.CommitDeferred(commands, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands);
            REQUIRE(committed.HasValue());
            REQUIRE(engine.Invalidate({root, tree.Revision(), UiLayoutDirtyKind::Structure}).HasValue());
            auto request = Request(evaluator, 2);
            request.sources.tree = tree.Revision();
            const auto updated = engine.Update(tree, request);
            REQUIRE(updated.HasValue());
            REQUIRE(updated.Value().Records().size() == 5);
            REQUIRE(updated.Value().Get(tree.Find(Stable<UiElementId>(5)).Value()).HasValue());
        }

        TEST_CASE("Layout failure retains the last good generation and pending dirtiness", "[runtime_ui][layout][failure]") {
            auto tree = Tree();
            auto engine = Engine();
            CountingEvaluator evaluator;
            auto first = engine.Update(tree, Request(evaluator));
            REQUIRE(first.HasValue());
            const auto leaf = tree.Find(Stable<UiElementId>(3)).Value();
            REQUIRE(engine.Invalidate({leaf, tree.Revision(), UiLayoutDirtyKind::Measure}).HasValue());
            evaluator.failMeasure = true;
            RequireError(engine.Update(tree, Request(evaluator, 2)), UiErrors::LayoutInvalid);
            REQUIRE(first.Value().Descriptor().interaction == Rev<UiInteractionRevision>(1));
            evaluator.failMeasure = false;
            REQUIRE(engine.Update(tree, Request(evaluator, 2)).HasValue());
        }

        TEST_CASE("Layout snapshot slots never overwrite leased generations", "[runtime_ui][layout][lifetime]") {
            auto tree = Tree();
            auto engine = Engine(2);
            CountingEvaluator evaluator;
            std::optional<UiLayoutSnapshot> first{std::move(engine.Update(tree, Request(evaluator))).Value()};
            const auto leaf = tree.Find(Stable<UiElementId>(3)).Value();
            REQUIRE(engine.Invalidate({leaf, tree.Revision(), UiLayoutDirtyKind::Arrange}).HasValue());
            std::optional<UiLayoutSnapshot> second{std::move(engine.Update(tree, Request(evaluator, 2))).Value()};
            REQUIRE(engine.Invalidate({leaf, tree.Revision(), UiLayoutDirtyKind::Arrange}).HasValue());
            RequireError(engine.Update(tree, Request(evaluator, 3)), UiErrors::LayoutSnapshotStorageExhausted);
            first.reset();
            REQUIRE(engine.Update(tree, Request(evaluator, 3)).HasValue());
            second.reset();
        }

        TEST_CASE("Dependent layout gets one bounded remeasure and rejects a second change", "[runtime_ui][layout][remeasure]") {
            auto tree = Tree();
            auto engine = Engine();
            DependentEvaluator evaluator;
            evaluator.alternateAssignments = true;
            RequireError(engine.Update(tree, Request(evaluator)), UiErrors::LayoutNonConvergent);
        }

        TEST_CASE("Layout validates source and lifecycle evidence", "[runtime_ui][layout][lifecycle]") {
            auto tree = Tree();
            auto engine = Engine();
            CountingEvaluator evaluator;
            auto request = Request(evaluator);
            request.sources.tree = Rev<UiRuntimeTreeRevision>(99);
            RequireError(engine.Update(tree, request), UiErrors::LayoutSourceStale);
            REQUIRE(engine.Update(tree, Request(evaluator)).HasValue());
            REQUIRE(engine.BeginRetirement().HasValue());
            RequireError(engine.Update(tree, Request(evaluator)), UiErrors::LayoutLifecycleUnavailable);
            engine.Shutdown();
            engine.Shutdown();
            REQUIRE(engine.State() == UiLayoutEngineState::Stopped);
        }

        TEST_CASE("Font scale is revisioned through intrinsic measurement and retained snapshots", "[runtime_ui][layout][font_scale]") {
            auto tree = Tree();
            auto engine = Engine();
            CountingEvaluator evaluator;
            auto initial = engine.Update(tree, Request(evaluator));
            REQUIRE(initial.HasValue());
            REQUIRE(initial.Value().Descriptor().fontScale == UiCanvasScaleFactor{1, 1});

            auto scaledRequest = Request(evaluator, 2);
            scaledRequest.fontScale = {3, 2};
            const auto scaled = engine.Update(tree, scaledRequest);
            REQUIRE(scaled.HasValue());
            REQUIRE(evaluator.lastFontScale == UiCanvasScaleFactor{3, 2});
            REQUIRE(scaled.Value().Descriptor().fontScale == UiCanvasScaleFactor{3, 2});
            REQUIRE(evaluator.measures[tree.Root().Value().handle.slot] > 0);

            auto malformed = Request(evaluator, 3);
            malformed.fontScale.denominator = 0;
            RequireError(engine.Update(tree, malformed), UiErrors::LayoutInvalid);
            REQUIRE(scaled.Value().Descriptor().fontScale == UiCanvasScaleFactor{3, 2});
        }

        TEST_CASE("Layout frame-hot updates allocate no fallback storage", "[runtime_ui][layout][allocation]") {
            auto tree = Tree();
            auto engine = Engine();
            CountingEvaluator evaluator;
            REQUIRE(engine.Update(tree, Request(evaluator)).HasValue());
            const auto before = layoutAllocations.load(std::memory_order_relaxed);
            for (std::uint64_t revision = 2; revision < 8; ++revision) {
                const auto leaf = tree.Find(Stable<UiElementId>(3)).Value();
                REQUIRE(engine.Invalidate({leaf, tree.Revision(), UiLayoutDirtyKind::Arrange}).HasValue());
                auto snapshot = engine.Update(tree, Request(evaluator, revision));
                REQUIRE(snapshot.HasValue());
            }
            REQUIRE(layoutAllocations.load(std::memory_order_relaxed) == before);
        }

        TEST_CASE("Declarative layout resolves anchors pivots offsets box model and intrinsic metrics", "[runtime_ui][layout][semantic]") {
            auto tree = Tree();
            const auto root = tree.Root().Value().handle;
            const auto anchored = tree.Find(Stable<UiElementId>(2)).Value();
            const auto text = tree.Find(Stable<UiElementId>(3)).Value();
            const auto stretched = tree.Find(Stable<UiElementId>(4)).Value();

            UiLayoutStyle rootStyle;
            rootStyle.padding = {8, 8, 8, 8};
            rootStyle.border = {2, 2, 2, 2};
            UiLayoutStyle anchoredStyle;
            anchoredStyle.positioning = UiLayoutPositioning::Absolute;
            anchoredStyle.width = UiLength::Dip(256);
            anchoredStyle.height = UiLength::Dip(64);
            anchoredStyle.anchors.horizontal.start = 0;
            anchoredStyle.anchors.vertical.start = 0;
            anchoredStyle.pivot = {0, 0};
            anchoredStyle.offsets = {10, 20, 0, 0};
            UiLayoutStyle textStyle;
            textStyle.positioning = UiLayoutPositioning::Absolute;
            textStyle.anchors.horizontal.start = 0;
            textStyle.anchors.vertical.end = UiScalarUnitsPerDip;
            textStyle.pivot = {0, 0};
            UiLayoutStyle stretchStyle;
            stretchStyle.positioning = UiLayoutPositioning::Absolute;
            stretchStyle.anchors.horizontal.start = 0;
            stretchStyle.anchors.horizontal.end = UiScalarUnitsPerDip;
            stretchStyle.anchors.vertical.start = 0;
            stretchStyle.anchors.vertical.end = UiScalarUnitsPerDip;

            const std::array descriptors{
                Descriptor(root, rootStyle),
                Descriptor(anchored, anchoredStyle),
                Descriptor(text, textStyle, {UiLayoutIntrinsicKind::Text, true, {}}),
                Descriptor(stretched, stretchStyle),
            };
            IntrinsicProvider provider;
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors, &provider);
            REQUIRE(evaluator.HasValue());
            auto engine = Engine();
            auto snapshot = engine.Update(tree, Request(evaluator.Value()));
            REQUIRE(snapshot.HasValue());

            const auto anchoredRecord = snapshot.Value().Get(anchored).Value();
            REQUIRE(anchoredRecord.arrangement.contentBox == UiLogicalRect{{10, 20}, {256, 64}});
            const auto textRecord = snapshot.Value().Get(text).Value();
            REQUIRE(textRecord.measurement.desired == UiLogicalExtent{128, 32});
            REQUIRE(textRecord.measurement.baseline == 24);
            REQUIRE(textRecord.arrangement.contentBox == UiLogicalRect{{10, 52}, {128, 32}});
            REQUIRE(provider.textCalls > 0);
            REQUIRE(snapshot.Value().Get(stretched).Value().arrangement.contentBox.extent == UiLogicalExtent{1024, 768});
            REQUIRE(snapshot.Value().Get(root).Value().arrangement.paddingBox == UiLogicalRect{{-8, -8}, {1040, 784}});
        }

        TEST_CASE("Declarative layout applies aspect ratio and reports deterministic bound conflicts", "[runtime_ui][layout][semantic]") {
            auto tree = Tree();
            const auto root = tree.Root().Value().handle;
            const auto ratio = tree.Find(Stable<UiElementId>(2)).Value();
            const auto conflict = tree.Find(Stable<UiElementId>(3)).Value();
            const auto remaining = tree.Find(Stable<UiElementId>(4)).Value();

            UiLayoutStyle rootStyle;
            UiLayoutStyle ratioStyle;
            ratioStyle.positioning = UiLayoutPositioning::Absolute;
            ratioStyle.width = UiLength::Dip(128);
            ratioStyle.aspectRatio = {2, 1};
            ratioStyle.anchors.horizontal.start = 0;
            ratioStyle.anchors.vertical.start = 0;
            ratioStyle.pivot = {0, 0};
            UiLayoutStyle conflictStyle;
            conflictStyle.minimumWidth = UiLength::Dip(200);
            conflictStyle.maximumWidth = UiLength::Dip(100);
            UiLayoutStyle remainingStyle;

            const std::array descriptors{
                Descriptor(root, rootStyle),
                Descriptor(ratio, ratioStyle),
                Descriptor(conflict, conflictStyle),
                Descriptor(remaining, remainingStyle),
            };
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors);
            REQUIRE(evaluator.HasValue());
            auto engine = Engine();
            auto snapshot = engine.Update(tree, Request(evaluator.Value()));
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Get(ratio).Value().measurement.desired == UiLogicalExtent{128, 64});
            REQUIRE(snapshot.Value().Get(conflict).Value().measurement.constraintResult == UiLayoutConstraintResult::Unsatisfiable);
            REQUIRE(snapshot.Value().Get(conflict).Value().measurement.desired.width == 200);
        }

        TEST_CASE("Declarative layout excludes absolute and anchored children from flow and intrinsic size",
                  "[runtime_ui][layout][semantic]") {
            auto tree = FlatTree();
            const auto root = tree.Root().Value().handle;
            const auto absolute = tree.Find(Stable<UiElementId>(2)).Value();
            const auto flow = tree.Find(Stable<UiElementId>(3)).Value();
            const auto anchored = tree.Find(Stable<UiElementId>(4)).Value();

            UiLayoutStyle absoluteStyle;
            absoluteStyle.positioning = UiLayoutPositioning::Absolute;
            absoluteStyle.width = UiLength::Dip(100);
            absoluteStyle.height = UiLength::Dip(200);
            absoluteStyle.offsets = {500, 100, 0, 0};
            UiLayoutStyle flowStyle;
            flowStyle.width = UiLength::Dip(40);
            flowStyle.height = UiLength::Dip(30);
            flowStyle.margin = {11, 5, 13, 7};
            UiLayoutStyle anchoredStyle;
            anchoredStyle.width = UiLength::Dip(20);
            anchoredStyle.height = UiLength::Dip(10);
            anchoredStyle.anchors.horizontal.start = 0;
            anchoredStyle.pivot = {0, 0};

            const std::array descriptors{
                Descriptor(root, {}),
                Descriptor(absolute, absoluteStyle),
                Descriptor(flow, flowStyle),
                Descriptor(anchored, anchoredStyle),
            };
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors);
            REQUIRE(evaluator.HasValue());
            auto engine = Engine();
            auto snapshot = engine.Update(tree, Request(evaluator.Value()));
            REQUIRE(snapshot.HasValue());

            const auto rootRecord = snapshot.Value().Get(root).Value();
            const auto flowRecord = snapshot.Value().Get(flow).Value();
            REQUIRE(flowRecord.measurement.desired.width == 40);
            REQUIRE(flowRecord.measurement.desired.height == 30);
            REQUIRE(rootRecord.measurement.desired.width == 64);
            REQUIRE(rootRecord.measurement.desired.height == 42);
            REQUIRE(snapshot.Value().Get(absolute).Value().arrangement.contentBox == UiLogicalRect{{500, 100}, {100, 200}});
            REQUIRE(snapshot.Value().Get(flow).Value().arrangement.contentBox == UiLogicalRect{{11, 5}, {40, 30}});
            REQUIRE(snapshot.Value().Get(anchored).Value().arrangement.contentBox == UiLogicalRect{{0, 0}, {20, 10}});
        }

        TEST_CASE("Declarative layout retains last good generation for required intrinsic failure and uses optional fallback",
                  "[runtime_ui][layout][semantic][failure]") {
            auto tree = Tree();
            const auto root = tree.Root().Value().handle;
            const auto text = tree.Find(Stable<UiElementId>(2)).Value();
            const auto image = tree.Find(Stable<UiElementId>(3)).Value();
            const auto remaining = tree.Find(Stable<UiElementId>(4)).Value();
            UiLayoutStyle textStyle;
            textStyle.positioning = UiLayoutPositioning::Absolute;
            UiLayoutStyle imageStyle;
            imageStyle.positioning = UiLayoutPositioning::Absolute;
            const std::array descriptors{
                Descriptor(root, {}),
                Descriptor(text, textStyle, {UiLayoutIntrinsicKind::Text, true, {}}),
                Descriptor(image, imageStyle, {UiLayoutIntrinsicKind::Image, false, {40, 20}}),
                Descriptor(remaining, {}),
            };
            IntrinsicProvider provider;
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors, &provider);
            REQUIRE(evaluator.HasValue());
            auto engine = Engine();
            auto first = engine.Update(tree, Request(evaluator.Value()));
            REQUIRE(first.HasValue());
            const auto firstInteraction = first.Value().Descriptor().interaction;
            provider.failText = true;
            REQUIRE(engine.Invalidate({text, tree.Revision(), UiLayoutDirtyKind::Measure}).HasValue());
            RequireError(engine.Update(tree, Request(evaluator.Value(), 2)), UiErrors::LayoutIntrinsicUnavailable);
            REQUIRE(first.Value().Descriptor().interaction == firstInteraction);
            provider.failText = false;
            REQUIRE(engine.Update(tree, Request(evaluator.Value(), 2)).HasValue());
            provider.failImage = true;
            REQUIRE(engine.Invalidate({image, tree.Revision(), UiLayoutDirtyKind::Measure}).HasValue());
            REQUIRE(engine.Update(tree, Request(evaluator.Value(), 3)).Value().Get(image).Value().measurement.desired ==
                    UiLogicalExtent{40, 20});
        }

        TEST_CASE("Declarative stack supports horizontal direction, gap and cross alignment", "[runtime_ui][layout][containers]") {
            auto tree = FlatTree();
            const auto root = tree.Root().Value().handle;
            const auto first = tree.Find(Stable<UiElementId>(2)).Value();
            const auto second = tree.Find(Stable<UiElementId>(3)).Value();
            const auto third = tree.Find(Stable<UiElementId>(4)).Value();

            UiLayoutStyle rootStyle;
            rootStyle.container.orientation = UiLayoutOrientation::Horizontal;
            rootStyle.container.gap = 10;
            rootStyle.container.crossAlignment = UiLayoutAlignment::Center;
            UiLayoutStyle itemStyle;
            itemStyle.width = UiLength::Dip(100);
            itemStyle.height = UiLength::Dip(20);
            const std::array descriptors{Descriptor(root, rootStyle), Descriptor(first, itemStyle), Descriptor(second, itemStyle),
                                         Descriptor(third, itemStyle)};
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors);
            REQUIRE(evaluator.HasValue());
            auto snapshot = Engine().Update(tree, Request(evaluator.Value()));
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Get(first).Value().arrangement.contentBox == UiLogicalRect{{0, 374}, {100, 20}});
            REQUIRE(snapshot.Value().Get(second).Value().arrangement.contentBox == UiLogicalRect{{110, 374}, {100, 20}});
            REQUIRE(snapshot.Value().Get(third).Value().arrangement.contentBox == UiLogicalRect{{220, 374}, {100, 20}});
        }

        TEST_CASE("Declarative stack preserves authored order for odd free space", "[runtime_ui][layout][containers][distribution]") {
            auto tree = FlatTree();
            const auto root = tree.Root().Value().handle;
            const auto first = tree.Find(Stable<UiElementId>(2)).Value();
            const auto second = tree.Find(Stable<UiElementId>(3)).Value();
            const auto third = tree.Find(Stable<UiElementId>(4)).Value();

            UiLayoutStyle rootStyle;
            rootStyle.container.orientation = UiLayoutOrientation::Horizontal;
            UiLayoutStyle itemStyle;
            itemStyle.width = UiLength::Dip(10);
            itemStyle.height = UiLength::Dip(10);
            const std::array descriptors{Descriptor(root, rootStyle), Descriptor(first, itemStyle), Descriptor(second, itemStyle),
                                         Descriptor(third, itemStyle)};
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors);
            REQUIRE(evaluator.HasValue());
            auto request = Request(evaluator.Value());
            request.rootConstraints.maximum = {35, 100};
            request.rootContent.extent = {35, 100};

            auto snapshot = Engine().Update(tree, request);
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Get(first).Value().arrangement.contentBox.origin.x == 0);
            REQUIRE(snapshot.Value().Get(second).Value().arrangement.contentBox.origin.x == 10);
        }

        TEST_CASE("Declarative stack centers odd free space with ties to even", "[runtime_ui][layout][containers][distribution]") {
            auto tree = FlatTree();
            const auto root = tree.Root().Value().handle;
            const auto first = tree.Find(Stable<UiElementId>(2)).Value();
            const auto second = tree.Find(Stable<UiElementId>(3)).Value();
            const auto third = tree.Find(Stable<UiElementId>(4)).Value();
            UiLayoutStyle rootStyle;
            rootStyle.container.orientation = UiLayoutOrientation::Horizontal;
            rootStyle.container.mainAlignment = UiLayoutDistribution::Center;
            UiLayoutStyle itemStyle;
            itemStyle.width = UiLength::Dip(10);
            itemStyle.height = UiLength::Dip(10);
            const std::array centeredDescriptors{Descriptor(root, rootStyle), Descriptor(first, itemStyle), Descriptor(second, itemStyle),
                                                 Descriptor(third, itemStyle)};
            auto centeredEvaluator = UiDeclarativeLayoutEvaluator::Create(centeredDescriptors);
            REQUIRE(centeredEvaluator.HasValue());
            auto centeredRequest = Request(centeredEvaluator.Value(), 2);
            centeredRequest.rootConstraints.maximum = {35, 100};
            centeredRequest.rootContent.extent = {35, 100};
            const auto snapshot = Engine().Update(tree, centeredRequest);
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Get(first).Value().arrangement.contentBox.origin.x == 2);
        }

        TEST_CASE("Declarative stack distributes SpaceBetween remainders in authored order",
                  "[runtime_ui][layout][containers][distribution]") {
            auto tree = FlatTree();
            const auto root = tree.Root().Value().handle;
            const auto first = tree.Find(Stable<UiElementId>(2)).Value();
            const auto second = tree.Find(Stable<UiElementId>(3)).Value();
            const auto third = tree.Find(Stable<UiElementId>(4)).Value();
            UiLayoutStyle rootStyle;
            rootStyle.container.orientation = UiLayoutOrientation::Horizontal;
            rootStyle.container.mainAlignment = UiLayoutDistribution::SpaceBetween;
            UiLayoutStyle itemStyle;
            itemStyle.width = UiLength::Dip(10);
            itemStyle.height = UiLength::Dip(10);
            const std::array betweenDescriptors{Descriptor(root, rootStyle), Descriptor(first, itemStyle), Descriptor(second, itemStyle),
                                                Descriptor(third, itemStyle)};
            auto betweenEvaluator = UiDeclarativeLayoutEvaluator::Create(betweenDescriptors);
            REQUIRE(betweenEvaluator.HasValue());
            auto betweenRequest = Request(betweenEvaluator.Value(), 3);
            betweenRequest.rootConstraints.maximum = {35, 100};
            betweenRequest.rootContent.extent = {35, 100};
            const auto snapshot = Engine().Update(tree, betweenRequest);
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Get(second).Value().arrangement.contentBox.origin.x == 12);
            REQUIRE(snapshot.Value().Get(third).Value().arrangement.contentBox.origin.x == 25);
        }

        TEST_CASE("Declarative stack distributes SpaceEvenly remainders in authored order",
                  "[runtime_ui][layout][containers][distribution]") {
            auto tree = FlatTree();
            const auto root = tree.Root().Value().handle;
            const auto first = tree.Find(Stable<UiElementId>(2)).Value();
            const auto second = tree.Find(Stable<UiElementId>(3)).Value();
            const auto third = tree.Find(Stable<UiElementId>(4)).Value();
            UiLayoutStyle rootStyle;
            rootStyle.container.orientation = UiLayoutOrientation::Horizontal;
            rootStyle.container.mainAlignment = UiLayoutDistribution::SpaceEvenly;
            UiLayoutStyle itemStyle;
            itemStyle.width = UiLength::Dip(10);
            itemStyle.height = UiLength::Dip(10);
            const std::array evenlyDescriptors{Descriptor(root, rootStyle), Descriptor(first, itemStyle), Descriptor(second, itemStyle),
                                               Descriptor(third, itemStyle)};
            auto evenlyEvaluator = UiDeclarativeLayoutEvaluator::Create(evenlyDescriptors);
            REQUIRE(evenlyEvaluator.HasValue());
            auto evenlyRequest = Request(evenlyEvaluator.Value(), 4);
            evenlyRequest.rootConstraints.maximum = {35, 100};
            evenlyRequest.rootContent.extent = {35, 100};
            const auto snapshot = Engine().Update(tree, evenlyRequest);
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Get(first).Value().arrangement.contentBox.origin.x == 1);
            REQUIRE(snapshot.Value().Get(second).Value().arrangement.contentBox.origin.x == 12);
            REQUIRE(snapshot.Value().Get(third).Value().arrangement.contentBox.origin.x == 24);
        }

        TEST_CASE("Declarative flex distributes positive space and wraps deterministically", "[runtime_ui][layout][containers][flex]") {
            auto tree = FlatTree();
            const auto root = tree.Root().Value().handle;
            const auto first = tree.Find(Stable<UiElementId>(2)).Value();
            const auto second = tree.Find(Stable<UiElementId>(3)).Value();
            const auto third = tree.Find(Stable<UiElementId>(4)).Value();

            UiLayoutStyle rootStyle;
            rootStyle.container.kind = UiLayoutContainerKind::Flex;
            rootStyle.container.orientation = UiLayoutOrientation::Horizontal;
            rootStyle.container.gap = 10;
            UiLayoutStyle flexItem;
            flexItem.width = UiLength::Dip(100);
            flexItem.height = UiLength::Dip(20);
            flexItem.flex.grow = 1;
            const std::array descriptors{Descriptor(root, rootStyle), Descriptor(first, flexItem), Descriptor(second, flexItem),
                                         Descriptor(third, flexItem)};
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors);
            REQUIRE(evaluator.HasValue());
            auto request = Request(evaluator.Value());
            request.rootConstraints.maximum.width = 700;
            request.rootContent.extent.width = 700;
            auto snapshot = Engine().Update(tree, request);
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Get(first).Value().arrangement.contentBox.extent.width == 226);
            REQUIRE(snapshot.Value().Get(second).Value().arrangement.contentBox.origin.x == 236);
            REQUIRE(snapshot.Value().Get(third).Value().arrangement.contentBox.origin.x == 473);

            rootStyle.container.wrap = UiLayoutWrapMode::Wrap;
            flexItem.flex.grow = 0;
            flexItem.width = UiLength::Dip(300);
            const std::array wrappedDescriptors{Descriptor(root, rootStyle), Descriptor(first, flexItem), Descriptor(second, flexItem),
                                                Descriptor(third, flexItem)};
            auto wrappedEvaluator = UiDeclarativeLayoutEvaluator::Create(wrappedDescriptors);
            REQUIRE(wrappedEvaluator.HasValue());
            auto wrappedRequest = Request(wrappedEvaluator.Value());
            wrappedRequest.rootConstraints.maximum.width = 700;
            wrappedRequest.rootContent.extent.width = 700;
            auto wrapped = Engine().Update(tree, wrappedRequest);
            REQUIRE(wrapped.HasValue());
            REQUIRE(wrapped.Value().Get(third).Value().arrangement.contentBox == UiLogicalRect{{0, 30}, {300, 20}});
        }

        TEST_CASE("Declarative containers arrange nested container descendants", "[runtime_ui][layout][containers][nested]") {
            auto tree = Tree();
            const auto root = tree.Root().Value().handle;
            const auto container = tree.Find(Stable<UiElementId>(2)).Value();
            const auto nested = tree.Find(Stable<UiElementId>(3)).Value();
            const auto sibling = tree.Find(Stable<UiElementId>(4)).Value();

            UiLayoutStyle rootStyle;
            rootStyle.container.kind = UiLayoutContainerKind::Flex;
            rootStyle.container.orientation = UiLayoutOrientation::Horizontal;
            rootStyle.container.gap = 10;
            UiLayoutStyle containerStyle;
            containerStyle.width = UiLength::Dip(300);
            containerStyle.height = UiLength::Dip(100);
            containerStyle.container.kind = UiLayoutContainerKind::Flex;
            containerStyle.container.orientation = UiLayoutOrientation::Vertical;
            containerStyle.container.gap = 5;
            UiLayoutStyle nestedStyle;
            nestedStyle.width = UiLength::Dip(80);
            nestedStyle.height = UiLength::Dip(20);
            UiLayoutStyle siblingStyle;
            siblingStyle.width = UiLength::Dip(50);
            siblingStyle.height = UiLength::Dip(20);

            const std::array descriptors{Descriptor(root, rootStyle), Descriptor(container, containerStyle),
                                         Descriptor(nested, nestedStyle), Descriptor(sibling, siblingStyle)};
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors);
            REQUIRE(evaluator.HasValue());
            auto request = Request(evaluator.Value());
            request.rootConstraints.maximum = {500, 200};
            request.rootContent.extent = {500, 200};
            auto snapshot = Engine().Update(tree, request);
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Get(container).Value().arrangement.contentBox == UiLogicalRect{{0, 0}, {300, 100}});
            REQUIRE(snapshot.Value().Get(nested).Value().arrangement.contentBox == UiLogicalRect{{0, 0}, {80, 20}});
            REQUIRE(snapshot.Value().Get(sibling).Value().arrangement.contentBox == UiLogicalRect{{310, 0}, {50, 20}});
        }

        TEST_CASE("Declarative flex clamps negative and positive space to min and max bounds",
                  "[runtime_ui][layout][containers][flex][bounds]") {
            auto tree = FlatTree();
            const auto root = tree.Root().Value().handle;
            const auto first = tree.Find(Stable<UiElementId>(2)).Value();
            const auto second = tree.Find(Stable<UiElementId>(3)).Value();
            const auto outOfFlow = tree.Find(Stable<UiElementId>(4)).Value();

            UiLayoutStyle rootStyle;
            rootStyle.container.kind = UiLayoutContainerKind::Flex;
            rootStyle.container.orientation = UiLayoutOrientation::Horizontal;
            UiLayoutStyle firstStyle;
            firstStyle.width = UiLength::Dip(200);
            firstStyle.height = UiLength::Dip(20);
            firstStyle.minimumWidth = UiLength::Dip(150);
            firstStyle.flex.shrink = 1;
            UiLayoutStyle secondStyle = firstStyle;
            secondStyle.minimumWidth = UiLength::Dip(0);
            UiLayoutStyle outOfFlowStyle;
            outOfFlowStyle.positioning = UiLayoutPositioning::Absolute;
            const std::array shrinkDescriptors{Descriptor(root, rootStyle), Descriptor(first, firstStyle), Descriptor(second, secondStyle),
                                               Descriptor(outOfFlow, outOfFlowStyle)};
            auto shrinkEvaluator = UiDeclarativeLayoutEvaluator::Create(shrinkDescriptors);
            REQUIRE(shrinkEvaluator.HasValue());
            auto shrinkRequest = Request(shrinkEvaluator.Value());
            shrinkRequest.rootConstraints.maximum = {250, 100};
            shrinkRequest.rootContent.extent = {250, 100};
            auto shrinkSnapshot = Engine().Update(tree, shrinkRequest);
            REQUIRE(shrinkSnapshot.HasValue());
            REQUIRE(shrinkSnapshot.Value().Get(first).Value().arrangement.contentBox == UiLogicalRect{{0, 0}, {150, 20}});
            REQUIRE(shrinkSnapshot.Value().Get(second).Value().arrangement.contentBox == UiLogicalRect{{150, 0}, {100, 20}});

            firstStyle.width = UiLength::Dip(100);
            firstStyle.minimumWidth = UiLength::Dip(0);
            firstStyle.maximumWidth = UiLength::Dip(120);
            firstStyle.flex.grow = 1;
            secondStyle = firstStyle;
            secondStyle.maximumWidth = UiLength::Auto();
            const std::array growDescriptors{Descriptor(root, rootStyle), Descriptor(first, firstStyle), Descriptor(second, secondStyle),
                                             Descriptor(outOfFlow, outOfFlowStyle)};
            auto growEvaluator = UiDeclarativeLayoutEvaluator::Create(growDescriptors);
            REQUIRE(growEvaluator.HasValue());
            auto growRequest = Request(growEvaluator.Value());
            growRequest.rootConstraints.maximum = {500, 100};
            growRequest.rootContent.extent = {500, 100};
            auto growSnapshot = Engine().Update(tree, growRequest);
            REQUIRE(growSnapshot.HasValue());
            REQUIRE(growSnapshot.Value().Get(first).Value().arrangement.contentBox == UiLogicalRect{{0, 0}, {120, 20}});
            REQUIRE(growSnapshot.Value().Get(second).Value().arrangement.contentBox == UiLogicalRect{{120, 0}, {380, 20}});
        }

        TEST_CASE("Declarative flex safely redistributes extreme weighted shrink", "[runtime_ui][layout][containers][flex][bounds]") {
            auto tree = FlatTree();
            const auto root = tree.Root().Value().handle;
            const auto first = tree.Find(Stable<UiElementId>(2)).Value();
            const auto second = tree.Find(Stable<UiElementId>(3)).Value();
            const auto outOfFlow = tree.Find(Stable<UiElementId>(4)).Value();

            UiLayoutStyle rootStyle;
            rootStyle.container.kind = UiLayoutContainerKind::Flex;
            rootStyle.container.orientation = UiLayoutOrientation::Horizontal;
            UiLayoutStyle firstStyle;
            firstStyle.width = UiLength::Dip(200'000'000);
            firstStyle.height = UiLength::Dip(20);
            firstStyle.flex.shrink = MaximumUiFlexFactor;
            UiLayoutStyle secondStyle = firstStyle;
            UiLayoutStyle outOfFlowStyle;
            outOfFlowStyle.positioning = UiLayoutPositioning::Absolute;
            const std::array descriptors{Descriptor(root, rootStyle), Descriptor(first, firstStyle), Descriptor(second, secondStyle),
                                         Descriptor(outOfFlow, outOfFlowStyle)};
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors);
            REQUIRE(evaluator.HasValue());
            auto request = Request(evaluator.Value());
            request.rootConstraints.maximum = {100'000'000, 100};
            request.rootContent.extent = {100'000'000, 100};
            auto snapshot = Engine().Update(tree, request);
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Get(first).Value().arrangement.contentBox == UiLogicalRect{{0, 0}, {50'000'000, 20}});
            REQUIRE(snapshot.Value().Get(second).Value().arrangement.contentBox == UiLogicalRect{{50'000'000, 0}, {50'000'000, 20}});
        }

        TEST_CASE("Declarative grid resolves fractional columns, auto rows, gaps and authored placement",
                  "[runtime_ui][layout][containers][grid]") {
            auto tree = FlatTree();
            const auto root = tree.Root().Value().handle;
            const auto first = tree.Find(Stable<UiElementId>(2)).Value();
            const auto second = tree.Find(Stable<UiElementId>(3)).Value();
            const auto third = tree.Find(Stable<UiElementId>(4)).Value();

            UiLayoutStyle rootStyle;
            rootStyle.container.kind = UiLayoutContainerKind::Grid;
            rootStyle.container.gap = 10;
            rootStyle.container.columnCount = 2;
            rootStyle.container.columns[0] = UiGridTrack::Fraction(1);
            rootStyle.container.columns[1] = UiGridTrack::Fraction(1);
            UiLayoutStyle itemStyle;
            itemStyle.width = UiLength::Dip(40);
            itemStyle.height = UiLength::Dip(20);
            const std::array descriptors{Descriptor(root, rootStyle), Descriptor(first, itemStyle), Descriptor(second, itemStyle),
                                         Descriptor(third, itemStyle)};
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors);
            REQUIRE(evaluator.HasValue());
            auto request = Request(evaluator.Value());
            request.rootConstraints.maximum = {210, 200};
            request.rootContent.extent = {210, 200};
            auto snapshot = Engine().Update(tree, request);
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Get(first).Value().arrangement.contentBox == UiLogicalRect{{0, 0}, {40, 20}});
            REQUIRE(snapshot.Value().Get(second).Value().arrangement.contentBox == UiLogicalRect{{110, 0}, {40, 20}});
            REQUIRE(snapshot.Value().Get(third).Value().arrangement.contentBox == UiLogicalRect{{0, 30}, {40, 20}});
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
