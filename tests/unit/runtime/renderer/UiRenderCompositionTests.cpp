#include "../ui/UiRenderSnapshotTestSupport.h"
#include "Horo/Runtime/Render/UiRenderComposition.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiPresentationReceipt.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace Horo::Render {
    namespace {
        using namespace Runtime::Ui;

        SerializedUiId StableBytes(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return bytes;
        }

        UiOwnershipGeneration Owner(const std::uint64_t value = 17) {
            return UiOwnershipGeneration::Create(value).Value();
        }

        UiRenderViewId View(const std::uint32_t slot = 9) {
            return {Owner(), slot, 1};
        }

        UiRenderSnapshot Snapshot(const std::uint32_t canvasSlot, const std::uint64_t interactionRevision,
                                  const std::uint64_t snapshotRevision, const UiRenderViewId view = View()) {
            const auto document = UiDocumentId::Create(StableBytes(1)).Value();
            const auto element = UiElementId::Create(StableBytes(static_cast<std::uint8_t>(canvasSlot))).Value();
            const auto documentRevision = UiDocumentRevision::Create(4).Value();
            const auto treeRevision = UiRuntimeTreeRevision::Create(6).Value();
            const UiElementTreeDescriptor treeDescriptor{.instance = {Owner(), 1, 1},
                                                         .canvas = {Owner(), canvasSlot, 1},
                                                         .document = document,
                                                         .documentRevision = documentRevision,
                                                         .treeRevision = treeRevision,
                                                         .limits = {1, 1, 1}};
            auto allocatorResult = UiElementSlotAllocator::Create(Owner());
            REQUIRE(allocatorResult.HasValue());
            auto allocator = std::move(allocatorResult).Value();
            const std::array elements{UiElementDescriptor{element, {}}};
            auto treeResult = UiElementTree::Create(allocator, treeDescriptor, elements);
            REQUIRE(treeResult.HasValue());
            auto tree = std::move(treeResult).Value();

            const UiRenderSnapshotLimits limits{0, 0, 0, 0, 0, 1, 0};
            auto extractorResult = UiRenderExtractor::Create({view, limits, 1});
            REQUIRE(extractorResult.HasValue());
            auto extractor = std::move(extractorResult).Value();
            const auto descriptor =
                Runtime::Ui::Test::SnapshotDescriptor(tree, UiInteractionRevision::Create(interactionRevision).Value(),
                                                      UiRenderSnapshotRevision::Create(snapshotRevision).Value(), view, limits);
            const std::array transforms{UiLogicalTransform{}};
            auto snapshot = extractor.Extract(tree, descriptor, {.transforms = transforms});
            REQUIRE(snapshot.HasValue());
            return std::move(snapshot).Value();
        }

        RenderTextureDescriptor ColorTexture(const FramebufferExtent extent) {
            return {.dimension = RenderTextureDimension::TwoD,
                    .extent = extent,
                    .format = RenderTextureFormat::Rgba16Float,
                    .mipCount = 1,
                    .layerCount = 1,
                    .sampleCount = 1,
                    .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment,
                    .depth = 1};
        }

        RenderTextureDescriptor DepthTexture(const FramebufferExtent extent) {
            return {.dimension = RenderTextureDimension::TwoD,
                    .extent = extent,
                    .format = RenderTextureFormat::Depth32Float,
                    .mipCount = 1,
                    .layerCount = 1,
                    .sampleCount = 1,
                    .usage = RenderTextureUsage::RenderAttachment,
                    .depth = 1};
        }

        UiRenderCompositionPass Pass(const UiRenderSnapshot &snapshot, const RenderGraphOwnerId owner, const std::uint32_t passId,
                                     const std::uint32_t colorId, const UiRenderCompositionSpace space,
                                     const UiRenderCompositionPoint point, const UiRenderPresentationBand band,
                                     const FramebufferExtent extent) {
            UiRenderCompositionPass result{.snapshot = &snapshot,
                                           .pass = {owner, {passId}},
                                           .space = space,
                                           .point = point,
                                           .band = band,
                                           .colorInput = {{owner, colorId}, ColorTexture(extent)},
                                           .colorOutput = {{owner, colorId}, ColorTexture(extent)}};
            if (space == UiRenderCompositionSpace::World)
                result.depth = UiRenderCompositionTarget{{owner, colorId + 100}, DepthTexture(extent)};
            return result;
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }

        template <typename Mutation>
        void RequireRejectedPass(UiRenderCompositionPass pass, const RenderGraphOwnerId graph, const FramebufferExtent extent,
                                 Mutation mutate, const ErrorCodeDescriptor &expected = UiErrors::RenderCompositionInvalid) {
            mutate(pass);
            const std::array passes{std::move(pass)};
            RequireError(ValidateUiRenderComposition({View(), graph, extent, {1}, passes}), expected);
        }

        UiPresentationReceipt Receipt(const UiCanvasInstanceId canvas, const std::uint64_t interactionRevision,
                                      const std::uint64_t snapshotRevision, const UiPresentationOutcome outcome,
                                      const UiPresentationReason reason) {
            return {View(),
                    canvas,
                    UiInteractionRevision::Create(interactionRevision).Value(),
                    UiRenderSnapshotRevision::Create(snapshotRevision).Value(),
                    outcome,
                    reason};
        }

        UiPresentedInteractionState PresentationState(const UiCanvasInstanceId canvas) {
            auto result = UiPresentedInteractionState::Create(View(), canvas);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        TEST_CASE("UI composition admits ordered world camera and screen passes", "[renderer][runtime_ui][composition]") {
            const auto world = Snapshot(2, 2, 2);
            const auto camera = Snapshot(3, 3, 3);
            const auto screen = Snapshot(4, 4, 4);
            constexpr RenderGraphOwnerId graph{41};
            constexpr FramebufferExtent extent{1280, 720};
            const std::array passes{
                Pass(world, graph, 1, 1, UiRenderCompositionSpace::World, UiRenderCompositionPoint::SceneBeforePostProcess,
                     UiRenderPresentationBand::World, extent),
                Pass(camera, graph, 2, 2, UiRenderCompositionSpace::Camera, UiRenderCompositionPoint::SceneAfterPostProcess,
                     UiRenderPresentationBand::Hud, extent),
                Pass(screen, graph, 3, 3, UiRenderCompositionSpace::Screen, UiRenderCompositionPoint::DisplayOverlay,
                     UiRenderPresentationBand::Screen, extent),
            };
            REQUIRE(ValidateUiRenderComposition({View(), graph, extent, {3}, passes}).HasValue());
            REQUIRE(ValidateUiRenderComposition({View(), graph, extent, {3}, {}}).HasValue());
        }

        TEST_CASE("UI composition rejects overflow ordering duplicates and mixed views", "[renderer][runtime_ui][composition]") {
            const auto first = Snapshot(2, 2, 2);
            const auto second = Snapshot(3, 3, 3);
            constexpr RenderGraphOwnerId graph{41};
            constexpr FramebufferExtent extent{1280, 720};
            std::array passes{
                Pass(first, graph, 1, 1, UiRenderCompositionSpace::Camera, UiRenderCompositionPoint::SceneAfterPostProcess,
                     UiRenderPresentationBand::Hud, extent),
                Pass(second, graph, 2, 2, UiRenderCompositionSpace::Screen, UiRenderCompositionPoint::DisplayOverlay,
                     UiRenderPresentationBand::Screen, extent),
            };

            RequireError(ValidateUiRenderComposition({View(), graph, extent, {1}, passes}), UiErrors::RenderCompositionCapacityExceeded);
            std::swap(passes[0], passes[1]);
            RequireError(ValidateUiRenderComposition({View(), graph, extent, {2}, passes}), UiErrors::RenderCompositionInvalid);
            std::swap(passes[0], passes[1]);
            passes[1] = passes[0];
            RequireError(ValidateUiRenderComposition({View(), graph, extent, {2}, passes}), UiErrors::RenderCompositionInvalid);

            const auto otherView = Snapshot(4, 4, 4, View(10));
            passes[1] = Pass(otherView, graph, 2, 2, UiRenderCompositionSpace::Screen, UiRenderCompositionPoint::DisplayOverlay,
                             UiRenderPresentationBand::Screen, extent);
            RequireError(ValidateUiRenderComposition({View(), graph, extent, {2}, passes}), UiErrors::HandleOwnerMismatch);
        }

        TEST_CASE("UI composition validates target graph shape and space policy", "[renderer][runtime_ui][composition]") {
            const auto snapshot = Snapshot(2, 2, 2);
            constexpr RenderGraphOwnerId graph{41};
            constexpr FramebufferExtent extent{1280, 720};
            const auto world = Pass(snapshot, graph, 1, 1, UiRenderCompositionSpace::World,
                                    UiRenderCompositionPoint::SceneBeforePostProcess, UiRenderPresentationBand::World, extent);
            const auto screen = Pass(snapshot, graph, 1, 1, UiRenderCompositionSpace::Screen, UiRenderCompositionPoint::DisplayOverlay,
                                     UiRenderPresentationBand::Screen, extent);

            RequireRejectedPass(world, graph, extent, [](auto &pass) {
                pass.depth.reset();
            });
            RequireRejectedPass(screen, graph, extent, [](auto &pass) {
                pass.colorOutput.texture.format = RenderTextureFormat::Depth32Float;
            });
            RequireRejectedPass(screen, graph, extent, [](auto &pass) {
                pass.colorOutput.resource.owner = {99};
            }, UiErrors::HandleOwnerMismatch);
            RequireRejectedPass(screen, graph, extent, [](auto &pass) {
                pass.colorOutput.texture.format = RenderTextureFormat::Rgba8Unorm;
            });
            RequireRejectedPass(world, graph, extent, [](auto &pass) {
                pass.depth->resource.owner = {99};
            }, UiErrors::HandleOwnerMismatch);
        }

        TEST_CASE("only presented UI receipts advance interaction eligibility", "[renderer][runtime_ui][presentation]") {
            const auto canvas = UiCanvasInstanceId{Owner(), 2, 1};
            auto state = PresentationState(canvas);
            const auto skipped = Receipt(canvas, 2, 2, UiPresentationOutcome::Skipped, UiPresentationReason::Suppressed);
            auto applied = state.Apply(skipped);
            REQUIRE(applied.HasValue());
            REQUIRE_FALSE(applied.Value());
            REQUIRE_FALSE(state.LastPresentedInteraction().IsValid());

            const auto failed = Receipt(canvas, 3, 3, UiPresentationOutcome::Failed, UiPresentationReason::ExecutionFailure);
            applied = state.Apply(failed);
            REQUIRE(applied.HasValue());
            REQUIRE_FALSE(applied.Value());
            REQUIRE_FALSE(state.LastPresentedInteraction().IsValid());

            const auto presented = Receipt(canvas, 4, 4, UiPresentationOutcome::Presented, UiPresentationReason::None);
            applied = state.Apply(presented);
            REQUIRE(applied.HasValue());
            REQUIRE(applied.Value());
            REQUIRE(state.LastPresentedInteraction() == UiInteractionRevision::Create(4).Value());

            auto stale = presented;
            stale.snapshotRevision = UiRenderSnapshotRevision::Create(3).Value();
            RequireError(state.Apply(stale), UiErrors::RenderPresentationStale);
            REQUIRE(state.LastPresentedInteraction() == UiInteractionRevision::Create(4).Value());

            auto repeatedInteraction = presented;
            repeatedInteraction.snapshotRevision = UiRenderSnapshotRevision::Create(5).Value();
            applied = state.Apply(repeatedInteraction);
            REQUIRE(applied.HasValue());
            REQUIRE_FALSE(applied.Value());
            REQUIRE(state.LastObservedSnapshot() == UiRenderSnapshotRevision::Create(5).Value());
        }

        TEST_CASE("UI presentation receipts require consistent terminal reasons", "[renderer][runtime_ui][presentation]") {
            const auto canvas = UiCanvasInstanceId{Owner(), 2, 1};
            auto state = PresentationState(canvas);
            auto invalid = Receipt(canvas, 2, 2, UiPresentationOutcome::Presented, UiPresentationReason::ExecutionFailure);
            RequireError(state.Apply(invalid), UiErrors::RenderPresentationInvalid);
            invalid.outcome = static_cast<UiPresentationOutcome>(99);
            RequireError(state.Apply(invalid), UiErrors::RenderPresentationInvalid);

            const auto foreignCanvas = UiCanvasInstanceId{Owner(), 3, 1};
            invalid = Receipt(foreignCanvas, 2, 2, UiPresentationOutcome::Presented, UiPresentationReason::None);
            RequireError(state.Apply(invalid), UiErrors::HandleOwnerMismatch);
        }
    }  // namespace
}  // namespace Horo::Render
