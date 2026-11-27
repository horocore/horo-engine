#include "Horo/Runtime/Ui/UiRenderSnapshot.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <type_traits>

namespace {
    std::atomic<std::size_t> snapshotAllocations{};
}

void *operator new(const std::size_t size) {
    snapshotAllocations.fetch_add(1, std::memory_order_relaxed);
    void *memory = std::malloc(size);
    if (memory == nullptr)
        throw std::bad_alloc{};
    return memory;
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    operator delete(memory);
}

namespace Horo::Runtime::Ui {
    namespace {
        SerializedUiId StableBytes(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return bytes;
        }

        UiDocumentId Document(const std::uint8_t marker) {
            return UiDocumentId::Create(StableBytes(marker)).Value();
        }

        UiElementId Element(const std::uint8_t marker) {
            return UiElementId::Create(StableBytes(marker)).Value();
        }

        UiOwnershipGeneration Owner(const std::uint64_t value = 17) {
            return UiOwnershipGeneration::Create(value).Value();
        }

        UiDocumentRevision DocumentRevision(const std::uint64_t value = 4) {
            return UiDocumentRevision::Create(value).Value();
        }

        UiRuntimeTreeRevision TreeRevision(const std::uint64_t value = 6) {
            return UiRuntimeTreeRevision::Create(value).Value();
        }

        UiInteractionRevision InteractionRevision(const std::uint64_t value = 8) {
            return UiInteractionRevision::Create(value).Value();
        }

        UiRenderSnapshotRevision SnapshotRevision(const std::uint64_t value = 2) {
            return UiRenderSnapshotRevision::Create(value).Value();
        }

        UiRenderResourceRevision ResourceRevision(const std::uint64_t value = 3) {
            return UiRenderResourceRevision::Create(value).Value();
        }

        Assets::AssetId Asset(const std::uint8_t marker) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = marker;
            return Assets::AssetId::FromBytes(bytes);
        }

        UiElementTreeDescriptor TreeDescriptor(const std::uint32_t canvasSlot = 2) {
            return {.instance = {Owner(), 1, 1},
                    .canvas = {Owner(), canvasSlot, 1},
                    .document = Document(1),
                    .documentRevision = DocumentRevision(),
                    .treeRevision = TreeRevision(),
                    .limits = {4, 4, 4}};
        }

