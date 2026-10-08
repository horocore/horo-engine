#include "Horo/Runtime/Ui/UiTheme.h"
#include "UiTestUtils.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <optional>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        using Test::Stable;

        template <typename T> T Rev(const std::uint64_t value = 1) {
            return T::Create(value).Value();
        }

        UiOwnershipGeneration Owner() {
            return UiOwnershipGeneration::Create(729).Value();
        }

        RuntimeStyleAssetId Theme(const std::uint8_t value = 10) {
            return Stable<RuntimeStyleAssetId>(value);
        }

        UiStylePropertyId Property(const std::uint8_t value) {
            return Stable<UiStylePropertyId>(value);
        }

        UiStyleTokenReference Token() {
            return {Theme(), Stable<UiStyleTokenId>(1)};
        }

        UiStyleColor Color(const float red) {
            return {red, 0.2F, 0.3F, 1.0F};
        }

        UiStyleAssignment Literal(const std::uint8_t property, UiStyleValue value) {
            return {Property(property), UiStyleValueSource::Literal(std::move(value)), false};
        }

        void ErrorIs(const auto &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }

        UiStyleRegistryDefinition Definition(const float red = 0.2F, const std::int32_t width = 128) {
            UiStyleRegistryDefinition result;
            for (std::uint8_t index = 1; index <= 3; ++index)
                result.properties.push_back({Property(index), UiStyleValueCategory::Dimension, UiStyleDimension{}, {.measure = true}});
            result.properties.push_back({Property(4), UiStyleValueCategory::Color, Color(0.0F), {.paint = true}});
            result.properties.push_back({Property(5), UiStyleValueCategory::Scalar, UiStyleScalar{1.0F}, {.paint = true}});
            for (std::uint8_t index = 10; index <= 11; ++index) {
                UiStyleAssetDefinition asset{.id = Theme(index)};
                asset.tokens.push_back({Stable<UiStyleTokenId>(1), UiStyleValueCategory::Color,
                                        UiStyleValueSource::Literal(Color(index == 10 ? red : 0.8F)), false});
                asset.assignments = {Literal(1, UiStyleDimension{320}),
                                     Literal(2, UiStyleDimension{64}),
                                     Literal(3, UiStyleDimension{8}),
                                     {Property(4), UiStyleValueSource::Token({asset.id, Stable<UiStyleTokenId>(1)}), false}};
                asset.classes.push_back(
                    {.id = Stable<UiStyleClassId>(7), .assignments = {Literal(1, UiStyleDimension{index == 10 ? width : 192})}});
                result.assets.push_back(std::move(asset));
            }
            return result;
        }

        UiThemeRuntimeDescriptor Descriptor(const std::uint32_t renderSlots = 5) {
            const RuntimeUiInstanceId instance{Owner(), 1, 1};
            const UiCanvasInstanceId canvas{Owner(), 2, 1};
            const auto document = Stable<UiDocumentId>(1);
            return {{instance, canvas, document, 2, 5, 4, 5, Rev<RuntimeStyleGeneration>(), Rev<UiStylePublicationRevision>()},
                    {instance, canvas, document, 2, 4, 5, Rev<UiInteractionRevision>()},
                    {{Owner(), 9, 1}, {2, 1, 1, 1, 1, 1, 1}, renderSlots},
                    {Property(1), Property(2), Property(3), Property(4), Property(5)},
                    2};
        }

        UiElementTree Tree() {
            auto slots = UiElementSlotAllocator::Create(Owner());
            REQUIRE(slots.HasValue());
            auto allocator = std::move(slots).Value();
            const std::array elements{UiElementDescriptor{Stable<UiElementId>(1), {}},
                                      UiElementDescriptor{Stable<UiElementId>(2), Stable<UiElementId>(1)}};
            const auto owner = Descriptor();
            auto tree = UiElementTree::Create(allocator,
                                              {owner.style.instance,
                                               owner.style.canvas,
                                               owner.style.document,
                                               Rev<UiDocumentRevision>(),
                                               Rev<UiRuntimeTreeRevision>(),
                                               {2, 4, 4}},
                                              elements);
            REQUIRE(tree.HasValue());
            return std::move(tree).Value();
        }

        UiThemeRuntime Runtime(const std::uint32_t renderSlots = 5) {
            auto registry = RuntimeStyleRegistry::Create(Definition(), Rev<RuntimeStyleGeneration>());
            REQUIRE(registry.HasValue());
            auto owner = UiThemeRuntime::Create(Descriptor(renderSlots), std::move(registry).Value(), Theme());
            REQUIRE(owner.HasValue());
            return std::move(owner).Value();
        }

        struct Frame final {
            std::array<UiStyleClassReference, 1> classes{{{{}, Stable<UiStyleClassId>(7)}}};
            std::array<UiThemeSurfaceInput, 2> surfaces;
            UiThemeUpdateRequest request;

            explicit Frame(const UiElementTree &tree) {
                surfaces[0].style.element = tree.Root().Value().handle;
                surfaces[1].style.element = tree.Find(Stable<UiElementId>(2)).Value();
                surfaces[1].style.classes = classes;
                request.styleSources = {tree.SourceDocumentRevision(),
                                        tree.Revision(),
                                        {},
                                        Rev<UiStyleContentRevision>(),
                                        Rev<UiStylePolicyRevision>(),
                                        Rev<UiInteractionRevision>()};
                request.layout = {{tree.SourceDocumentRevision(), tree.Revision(), Rev<UiLayoutContentRevision>(),
                                   Rev<UiLayoutStyleRevision>(), Rev<UiLayoutIntrinsicRevision>(), Rev<UiLayoutCanvasRevision>(),
                                   Rev<UiLayoutPolicyRevision>()},
                                  {{0, 0}, {6400, 6400}},
                                  {{0, 0}, {6400, 6400}},
                                  nullptr,
                                  {}};
                request.surfaces = surfaces;
            }
        };

        float Red(const UiThemeSnapshot &snapshot) {
            return std::get<UiSolidDraw>(snapshot.render.Commands()[1].payload).color.red;
        }

        TEST_CASE("Theme switch updates real surface layout and paint without replacing retained elements", "[runtime_ui][theme]") {
            auto tree = Tree();
            auto owner = Runtime();
            Frame frame{tree};
            auto first = owner.Update(tree, frame.request);
            REQUIRE(first.HasValue());
            const auto child = frame.surfaces[1].style.element;
            CHECK(first.Value().layout.Get(child).Value().arrangement.contentBox.extent.width == 128);
            CHECK(Red(first.Value()) == 0.2F);
            const auto treeRevision = tree.Revision();
            frame.surfaces[1].style.state = UiVisualStateMask{UiVisualState::Pressed};
            const auto change = owner.BeginChange().Value();
            REQUIRE(owner.Prepare(change, Definition(), Theme(11)).HasValue());
            frame.request.change = change;
            auto next = owner.Update(tree, frame.request);
            REQUIRE(next.HasValue());
            CHECK(next.Value().theme == Theme(11));
            CHECK(next.Value().layout.Get(child).Value().arrangement.contentBox.extent.width == 192);
            CHECK(Red(next.Value()) == 0.8F);
            CHECK(next.Value().style.Get(child).Value().state == frame.surfaces[1].style.state);
            CHECK(next.Value().render.Descriptor().interactionRevision == next.Value().layout.Descriptor().interaction);
            CHECK(tree.Revision() == treeRevision);
            CHECK(tree.Find(Stable<UiElementId>(2)).Value() == child);
            CHECK(Red(first.Value()) == 0.2F);
            CHECK(first.Value().layout.Get(child).Value().arrangement.contentBox.extent.width == 128);
            ErrorIs(owner.Update(tree, frame.request), UiErrors::StyleSourceStale);
        }

        TEST_CASE("Paint-only hot reload reuses geometry and frame updates allocate no storage", "[runtime_ui][theme][allocation]") {
            auto tree = Tree();
            auto owner = Runtime();
            Frame frame{tree};
            auto first = owner.Update(tree, frame.request);
            REQUIRE(first.HasValue());
            const auto change = owner.BeginChange().Value();
            REQUIRE(owner.Prepare(change, Definition(0.7F), Theme()).HasValue());
            frame.request.change = change;
            Horo::Tests::AllocationProbe::Measurement allocations;
            auto next = [&] {
                Horo::Tests::AllocationProbe::ScopedMeasurement measure;
                auto result = owner.Update(tree, frame.request);
                allocations = measure.Snapshot();
                return result;
            }();
            REQUIRE(next.HasValue());
            CHECK(allocations.requests == 0);
            CHECK(Red(next.Value()) == 0.7F);
            CHECK(next.Value().layout.Descriptor().interaction == first.Value().layout.Descriptor().interaction);
            CHECK(next.Value().layout.Descriptor().sources.style == first.Value().layout.Descriptor().sources.style);
            CHECK(next.Value().style.Descriptor().sources.registry != first.Value().style.Descriptor().sources.registry);
            frame.request.change.reset();
            auto unchanged = [&] {
                Horo::Tests::AllocationProbe::ScopedMeasurement measure;
                auto result = owner.Update(tree, frame.request);
                allocations = measure.Snapshot();
                return result;
            }();
            REQUIRE(unchanged.HasValue());
            CHECK(allocations.requests == 0);
        }

        TEST_CASE("Removed theme tokens require explicit typed fallback and preserve last good on failure", "[runtime_ui][theme][reload]") {
            auto tree = Tree();
            auto owner = Runtime();
            Frame frame{tree};
            auto first = owner.Update(tree, frame.request);
            REQUIRE(first.HasValue());
            const auto change = owner.BeginChange().Value();
            auto removed = Definition();
            removed.assets[0].tokens.clear();
            ErrorIs(owner.Prepare(change, removed, Theme()), UiErrors::StyleReferenceInvalid);
            CHECK(Red(*owner.Current()) == 0.2F);
            frame.request.change = change;
            ErrorIs(owner.Update(tree, frame.request), UiErrors::StyleLifecycleUnavailable);
            const std::array wrong{UiThemeTokenFallback{Token(), UiStyleDimension{64}}};
            ErrorIs(owner.Prepare(change, removed, Theme(), wrong), UiErrors::StyleTypeMismatch);
            const std::array fallback{UiThemeTokenFallback{Token(), Color(0.6F)}};
            REQUIRE(owner.Prepare(change, removed, Theme(), fallback).HasValue());
            auto next = owner.Update(tree, frame.request);
            REQUIRE(next.HasValue());
            CHECK(Red(next.Value()) == 0.6F);
            CHECK(next.Value().fallbackTokens == 1);
            CHECK(Red(first.Value()) == 0.2F);
        }

        TEST_CASE("Failed theme layout and render candidates never replace the public generation", "[runtime_ui][theme][failure]") {
            auto tree = Tree();
            auto owner = Runtime(2);
            Frame frame{tree};
            std::optional<UiThemeSnapshot> first;
            {
                auto result = owner.Update(tree, frame.request);
                REQUIRE(result.HasValue());
                first = std::move(result).Value();
            }
            const auto change = owner.BeginChange().Value();
            REQUIRE(owner.Prepare(change, Definition(0.5F, 256), Theme()).HasValue());
            frame.request.change = change;
            frame.request.layout.rootConstraints.maximum = {-1, 6400};
            REQUIRE(owner.Update(tree, frame.request).HasError());
            CHECK(Red(*owner.Current()) == 0.2F);
            frame.request.layout.rootConstraints.maximum = {6400, 6400};
            auto next = owner.Update(tree, frame.request);
            REQUIRE(next.HasValue());
            CHECK(Red(next.Value()) == 0.5F);
            const auto reload = owner.BeginChange().Value();
            REQUIRE(owner.Prepare(reload, Definition(0.9F, 512), Theme()).HasValue());
            frame.request.change = reload;
            ErrorIs(owner.Update(tree, frame.request), UiErrors::RenderSnapshotStorageExhausted);
            CHECK(Red(*owner.Current()) == 0.5F);
            CHECK(owner.Current()->layout.Get(frame.surfaces[1].style.element).Value().arrangement.contentBox.extent.width == 256);
            first.reset();
            auto recovered = owner.Update(tree, frame.request);
            REQUIRE(recovered.HasValue());
            CHECK(Red(recovered.Value()) == 0.9F);
            CHECK(recovered.Value().layout.Get(frame.surfaces[1].style.element).Value().arrangement.contentBox.extent.width == 512);
        }

        TEST_CASE("Cancelled and superseded theme requests reject late preparation and publication", "[runtime_ui][theme][lifecycle]") {
            auto tree = Tree();
            auto owner = Runtime();
            Frame frame{tree};
            auto first = owner.Update(tree, frame.request);
            REQUIRE(first.HasValue());
            const auto cancelled = owner.BeginChange().Value();
            REQUIRE(owner.Prepare(cancelled, Definition(0.9F), Theme()).HasValue());
            REQUIRE(owner.Cancel(cancelled).HasValue());
            ErrorIs(owner.Prepare(cancelled, Definition(), Theme()), UiErrors::StyleSourceStale);
            frame.request.change = cancelled;
            ErrorIs(owner.Update(tree, frame.request), UiErrors::StyleSourceStale);
            const auto superseded = owner.BeginChange().Value();
            const auto replacement = owner.BeginChange().Value();
            CHECK(replacement != superseded);
            ErrorIs(owner.Cancel(superseded), UiErrors::StyleSourceStale);
            auto foreign = replacement;
            foreign.canvas.generation = 2;
            ErrorIs(owner.Prepare(foreign, Definition(), Theme()), UiErrors::StyleSourceStale);
            REQUIRE(owner.BeginRetirement().HasValue());
            ErrorIs(owner.Prepare(replacement, Definition(), Theme()), UiErrors::StyleLifecycleUnavailable);
            ErrorIs(owner.Update(tree, frame.request), UiErrors::StyleLifecycleUnavailable);
            CHECK(Red(*owner.Current()) == 0.2F);
            owner.Shutdown();
            owner.Shutdown();
            CHECK(owner.Current() == nullptr);
            CHECK_FALSE(owner.IsDrained());
            CHECK(Red(first.Value()) == 0.2F);
            first = Result<UiThemeSnapshot>::Failure(MakeError(UiErrors::StyleLifecycleUnavailable));
            CHECK(owner.IsDrained());
        }

        TEST_CASE("Repreparing a failed theme request never reuses the candidate registry identity", "[runtime_ui][theme][failure]") {
            auto tree = Tree();
            auto owner = Runtime();
            Frame frame{tree};
            REQUIRE(owner.Update(tree, frame.request).HasValue());
            const auto change = owner.BeginChange().Value();
            REQUIRE(owner.Prepare(change, Definition(0.7F), Theme()).HasValue());
            frame.request.change = change;
            frame.surfaces[1].layout.pivot.x = -1;
            REQUIRE(owner.Update(tree, frame.request).HasError());
            CHECK(Red(*owner.Current()) == 0.2F);
            REQUIRE(owner.Prepare(change, Definition(0.9F), Theme()).HasValue());
            frame.surfaces[1].layout.pivot.x = 32;
            auto snapshot = owner.Update(tree, frame.request);
            REQUIRE(snapshot.HasValue());
            CHECK(Red(snapshot.Value()) == 0.9F);
        }

        TEST_CASE("Theme preparation rejects malformed replacements and undeclared fallback", "[runtime_ui][theme][failure]") {
            auto tree = Tree();
            auto owner = Runtime();
            Frame frame{tree};
            REQUIRE(owner.Update(tree, frame.request).HasValue());
            const auto change = owner.BeginChange().Value();
            auto definition = Definition();
            SECTION("token cycle") {
                definition.assets[0].tokens[0].value = UiStyleValueSource::Token(Token());
                ErrorIs(owner.Prepare(change, std::move(definition), Theme()), UiErrors::StyleCycle);
            }
            SECTION("consumer category changed") {
                definition.properties[0] = {Property(1), UiStyleValueCategory::Color, Color(0.0F), {.paint = true}};
                definition.assets[0].assignments[0] = Literal(1, Color(0.0F));
                definition.assets[1].assignments[0] = Literal(1, Color(0.0F));
                definition.assets[0].classes.clear();
                definition.assets[1].classes.clear();
                ErrorIs(owner.Prepare(change, std::move(definition), Theme()), UiErrors::StyleTypeMismatch);
            }
            SECTION("missing selected asset") {
                ErrorIs(owner.Prepare(change, std::move(definition), Theme(99)), UiErrors::StyleReferenceInvalid);
            }
            SECTION("fallback would overwrite an existing token") {
                const std::array fallbacks{UiThemeTokenFallback{Token(), Color(0.6F)}};
                ErrorIs(owner.Prepare(change, std::move(definition), Theme(), fallbacks), UiErrors::StyleReferenceInvalid);
            }
            SECTION("unknown fallback token") {
                const std::array fallbacks{UiThemeTokenFallback{{Theme(), Stable<UiStyleTokenId>(99)}, Color(0.6F)}};
                ErrorIs(owner.Prepare(change, std::move(definition), Theme(), fallbacks), UiErrors::StyleReferenceInvalid);
            }
            SECTION("duplicate fallbacks") {
                definition.assets[0].tokens.clear();
                const std::array fallbacks{UiThemeTokenFallback{Token(), Color(0.6F)}, UiThemeTokenFallback{Token(), Color(0.7F)}};
                ErrorIs(owner.Prepare(change, std::move(definition), Theme(), fallbacks), UiErrors::StyleInvalid);
            }
            CHECK(Red(*owner.Current()) == 0.2F);
        }

        TEST_CASE("Theme update validates complete element and provider inputs without partial publication",
                  "[runtime_ui][theme][failure]") {
            auto tree = Tree();
            auto owner = Runtime();
            Frame frame{tree};
            const auto originalChild = frame.surfaces[1].style.element;
            REQUIRE(owner.Update(tree, frame.request).HasValue());
            const auto change = owner.BeginChange().Value();
            REQUIRE(owner.Prepare(change, Definition(0.8F), Theme()).HasValue());
            frame.request.change = change;
            SECTION("missing class in replacement") {
                auto definition = Definition();
                definition.assets[0].classes.clear();
                REQUIRE(owner.Prepare(change, std::move(definition), Theme()).HasValue());
                ErrorIs(owner.Update(tree, frame.request), UiErrors::StyleReferenceInvalid);
            }
            SECTION("missing required intrinsic provider") {
                frame.surfaces[1].intrinsic = {UiLayoutIntrinsicKind::Text, true, {}};
                ErrorIs(owner.Update(tree, frame.request), UiErrors::LayoutIntrinsicUnavailable);
            }
            SECTION("stale document source") {
                frame.request.styleSources.document = Rev<UiDocumentRevision>(2);
                ErrorIs(owner.Update(tree, frame.request), UiErrors::StyleSourceStale);
            }
            SECTION("stale tree source") {
                frame.request.styleSources.tree = Rev<UiRuntimeTreeRevision>(2);
                frame.request.layout.sources.tree = Rev<UiRuntimeTreeRevision>(2);
                ErrorIs(owner.Update(tree, frame.request), UiErrors::StyleSourceStale);
            }
            SECTION("incomplete preorder") {
                frame.request.surfaces = std::span<const UiThemeSurfaceInput>{frame.surfaces}.first(1);
                ErrorIs(owner.Update(tree, frame.request), UiErrors::StyleSourceStale);
            }
            SECTION("stale element incarnation") {
                ++frame.surfaces[1].style.element.generation;
                ErrorIs(owner.Update(tree, frame.request), UiErrors::StyleSourceStale);
            }
            SECTION("exhausted class-reference budget") {
                const std::array references{frame.classes[0], frame.classes[0], frame.classes[0]};
                frame.surfaces[1].style.classes = references;
                ErrorIs(owner.Update(tree, frame.request), UiErrors::CapacityExceeded);
            }
            CHECK(Red(*owner.Current()) == 0.2F);
            CHECK(owner.Current()->style.Records()[1].element == originalChild);
        }

        TEST_CASE("Theme rollback after a cancelled unpublished candidate restores the active consumer values",
                  "[runtime_ui][theme][reload]") {
            auto tree = Tree();
            auto owner = Runtime();
            Frame frame{tree};
            REQUIRE(owner.Update(tree, frame.request).HasValue());
            const auto change = owner.BeginChange().Value();
            REQUIRE(owner.Prepare(change, Definition(0.9F, 512), Theme()).HasValue());
            frame.request.change = change;
            frame.request.layout.rootConstraints.maximum.width = -1;
            REQUIRE(owner.Update(tree, frame.request).HasError());
            REQUIRE(owner.Cancel(change).HasValue());
            frame.request.change.reset();
            frame.request.layout.rootConstraints.maximum.width = 6400;
            auto active = owner.Update(tree, frame.request);
            REQUIRE(active.HasValue());
            CHECK(Red(active.Value()) == 0.2F);
            CHECK(active.Value().layout.Get(frame.surfaces[1].style.element).Value().arrangement.contentBox.extent.width == 128);
        }

        TEST_CASE("Theme snapshots outlive moved and destroyed owners and all caller storage", "[runtime_ui][theme][lifecycle]") {
            std::optional<UiThemeSnapshot> retained;
            {
                auto tree = Tree();
                auto owner = Runtime();
                Frame frame{tree};
                auto result = owner.Update(tree, frame.request);
                REQUIRE(result.HasValue());
                retained = std::move(result).Value();
                auto moved = std::move(owner);
                CHECK(owner.State() == UiThemeRuntimeState::Stopped);
                ErrorIs(owner.BeginChange(), UiErrors::StyleLifecycleUnavailable);
                frame.surfaces[1].fill = {1.0F, 1.0F, 1.0F, 1.0F};
                REQUIRE(moved.BeginChange().HasValue());
                tree.Shutdown();
            }
            REQUIRE(retained.has_value());
            CHECK(Red(*retained) == 0.2F);
            CHECK(retained->style.Records().size() == 2);
            CHECK(retained->layout.Records().size() == 2);
            CHECK(retained->render.Commands().size() == 2);
        }

        TEST_CASE("Theme creation and preparation expose capacity and generation failures", "[runtime_ui][theme][failure]") {
            SECTION("mismatched canvas owner") {
                auto descriptor = Descriptor();
                ++descriptor.layout.canvas.generation;
                auto registry = RuntimeStyleRegistry::Create(Definition(), Rev<RuntimeStyleGeneration>());
                ErrorIs(UiThemeRuntime::Create(descriptor, std::move(registry).Value(), Theme()), UiErrors::StyleInvalid);
            }
            SECTION("generation exhausted") {
                const auto generation = Rev<RuntimeStyleGeneration>(std::numeric_limits<std::uint64_t>::max());
                auto descriptor = Descriptor();
                descriptor.style.initialRegistryGeneration = generation;
                auto registry = RuntimeStyleRegistry::Create(Definition(), generation);
                auto owner = UiThemeRuntime::Create(descriptor, std::move(registry).Value(), Theme());
                REQUIRE(owner.HasValue());
                auto runtime = std::move(owner).Value();
                ErrorIs(runtime.BeginChange(), UiErrors::GenerationExhausted);
            }
            SECTION("preparation allocation failed") {
                auto owner = Runtime();
                const auto change = owner.BeginChange().Value();
                auto definition = Definition();
                auto result = [&] {
                    Horo::Tests::AllocationProbe::ScopedFailure fail;
                    return owner.Prepare(change, std::move(definition), Theme());
                }();
                ErrorIs(result, UiErrors::CapacityExceeded);
                REQUIRE(owner.Prepare(change, Definition(), Theme()).HasValue());
            }
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
