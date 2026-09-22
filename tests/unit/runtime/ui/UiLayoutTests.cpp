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
    }  // namespace
}  // namespace Horo::Runtime::Ui
