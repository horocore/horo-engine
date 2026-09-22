#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiHitTesting.h"
#include "Horo/Runtime/Ui/UiPresentationReceipt.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <optional>
#include <string_view>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Id> Id Stable(const std::uint8_t marker) {
            SerializedUiId bytes{};
            *bytes.rbegin() = marker;
            return Id::Create(bytes).Value();
        }

        UiOwnershipGeneration Owner() {
            return UiOwnershipGeneration::Create(41).Value();
        }

        template <typename Revision> Revision Rev(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        UiElementTreeDescriptor TreeDescriptor() {
            return {.instance = {Owner(), 1, 1},
                    .canvas = {Owner(), 2, 1},
                    .document = Stable<UiDocumentId>(1),
                    .documentRevision = Rev<UiDocumentRevision>(1),
                    .treeRevision = Rev<UiRuntimeTreeRevision>(1),
                    .limits = {4, 4, 4}};
        }

        UiElementTree Tree() {
            auto allocator = std::move(UiElementSlotAllocator::Create(Owner())).Value();
            const std::array elements{
                UiElementDescriptor{Stable<UiElementId>(1), {}},
                UiElementDescriptor{Stable<UiElementId>(2), Stable<UiElementId>(1)},
                UiElementDescriptor{Stable<UiElementId>(3), Stable<UiElementId>(1)},
            };
            auto result = UiElementTree::Create(allocator, TreeDescriptor(), elements);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        class Evaluator final : public UiLayoutEvaluator {
        public:
            Result<void> ResolveChildConstraints(const UiLayoutChildConstraintRequest &request,
                                                 const std::span<UiLayoutConstraints> output) const override {
                REQUIRE(output.size() == request.children.size());
                for (auto &constraints : output)
                    constraints = {{0, 0}, {200, 200}};
                return Result<void>::Success();
            }

            Result<UiLayoutMeasurement> Measure(const UiLayoutMeasureRequest &) const override {
                return Result<UiLayoutMeasurement>::Success({{100, 100}, false, false});
            }

            Result<UiLayoutArrangement> Arrange(const UiLayoutArrangeRequest &request,
                                                const std::span<UiLogicalRect> childContent) const override {
                for (auto &child : childContent)
                    child = {{0, 0}, {100, 100}};
                const UiLogicalRect rect = request.assignedContent;
                return Result<UiLayoutArrangement>::Success({rect, rect, rect, rect, rect, rect, NoUiBaseline});
            }
        };

        UiLayoutEngine LayoutEngine() {
            const auto tree = TreeDescriptor();
            return std::move(UiLayoutEngine::Create({tree.instance, tree.canvas, tree.document, 4, 4, 4, Rev<UiInteractionRevision>(1)}))
                .Value();
        }

        UiLayoutUpdateRequest LayoutRequest(const UiLayoutEvaluator &evaluator, const std::uint64_t generation = 1) {
            return {{Rev<UiDocumentRevision>(1), Rev<UiRuntimeTreeRevision>(1), Rev<UiLayoutContentRevision>(generation),
                     Rev<UiLayoutStyleRevision>(1), Rev<UiLayoutIntrinsicRevision>(1), Rev<UiLayoutCanvasRevision>(1),
                     Rev<UiLayoutPolicyRevision>(1)},
                    {{0, 0}, {200, 200}},
                    {{0, 0}, {200, 200}},
                    &evaluator};
        }

        UiHitTestStore HitStore(const UiRenderMode mode = UiRenderMode::ScreenSpaceOverlay, const std::uint32_t snapshots = 3) {
            const auto tree = TreeDescriptor();
            auto result = UiHitTestStore::Create({tree.instance, tree.canvas, tree.document, mode, {200, 200}, 4, snapshots});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        std::array<UiHitTestElement, 3> Projection(const UiLayoutSnapshot &layout) {
            const auto records = layout.Records();
            REQUIRE(records.size() == 3);
            return {UiHitTestElement{records[0].element, {}, {}, {}, 0, false, true, true},
                    UiHitTestElement{records[1].element, {}, {}, {}, 1, false, true, true},
                    UiHitTestElement{records[2].element, {}, {}, {}, 1, false, true, true}};
        }

        UiRenderViewId View() {
            return {Owner(), 9, 1};
        }

        UiPresentedInteractionState Presented(const UiCanvasInstanceId canvas, const UiInteractionRevision interaction,
                                              const std::uint64_t snapshot = 1) {
            auto state = std::move(UiPresentedInteractionState::Create(View(), canvas)).Value();
            REQUIRE(state
                        .Apply({state.View(), canvas, interaction, Rev<UiRenderSnapshotRevision>(snapshot),
                                UiPresentationOutcome::Presented, UiPresentationReason::None})
                        .HasValue());
            return state;
        }

        UiScreenPointerQuery ScreenQuery(const UiCanvasInstanceId canvas, const float x, const float y) {
            return {View(), canvas, {{200, 200}, {400, 400}, {128, 1}}, x, y};
        }

        template <typename Value> std::string_view ResultErrorCode(const Result<Value> &result) {
            REQUIRE(result.HasError());
            return result.ErrorValue().code.Value();
        }

        template <typename Value> void RequireError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(ResultErrorCode(result) == expected.code.Value());
        }

        struct HitTestFixture {
            UiElementTree tree{Tree()};
            Evaluator evaluator;
            UiLayoutEngine layoutEngine{LayoutEngine()};
            UiLayoutSnapshot layout{std::move(layoutEngine.Update(tree, LayoutRequest(evaluator))).Value()};
            std::array<UiHitTestElement, 3> projection{Projection(layout)};
        };

        TEST_CASE_METHOD(HitTestFixture, "Screen hit testing applies reverse paint order and semantic filters",
                         "[runtime_ui][hit_test][screen]") {
            projection[0].visible = false;
            projection[2].transform.values[4] = 50.0F;
            projection[2].transform.values[5] = 50.0F;
            projection[2].clip = {{50, 50}, {50, 50}};
            projection[2].hasClip = true;
            projection[1].hitSlop = {10, 10, 10, 10};
            auto store = HitStore();
            auto snapshot = std::move(store.Publish(layout, projection)).Value();
            auto presented = Presented(layout.Descriptor().canvas, layout.Descriptor().interaction);

            const auto topmost = snapshot.HitTestScreen(ScreenQuery(layout.Descriptor().canvas, 120.0F, 120.0F), presented);
            REQUIRE(topmost.HasValue());
            REQUIRE(topmost.Value().has_value());
            REQUIRE(topmost.Value()->element == projection[2].element);
            REQUIRE(topmost.Value()->logicalPosition == UiLogicalPoint{60, 60});

            projection[2].enabled = false;
            auto filtered = std::move(store.Publish(layout, projection)).Value();
            const auto next = filtered.HitTestScreen(ScreenQuery(layout.Descriptor().canvas, 120.0F, 120.0F), presented);
            REQUIRE(next.HasValue());
            REQUIRE(next.Value()->element == projection[1].element);
            const auto clipped = snapshot.HitTestScreen(ScreenQuery(layout.Descriptor().canvas, 220.0F, 120.0F), presented);
            REQUIRE(clipped.HasValue());
            REQUIRE_FALSE(clipped.Value().has_value());
            const auto slop = filtered.HitTestScreen(ScreenQuery(layout.Descriptor().canvas, 210.0F, 120.0F), presented);
            REQUIRE(slop.HasValue());
            REQUIRE(slop.Value()->element == projection[1].element);

            const auto before = ::Horo::Tests::AllocationProbe::Count();
            const auto allocationFree = snapshot.HitTestScreen(ScreenQuery(layout.Descriptor().canvas, 120.0F, 120.0F), presented);
            const auto after = ::Horo::Tests::AllocationProbe::Count();
            REQUIRE(allocationFree.HasValue());
            REQUIRE(after == before);

            auto wrongView = ScreenQuery(layout.Descriptor().canvas, 120.0F, 120.0F);
            ++wrongView.view.generation;
            RequireError(snapshot.HitTestScreen(wrongView, presented), UiErrors::HitTestSourceStale);
        }

        TEST_CASE_METHOD(HitTestFixture, "Presentation fencing survives skipped frames reload and shutdown",
                         "[runtime_ui][hit_test][lifetime]") {
            auto &firstLayout = layout;
            auto firstProjection = Projection(firstLayout);
            auto store = HitStore();
            auto first = std::move(store.Publish(firstLayout, firstProjection)).Value();
            auto presented = Presented(firstLayout.Descriptor().canvas, firstLayout.Descriptor().interaction);

            auto secondLayout = std::move(layoutEngine.Update(tree, LayoutRequest(evaluator, 2))).Value();
            auto secondProjection = Projection(secondLayout);
            secondProjection[2].visible = false;
            auto second = std::move(store.Publish(secondLayout, secondProjection)).Value();
            RequireError(second.HitTestScreen(ScreenQuery(secondLayout.Descriptor().canvas, 20.0F, 20.0F), presented),
                         UiErrors::HitTestNotPresented);

            REQUIRE(presented
                        .Apply({presented.View(), presented.Canvas(), secondLayout.Descriptor().interaction,
                                Rev<UiRenderSnapshotRevision>(2), UiPresentationOutcome::Skipped, UiPresentationReason::Suppressed})
                        .HasValue());
            RequireError(second.HitTestScreen(ScreenQuery(secondLayout.Descriptor().canvas, 20.0F, 20.0F), presented),
                         UiErrors::HitTestNotPresented);
            REQUIRE(first.HitTestScreen(ScreenQuery(firstLayout.Descriptor().canvas, 20.0F, 20.0F), presented).HasValue());

            REQUIRE(presented
                        .Apply({presented.View(), presented.Canvas(), secondLayout.Descriptor().interaction,
                                Rev<UiRenderSnapshotRevision>(3), UiPresentationOutcome::Presented, UiPresentationReason::None})
                        .HasValue());
            RequireError(first.HitTestScreen(ScreenQuery(firstLayout.Descriptor().canvas, 20.0F, 20.0F), presented),
                         UiErrors::HitTestNotPresented);
            store.Shutdown();
            const auto afterShutdown = second.HitTestScreen(ScreenQuery(secondLayout.Descriptor().canvas, 20.0F, 20.0F), presented);
            REQUIRE(afterShutdown.HasValue());
            REQUIRE(afterShutdown.Value()->element == secondProjection[1].element);
        }

        TEST_CASE_METHOD(HitTestFixture, "Screen hit testing maps physical pointers through the resolved safe content rectangle",
                         "[runtime_ui][hit_test][safe_area]") {
            projection[2].transform.values[4] = 50.0F;
            projection[2].transform.values[5] = 50.0F;
            projection[2].clip = {{50, 50}, {50, 50}};
            projection[2].hasClip = true;
            auto store = HitStore();
            auto snapshot = std::move(store.Publish(layout, projection)).Value();
            auto presented = Presented(layout.Descriptor().canvas, layout.Descriptor().interaction);
            const UiResolvedScreenCanvas safeCanvas{{200, 200}, {500, 450}, {128, 1}, {50, 25, 400, 400},       {50, 25, 50, 25},
                                                    {},         {},         {},       UiPixelSnapMode::Disabled};

            const auto hit = snapshot.HitTestScreen({View(), layout.Descriptor().canvas, safeCanvas, 170.0F, 145.0F}, presented);
            REQUIRE(hit.HasValue());
            REQUIRE(hit.Value().has_value());
            REQUIRE(hit.Value()->element == projection[2].element);
            REQUIRE(hit.Value()->logicalPosition == UiLogicalPoint{60, 60});

            const auto outside = snapshot.HitTestScreen({View(), layout.Descriptor().canvas, safeCanvas, 20.0F, 145.0F}, presented);
            REQUIRE(outside.HasValue());
            REQUIRE_FALSE(outside.Value().has_value());
        }

        TEST_CASE_METHOD(HitTestFixture, "World hit testing projects bounded rays without renderer state",
                         "[runtime_ui][hit_test][world]") {
            auto store = HitStore(UiRenderMode::WorldSpace);
            auto snapshot = std::move(store.Publish(layout, projection)).Value();
            auto presented = Presented(layout.Descriptor().canvas, layout.Descriptor().interaction);
            const UiWorldRayQuery query{View(),
                                        layout.Descriptor().canvas,
                                        {{0.5F, -0.5F, 1.0F}, {0.0F, 0.0F, -1.0F}, 0.0F, 2.0F},
                                        {{0.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F}, {0.0F, -2.0F, 0.0F}}};
            const auto hit = snapshot.HitTestWorld(query, presented);
            REQUIRE(hit.HasValue());
            REQUIRE(hit.Value().has_value());
            REQUIRE(hit.Value()->element == projection[2].element);
            REQUIRE(hit.Value()->logicalPosition == UiLogicalPoint{50, 50});
            REQUIRE(hit.Value()->rayDistance == 1.0F);

            auto parallel = query;
            parallel.ray.direction = {1.0F, 0.0F, 0.0F};
            const auto miss = snapshot.HitTestWorld(parallel, presented);
            REQUIRE(miss.HasValue());
            REQUIRE_FALSE(miss.Value().has_value());
            auto unbounded = query;
            unbounded.ray.maximumDistance = std::numeric_limits<float>::infinity();
            RequireError(snapshot.HitTestWorld(unbounded, presented), UiErrors::HitTestInvalid);
        }

        TEST_CASE_METHOD(HitTestFixture, "Publication rejects stale sources and never overwrites leased slots",
                         "[runtime_ui][hit_test][validation]") {
            auto store = HitStore(UiRenderMode::ScreenSpaceOverlay, 2);
            std::optional<UiHitTestSnapshot> first{std::move(store.Publish(layout, projection)).Value()};
            std::optional<UiHitTestSnapshot> second{std::move(store.Publish(layout, projection)).Value()};
            RequireError(store.Publish(layout, projection), UiErrors::HitTestSnapshotStorageExhausted);
            first.reset();
            REQUIRE(store.Publish(layout, projection).HasValue());
            second.reset();

            auto malformed = projection;
            ++malformed[1].element.generation;
            RequireError(store.Publish(layout, malformed), UiErrors::HitTestInvalid);
            REQUIRE(store.BeginRetirement().HasValue());
            RequireError(store.Publish(layout, projection), UiErrors::HitTestLifecycleUnavailable);
            store.Shutdown();
            REQUIRE(store.State() == UiHitTestStoreState::Stopped);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