        UiElementTree Tree() {
            const auto descriptor = TreeDescriptor();
            const std::array elements{UiElementDescriptor{Element(2), {}}, UiElementDescriptor{Element(3), Element(2)}};
            auto allocatorResult = UiElementSlotAllocator::Create(Owner());
            REQUIRE(allocatorResult.HasValue());
            auto allocator = std::move(allocatorResult).Value();
            auto result = UiElementTree::Create(allocator, descriptor, elements);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        UiRenderSnapshotLimits Limits() {
            return {8, 4, 16, 4, 4, 4, 4};
        }

        UiRenderSnapshotDescriptor Descriptor(const UiElementTree &tree) {
            return {.instance = tree.Instance(),
                    .canvas = tree.Canvas(),
                    .document = tree.SourceDocument(),
                    .documentRevision = tree.SourceDocumentRevision(),
                    .treeRevision = tree.Revision(),
                    .interactionRevision = InteractionRevision(),
                    .snapshotRevision = SnapshotRevision(),
                    .view = {Owner(), 9, 1},
                    .limits = Limits()};
        }

        UiRenderExtractor Extractor(const std::uint32_t concurrentSnapshots = 2, const bool measureCpuWork = false) {
            auto result = UiRenderExtractor::Create({{Owner(), 9, 1}, Limits(), concurrentSnapshots, measureCpuWork});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        struct Projection final {
            std::array<UiDrawCommand, 4> commands;
            std::array<UiLogicalTransform, 2> transforms;
            std::array<UiRenderResourceReference, 3> resources;
            std::array<UiPositionedGlyph, 2> glyphs;
            std::array<UiClip, 2> clips;
            std::array<UiTextRun, 1> textRuns;
            std::array<UiMask, 1> masks;

            UiRenderProjection View() const noexcept {
                return {commands, textRuns, glyphs, clips, masks, transforms, resources};
            }
        };

        Projection CompleteProjection(const UiElementTree &tree) {
            Projection result;
            result.resources = std::array{UiRenderResourceReference{Asset(1), ResourceRevision(), UiRenderResourceRole::Image},
                                          UiRenderResourceReference{Asset(2), ResourceRevision(), UiRenderResourceRole::FontFace},
                                          UiRenderResourceReference{Asset(3), ResourceRevision(), UiRenderResourceRole::Mask}};
            result.transforms = std::array{UiLogicalTransform{}, UiLogicalTransform{{1.0F, 0.0F, 0.0F, 1.0F, 64.0F, 32.0F}}};
            result.clips = std::array{UiClip{{{0, 0}, {640, 320}}, NoUiRenderIndex}, UiClip{{{64, 32}, {320, 160}}, 0}};
            result.masks = std::array{UiMask{{{0, 0}, {640, 320}}, 2, 0}};
            result.glyphs = std::array{UiPositionedGlyph{17, 0, {64, 64}}, UiPositionedGlyph{18, 1, {128, 64}}};
            result.textRuns = std::array{UiTextRun{1, 0, 2, {1.0F, 1.0F, 1.0F, 1.0F}}};
            const auto root = tree.Root().Value().handle;
            const auto child = tree.Find(Element(3)).Value();
            result.commands =
                std::array{UiDrawCommand{root, {{0, 0}, {640, 320}}, 0, 0, NoUiRenderIndex, 1.0F, UiSolidDraw{{0.1F, 0.2F, 0.3F, 1.0F}}},
                           UiDrawCommand{root,
                                         {{0, 0}, {640, 320}},
                                         0,
                                         0,
                                         NoUiRenderIndex,
                                         1.0F,
                                         UiBorderDraw{{1.0F, 1.0F, 1.0F, 1.0F}, 2}},
                           UiDrawCommand{child, {{64, 32}, {128, 64}}, 1, 1, 0, 0.75F, UiImageDraw{0, {1.0F, 1.0F, 1.0F, 1.0F}}},
                           UiDrawCommand{child, {{64, 96}, {256, 64}}, 0, 1, NoUiRenderIndex, 1.0F, UiTextDraw{0}}};
            return result;
        }

        Result<UiRenderSnapshot> Extract(UiRenderExtractor &extractor, const UiElementTree &tree,
                                         const UiRenderSnapshotDescriptor &descriptor, const Projection &projection) {
            return extractor.Extract(tree, descriptor, projection.View());
        }

        template <typename Value> void RequireError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }

        TEST_CASE("UI render extraction owns complete stable paint-order values", "[runtime_ui][render_snapshot]") {
            auto tree = Tree();
            auto extractor = Extractor();
            auto projection = CompleteProjection(tree);
            auto extracted = Extract(extractor, tree, Descriptor(tree), projection);
            REQUIRE(extracted.HasValue());
            auto snapshot = std::move(extracted).Value();
            REQUIRE(snapshot.Commands().size() == 4);
            REQUIRE(std::holds_alternative<UiSolidDraw>(snapshot.Commands()[0].payload));
            REQUIRE(std::holds_alternative<UiBorderDraw>(snapshot.Commands()[1].payload));
            REQUIRE(std::holds_alternative<UiImageDraw>(snapshot.Commands()[2].payload));
            REQUIRE(std::holds_alternative<UiTextDraw>(snapshot.Commands()[3].payload));
            REQUIRE(snapshot.TextRuns()[0].glyphCount == 2);
            REQUIRE(snapshot.Clips()[1].parent == 0);
            REQUIRE(snapshot.Resources()[1].role == UiRenderResourceRole::FontFace);

            projection.commands[0].opacity = 0.25F;
            projection.glyphs[0].glyph = 999;
            tree.Shutdown();
            REQUIRE(snapshot.Commands().size() == 4);
            REQUIRE(snapshot.Glyphs()[0].glyph == 17);
            REQUIRE(snapshot.Descriptor().snapshotRevision == SnapshotRevision());
        }

        TEST_CASE("UI render extraction requires exact live tree and view evidence", "[runtime_ui][render_snapshot]") {
            auto tree = Tree();
            auto extractor = Extractor();
            const auto projection = CompleteProjection(tree);
            auto descriptor = Descriptor(tree);
            descriptor.instance.slot = 99;
            REQUIRE(Extract(extractor, tree, descriptor, projection).HasError());
            descriptor = Descriptor(tree);
            descriptor.documentRevision = DocumentRevision(99);
            REQUIRE(Extract(extractor, tree, descriptor, projection).HasError());
            descriptor = Descriptor(tree);
            descriptor.interactionRevision = {};
            REQUIRE(Extract(extractor, tree, descriptor, projection).HasError());
            descriptor = Descriptor(tree);
            descriptor.view.ownership = Owner(18);
            REQUIRE(Extract(extractor, tree, descriptor, projection).HasError());
            REQUIRE(tree.BeginRetirement().HasValue());
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
        }

        TEST_CASE("UI render extraction validates resource and text ranges", "[runtime_ui][render_snapshot]") {
            auto tree = Tree();
            auto extractor = Extractor();
            auto projection = CompleteProjection(tree);
            projection.resources[0].asset = {};
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
            projection = CompleteProjection(tree);
            projection.resources[0].role = static_cast<UiRenderResourceRole>(99);
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
            projection = CompleteProjection(tree);
            projection.textRuns[0].fontResource = 0;
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
            projection = CompleteProjection(tree);
            projection.textRuns[0].firstGlyph = std::numeric_limits<std::uint32_t>::max();
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
            projection = CompleteProjection(tree);
            projection.textRuns[0].color.red = -0.1F;
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
        }

        TEST_CASE("UI render extraction validates clips masks and finite transforms", "[runtime_ui][render_snapshot]") {
            auto tree = Tree();
            auto extractor = Extractor();
            auto projection = CompleteProjection(tree);
            projection.clips[0].parent = 1;
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
            projection = CompleteProjection(tree);
            projection.clips[0].rect.extent.width = -1;
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
            projection = CompleteProjection(tree);
            projection.masks[0].resource = 0;
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
            projection = CompleteProjection(tree);
            projection.masks[0].transform = 99;
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
            projection = CompleteProjection(tree);
            projection.transforms[0].values[0] = std::numeric_limits<float>::infinity();
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
        }

        TEST_CASE("UI render extraction validates every draw reference and paint value", "[runtime_ui][render_snapshot]") {
            auto tree = Tree();
            auto extractor = Extractor();
            auto projection = CompleteProjection(tree);
            projection.commands[0].element.ownership = Owner(19);
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
            projection = CompleteProjection(tree);
            projection.commands[0].transform = 99;
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
            projection = CompleteProjection(tree);
            projection.commands[0].opacity = 2.0F;
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
            projection = CompleteProjection(tree);
            std::get<UiBorderDraw>(projection.commands[1].payload).width = -1;
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
            projection = CompleteProjection(tree);
            std::get<UiImageDraw>(projection.commands[2].payload).resource = 1;
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
            projection = CompleteProjection(tree);
            std::get<UiTextDraw>(projection.commands[3].payload).run = 99;
            REQUIRE(Extract(extractor, tree, Descriptor(tree), projection).HasError());
        }

        TEST_CASE("UI render extraction enforces declared bounds and permits an empty view", "[runtime_ui][render_snapshot]") {
            auto tree = Tree();
            auto extractor = Extractor();
            auto projection = CompleteProjection(tree);
            auto descriptor = Descriptor(tree);
            descriptor.limits.commands = static_cast<std::uint32_t>(projection.commands.size());
            REQUIRE(Extract(extractor, tree, descriptor, projection).HasValue());
            --descriptor.limits.commands;
            REQUIRE(Extract(extractor, tree, descriptor, projection).HasError());
            descriptor = Descriptor(tree);
            descriptor.limits.transforms = 0;
            REQUIRE(Extract(extractor, tree, descriptor, projection).HasError());

            auto emptyExtractor = Extractor();
            REQUIRE(emptyExtractor.Extract(tree, Descriptor(tree), {}).HasValue());
        }

        TEST_CASE("UI render extraction rejects another canvas element within one owner generation", "[runtime_ui][render_snapshot]") {
            auto allocatorResult = UiElementSlotAllocator::Create(Owner());
            REQUIRE(allocatorResult.HasValue());
            auto allocator = std::move(allocatorResult).Value();
            const std::array elements{UiElementDescriptor{Element(2), {}}, UiElementDescriptor{Element(3), Element(2)}};
            const auto firstDescriptor = TreeDescriptor();
            auto firstResult = UiElementTree::Create(allocator, firstDescriptor, elements);
            REQUIRE(firstResult.HasValue());
            auto first = std::move(firstResult).Value();
            const auto secondDescriptor = TreeDescriptor(3);
            auto secondResult = UiElementTree::Create(allocator, secondDescriptor, elements);
            REQUIRE(secondResult.HasValue());
            auto second = std::move(secondResult).Value();
            auto projection = CompleteProjection(second);
            projection.commands[0].element = first.Root().Value().handle;
            auto descriptor = Descriptor(second);
            descriptor.view = {Owner(), 10, 1};
            auto extractorResult = UiRenderExtractor::Create({descriptor.view, Limits(), 1});
            REQUIRE(extractorResult.HasValue());
            auto extractor = std::move(extractorResult).Value();
            REQUIRE(Extract(extractor, second, descriptor, projection).HasError());
        }

        TEST_CASE("UI render extractor bounds leases revisions and shutdown", "[runtime_ui][render_snapshot][lifecycle]") {
            auto tree = Tree();
            const auto projection = CompleteProjection(tree);
            auto extractor = Extractor(1);
            auto descriptor = Descriptor(tree);
            std::optional<UiRenderSnapshot> retained;
            {
                auto firstResult = Extract(extractor, tree, descriptor, projection);
                REQUIRE(firstResult.HasValue());
                retained.emplace(firstResult.Value());
            }
            descriptor.snapshotRevision = SnapshotRevision(3);
            const auto exhausted = Extract(extractor, tree, descriptor, projection);
            RequireError(exhausted, UiErrors::RenderSnapshotStorageExhausted);
            REQUIRE_FALSE(extractor.IsDrained());
            retained.reset();
            REQUIRE(extractor.IsDrained());
            auto reused = Extract(extractor, tree, descriptor, projection);
            REQUIRE(reused.HasValue());
            auto duplicate = Descriptor(tree);
            duplicate.snapshotRevision = SnapshotRevision(3);
            RequireError(Extract(extractor, tree, duplicate, projection), UiErrors::RevisionStale);
            REQUIRE(reused.Value().Masks().size() == 1);
            REQUIRE(reused.Value().Transforms().size() == 2);
            extractor.Close();
            extractor.Close();
            REQUIRE(extractor.State() == UiRenderExtractorState::Closed);
            descriptor.snapshotRevision = SnapshotRevision(4);
            RequireError(Extract(extractor, tree, descriptor, projection), UiErrors::RenderSnapshotLifecycleUnavailable);
            REQUIRE_FALSE(extractor.IsDrained());
        }

        TEST_CASE("UI render snapshot assignments preserve exact leases across two slots", "[runtime_ui][render_snapshot][lifecycle]") {
            auto tree = Tree();
            const auto projection = CompleteProjection(tree);
            auto extractor = Extractor(2);
            auto descriptor = Descriptor(tree);

            auto firstResult = Extract(extractor, tree, descriptor, projection);
            REQUIRE(firstResult.HasValue());
            std::optional<UiRenderSnapshot> first{std::move(firstResult).Value()};
            descriptor.snapshotRevision = SnapshotRevision(3);
            auto secondResult = Extract(extractor, tree, descriptor, projection);
            REQUIRE(secondResult.HasValue());
            std::optional<UiRenderSnapshot> second{std::move(secondResult).Value()};

            descriptor.snapshotRevision = SnapshotRevision(4);
            RequireError(Extract(extractor, tree, descriptor, projection), UiErrors::RenderSnapshotStorageExhausted);

            std::optional<UiRenderSnapshot> assigned{*first};
            *assigned = *assigned;
            *assigned = *second;
            REQUIRE(assigned->Descriptor().snapshotRevision == SnapshotRevision(3));
            *first = std::move(*second);
            *first = std::move(*first);
            REQUIRE(first->Descriptor().snapshotRevision == SnapshotRevision(3));

            auto reused = Extract(extractor, tree, descriptor, projection);
            REQUIRE(reused.HasValue());
            REQUIRE(reused.Value().Descriptor().snapshotRevision == SnapshotRevision(4));
            REQUIRE_FALSE(extractor.IsDrained());
            assigned.reset();
            first.reset();
            second.reset();
        }

        TEST_CASE("UI render extractor validates store bounds and exact view", "[runtime_ui][render_snapshot][limits]") {
            auto invalid = UiRenderExtractorDescriptor{{}, Limits(), 1};
            REQUIRE_FALSE(invalid.IsValid());
            RequireError(UiRenderExtractor::Create(invalid), UiErrors::HandleMalformed);
            invalid = {{Owner(), 9, 1}, Limits(), 0};
            REQUIRE_FALSE(invalid.IsValid());
            RequireError(UiRenderExtractor::Create(invalid), UiErrors::CapacityExceeded);
            invalid.concurrentSnapshots = MaximumUiRenderSnapshotsInFlight + 1;
            RequireError(UiRenderExtractor::Create(invalid), UiErrors::CapacityExceeded);

            auto tree = Tree();
            const auto projection = CompleteProjection(tree);
            auto reserved = Limits();
            --reserved.commands;
            auto extractorResult = UiRenderExtractor::Create({{Owner(), 9, 1}, reserved, 1});
            REQUIRE(extractorResult.HasValue());
            auto extractor = std::move(extractorResult).Value();
            RequireError(Extract(extractor, tree, Descriptor(tree), projection), UiErrors::CapacityExceeded);

            auto exact = Extractor();
            auto wrongView = Descriptor(tree);
            wrongView.view.slot = 10;
            RequireError(Extract(exact, tree, wrongView, projection), UiErrors::HandleOwnerMismatch);
        }

        TEST_CASE("UI render snapshot outlives its closed extractor and source tree", "[runtime_ui][render_snapshot][shutdown]") {
            auto tree = Tree();
            auto projection = CompleteProjection(tree);
            std::optional<UiRenderSnapshot> retained;
            {
                auto extractor = Extractor(1);
                auto result = Extract(extractor, tree, Descriptor(tree), projection);
                REQUIRE(result.HasValue());
                retained.emplace(std::move(result).Value());
                extractor.Close();
                REQUIRE_FALSE(extractor.IsDrained());
            }
            projection.glyphs[0].glyph = 999;
            tree.Shutdown();
            REQUIRE(retained->Glyphs()[0].glyph == 17);
            const UiRenderViewId expectedView{Owner(), 9, 1};
            REQUIRE(retained->Descriptor().view == expectedView);
            retained.reset();
        }

        TEST_CASE("UI render extractor performs no steady-state allocation", "[runtime_ui][render_snapshot][realtime]") {
            auto tree = Tree();
            const auto projection = CompleteProjection(tree);
            bool measureCpuWork{};
            SECTION("timing disabled") {}
            SECTION("timing enabled") {
                measureCpuWork = true;
            }
            auto extractor = Extractor(1, measureCpuWork);
            auto descriptor = Descriptor(tree);
            for (std::uint64_t revision = 2; revision < 34; ++revision) {
                descriptor.snapshotRevision = SnapshotRevision(revision);
                const auto before = snapshotAllocations.load(std::memory_order_relaxed);
                const auto extracted = Extract(extractor, tree, descriptor, projection);
                const auto after = snapshotAllocations.load(std::memory_order_relaxed);
                REQUIRE(after == before);
                REQUIRE(extracted.HasValue());
                REQUIRE(extracted.Value().Commands().size() == projection.commands.size());
            }
            const auto timing = extractor.CpuWorkStatistics();
            CHECK(timing.enabled == measureCpuWork);
            CHECK(timing.attempts == (measureCpuWork ? 32 : 0));
            CHECK(timing.succeeded == timing.attempts);
        }

        static_assert(std::is_copy_constructible_v<UiRenderSnapshot>);
        static_assert(!std::is_copy_constructible_v<UiRenderExtractor>);
    }  // namespace
}  // namespace Horo::Runtime::Ui
