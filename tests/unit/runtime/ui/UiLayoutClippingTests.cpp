#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiLayoutClipping.h"
#include "UiTestUtils.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        using Test::Stable;

        UiOwnershipGeneration Owner() {
            return UiOwnershipGeneration::Create(41).Value();
        }

        template <typename Revision> Revision RevisionValue(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        struct LayoutEvaluator final : UiLayoutEvaluator {
            Result<void> ResolveChildConstraints(const UiLayoutChildConstraintRequest &request,
                                                 const std::span<UiLayoutConstraints> output) const override {
                if (output.size() != request.children.size())
                    return Result<void>::Failure(MakeError(UiErrors::LayoutInvalid));
                for (auto &constraint : output)
                    constraint = request.constraints;
                return Result<void>::Success();
            }

            Result<UiLayoutMeasurement> Measure(const UiLayoutMeasureRequest &request) const override {
                return Result<UiLayoutMeasurement>::Success({request.constraints.maximum, false, false});
            }

            Result<UiLayoutArrangement> Arrange(const UiLayoutArrangeRequest &request,
                                                const std::span<UiLogicalRect> childContent) const override {
                for (std::uint32_t index = 0; index < childContent.size(); ++index) {
                    switch (request.element.slot) {
                        case 1:
                            childContent[index] = {{10, 10}, {100, 100}};
                            break;
                        case 2:
                            childContent[index] = {{80, 80}, {80, 80}};
                            break;
                        case 3:
                            childContent[index] = {{300, 300}, {20, 20}};
                            break;
                        default:
                            return Result<UiLayoutArrangement>::Failure(MakeError(UiErrors::LayoutInvalid));
                    }
                }

                auto overflow = request.assignedContent;
                if (request.element.slot == 2)
                    overflow = {{10, 10}, {400, 400}};
                else if (request.element.slot == 3)
                    overflow = {{80, 80}, {300, 300}};
                return Result<UiLayoutArrangement>::Success({request.assignedContent, request.assignedContent, request.assignedContent,
                                                             request.assignedContent, overflow, request.assignedContent, NoUiBaseline});
            }
        };

        struct Fixture final {
            UiElementTree tree;
            UiLayoutEngine layout;
            UiLayoutClipEngine clipping;
            UiLayoutSnapshot snapshot;

            static Fixture Create() {
                auto allocatorResult = UiElementSlotAllocator::Create(Owner());
                REQUIRE(allocatorResult.HasValue());
                auto allocator = std::move(allocatorResult).Value();
                const std::array elements{
                    UiElementDescriptor{Stable<UiElementId>(1), {}},
                    UiElementDescriptor{Stable<UiElementId>(2), Stable<UiElementId>(1)},
                    UiElementDescriptor{Stable<UiElementId>(3), Stable<UiElementId>(2)},
                    UiElementDescriptor{Stable<UiElementId>(4), Stable<UiElementId>(3)},
                };
                const UiElementTreeDescriptor treeDescriptor{
                    {Owner(), 1, 1},
                    {Owner(), 2, 1},
                    Stable<UiDocumentId>(1),
                    RevisionValue<UiDocumentRevision>(2),
                    RevisionValue<UiRuntimeTreeRevision>(3),
                    {4, 8, 8},
                };
                auto treeResult = UiElementTree::Create(allocator, treeDescriptor, elements);
                REQUIRE(treeResult.HasValue());

                const auto layoutDescriptor = UiLayoutEngineDescriptor{
                    treeDescriptor.instance,
                    treeDescriptor.canvas,
                    treeDescriptor.document,
                    4,
                    8,
                    3,
                    RevisionValue<UiInteractionRevision>(1),
                };
                auto layoutResult = UiLayoutEngine::Create(layoutDescriptor);
                REQUIRE(layoutResult.HasValue());

                const auto clippingDescriptor = UiLayoutClipEngineDescriptor{
                    treeDescriptor.instance, treeDescriptor.canvas, treeDescriptor.document, 4, 4, 2, 3,
                };
                auto clippingResult = UiLayoutClipEngine::Create(clippingDescriptor);
                REQUIRE(clippingResult.HasValue());

                auto tree = std::move(treeResult).Value();
                auto layout = std::move(layoutResult).Value();
                auto clipping = std::move(clippingResult).Value();
                LayoutEvaluator evaluator;
                const auto layoutResultValue = layout.Update(tree, LayoutRequest(evaluator));
                REQUIRE(layoutResultValue.HasValue());
                return Fixture{std::move(tree), std::move(layout), std::move(clipping), std::move(layoutResultValue).Value()};
            }

            static UiLayoutSourceRevisions Sources() {
                return {RevisionValue<UiDocumentRevision>(2),        RevisionValue<UiRuntimeTreeRevision>(3),
                        RevisionValue<UiLayoutContentRevision>(1),   RevisionValue<UiLayoutStyleRevision>(1),
                        RevisionValue<UiLayoutIntrinsicRevision>(1), RevisionValue<UiLayoutCanvasRevision>(1),
                        RevisionValue<UiLayoutPolicyRevision>(1)};
            }

            static UiLayoutUpdateRequest LayoutRequest(const UiLayoutEvaluator &evaluator) {
                return {Sources(), {{0, 0}, {200, 200}}, {{0, 0}, {200, 200}}, &evaluator};
            }

            std::array<UiLayoutClipDescriptor, 4> Descriptors(
                const UiLayoutOverflowPolicy targetPolicy = UiLayoutOverflowPolicy::Clip) const {
                const auto records = snapshot.Records();
                return {UiLayoutClipDescriptor{records[0].element, UiLayoutOverflowPolicy::Visible, {}},
                        UiLayoutClipDescriptor{records[1].element, UiLayoutOverflowPolicy::Scroll, {999, 999}},
                        UiLayoutClipDescriptor{records[2].element, UiLayoutOverflowPolicy::Scroll, {-999, -999}},
                        UiLayoutClipDescriptor{records[3].element, targetPolicy, {}}};
            }

            UiFocusBringIntoViewRequest Bring(const UiFocusBringIntoViewPolicy policy) const {
                const auto &descriptor = snapshot.Descriptor();
                const auto owner = UiFocusOwnerContext{descriptor.instance,
                                                       descriptor.canvas,
                                                       descriptor.document,
                                                       descriptor.sources.document,
                                                       descriptor.sources.tree,
                                                       descriptor.interaction,
                                                       {std::nullopt, UiFocusPresentationLayerId{Owner(), 1, 1}}};
                return {owner, {Stable<UiElementId>(4), snapshot.Records().back().element}, policy};
            }
        };

        TEST_CASE("Clip projection clamps scroll extents and composes nested clip chains", "[runtime_ui][layout][clipping]") {
            auto fixture = Fixture::Create();
            auto descriptors = fixture.Descriptors();
            auto projection = fixture.clipping.Update(fixture.tree, fixture.snapshot, {descriptors, std::nullopt});
            REQUIRE(projection.HasValue());

            const auto snapshot = std::move(projection).Value();
            REQUIRE(snapshot.Records().size() == 4);
            REQUIRE(snapshot.Clips().size() == 3);
            REQUIRE(snapshot.Scrolls().size() == 2);
            REQUIRE(snapshot.Scrolls()[0].content.extent == UiLogicalExtent{400, 400});
            REQUIRE(snapshot.Scrolls()[0].maximumOffset == UiLogicalPoint{300, 300});
            REQUIRE(snapshot.Scrolls()[0].offset == UiLogicalPoint{300, 300});
            REQUIRE(snapshot.Scrolls()[1].offset == UiLogicalPoint{0, 0});
            REQUIRE(snapshot.Records()[1].ownClip == 0);
            REQUIRE(snapshot.Records()[2].clip == 0);
            REQUIRE(snapshot.Records()[2].ownClip == 1);
            REQUIRE(snapshot.Records()[3].clip == 1);
            REQUIRE(snapshot.Clips()[1].parent == 0);
            REQUIRE(snapshot.Records()[3].scrollTranslation == UiLogicalPoint{-300, -300});
        }

        TEST_CASE("Bring-into-view updates nested scroll state inner-to-outer", "[runtime_ui][layout][scroll]") {
            auto fixture = Fixture::Create();
            auto descriptors = fixture.Descriptors(UiLayoutOverflowPolicy::Visible);
            descriptors[1].scrollOffset = {};
            descriptors[2].scrollOffset = {};
            auto projection =
                fixture.clipping.Update(fixture.tree, fixture.snapshot, {descriptors, fixture.Bring(UiFocusBringIntoViewPolicy::Nearest)});
            REQUIRE(projection.HasValue());

            const auto snapshot = std::move(projection).Value();
            REQUIRE(snapshot.Scrolls().size() == 2);
            REQUIRE(snapshot.Scrolls()[0].offset == UiLogicalPoint{50, 50});
            REQUIRE(snapshot.Scrolls()[1].offset == UiLogicalPoint{160, 160});
            REQUIRE(snapshot.Records()[3].scrollTranslation == UiLogicalPoint{-210, -210});
        }

        TEST_CASE("Bring-into-view alignment policies resolve deterministically", "[runtime_ui][layout][scroll]") {
            struct AlignmentCase final {
                UiFocusBringIntoViewPolicy policy;
                UiLogicalPoint outer;
                UiLogicalPoint inner;
            };

            const std::array cases{AlignmentCase{UiFocusBringIntoViewPolicy::Start, {70, 70}, {220, 220}},
                                   AlignmentCase{UiFocusBringIntoViewPolicy::Center, {60, 60}, {190, 190}},
                                   AlignmentCase{UiFocusBringIntoViewPolicy::End, {50, 50}, {160, 160}}};
            for (const auto &alignment : cases) {
                auto fixture = Fixture::Create();
                auto descriptors = fixture.Descriptors(UiLayoutOverflowPolicy::Visible);
                descriptors[1].scrollOffset = {};
                descriptors[2].scrollOffset = {};
                const auto projection =
                    fixture.clipping.Update(fixture.tree, fixture.snapshot, {descriptors, fixture.Bring(alignment.policy)});
                REQUIRE(projection.HasValue());
                const auto snapshot = std::move(projection).Value();
                REQUIRE(snapshot.Scrolls()[0].offset == alignment.outer);
                REQUIRE(snapshot.Scrolls()[1].offset == alignment.inner);
            }
        }

        TEST_CASE("Stale reveal requests preserve the prior immutable generation", "[runtime_ui][layout][scroll][lifecycle]") {
            auto fixture = Fixture::Create();
            auto descriptors = fixture.Descriptors();
            auto first = fixture.clipping.Update(fixture.tree, fixture.snapshot, {descriptors, std::nullopt});
            REQUIRE(first.HasValue());
            std::optional<UiLayoutClipSnapshot> retained{std::move(first).Value()};

            auto stale = fixture.Bring(UiFocusBringIntoViewPolicy::Start);
            stale.owner.interaction = RevisionValue<UiInteractionRevision>(99);
            const auto rejected = fixture.clipping.Update(fixture.tree, fixture.snapshot, {descriptors, stale});
            REQUIRE_FALSE(rejected.HasValue());
            REQUIRE(rejected.ErrorValue().code.Value() == UiErrors::LayoutClipSourceStale.code.Value());
            REQUIRE(retained->Scrolls()[0].offset == UiLogicalPoint{300, 300});

            REQUIRE(fixture.clipping.BeginRetirement().HasValue());
            const auto unavailable = fixture.clipping.Update(fixture.tree, fixture.snapshot, {descriptors, std::nullopt});
            REQUIRE_FALSE(unavailable.HasValue());
            REQUIRE(unavailable.ErrorValue().code.Value() == UiErrors::LayoutClipLifecycleUnavailable.code.Value());
            fixture.clipping.Shutdown();
            fixture.clipping.Shutdown();
            REQUIRE(fixture.clipping.IsDrained() == false);
            retained.reset();
            REQUIRE(fixture.clipping.IsDrained());
        }

        TEST_CASE("Clip projection rejects publication while every snapshot slot is leased", "[runtime_ui][layout][capacity]") {
            auto fixture = Fixture::Create();
            auto descriptors = fixture.Descriptors();
            auto firstResult = fixture.clipping.Update(fixture.tree, fixture.snapshot, {descriptors, std::nullopt});
            auto secondResult = fixture.clipping.Update(fixture.tree, fixture.snapshot, {descriptors, std::nullopt});
            auto thirdResult = fixture.clipping.Update(fixture.tree, fixture.snapshot, {descriptors, std::nullopt});
            REQUIRE(firstResult.HasValue());
            REQUIRE(secondResult.HasValue());
            REQUIRE(thirdResult.HasValue());
            std::optional<UiLayoutClipSnapshot> first{std::move(firstResult).Value()};
            auto second = std::move(secondResult).Value();
            auto third = std::move(thirdResult).Value();

            const auto exhausted = fixture.clipping.Update(fixture.tree, fixture.snapshot, {descriptors, std::nullopt});
            REQUIRE_FALSE(exhausted.HasValue());
            REQUIRE(exhausted.ErrorValue().code.Value() == UiErrors::LayoutClipSnapshotStorageExhausted.code.Value());
            REQUIRE(third.Scrolls()[0].offset == UiLogicalPoint{300, 300});

            first.reset();
            const auto recovered = fixture.clipping.Update(fixture.tree, fixture.snapshot, {descriptors, std::nullopt});
            REQUIRE(recovered.HasValue());
            REQUIRE(second.Scrolls()[0].offset == UiLogicalPoint{300, 300});
        }

        TEST_CASE("Clip projection frame-hot updates use reserved storage", "[runtime_ui][layout][clipping][allocation]") {
            auto fixture = Fixture::Create();
            auto descriptors = fixture.Descriptors();
            REQUIRE(fixture.clipping.Update(fixture.tree, fixture.snapshot, {descriptors, std::nullopt}).HasValue());
            const auto before = Horo::Tests::AllocationProbe::Count();
            for (std::uint32_t iteration = 0; iteration < 4; ++iteration) {
                auto projection = fixture.clipping.Update(fixture.tree, fixture.snapshot, {descriptors, std::nullopt});
                REQUIRE(projection.HasValue());
            }
            REQUIRE(Horo::Tests::AllocationProbe::Count() == before);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
