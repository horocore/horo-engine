#include "Horo/Runtime/Ui/UiStyle.h"

#include "UiTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        using Test::Stable;

        UiOwnershipGeneration Owner() {
            return UiOwnershipGeneration::Create(61).Value();
        }

        template <typename Revision> Revision Rev(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        UiElementTree MakeTree() {
            const UiElementTreeDescriptor descriptor{.instance = {Owner(), 1, 1},
                                                      .canvas = {Owner(), 2, 1},
                                                      .document = Stable<UiDocumentId>(1),
                                                      .documentRevision = Rev<UiDocumentRevision>(1),
                                                      .treeRevision = Rev<UiRuntimeTreeRevision>(1),
                                                      .limits = {2, 4, 4}};
            const std::array elements{UiElementDescriptor{Stable<UiElementId>(1), {}},
                                      UiElementDescriptor{Stable<UiElementId>(2), Stable<UiElementId>(1)}};
            auto allocator = UiElementSlotAllocator::Create(Owner());
            REQUIRE(allocator.HasValue());
            auto slotAllocator = std::move(allocator).Value();
            auto result = UiElementTree::Create(slotAllocator, descriptor, elements);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        UiStylePropertyId Property(const std::uint8_t value) {
            return Stable<UiStylePropertyId>(value);
        }

        RuntimeStyleAssetId Asset(const std::uint8_t value) {
            return Stable<RuntimeStyleAssetId>(value);
        }

        UiStyleClassId Class(const std::uint8_t value) {
            return Stable<UiStyleClassId>(value);
        }

        UiStyleTokenId Token(const std::uint8_t value) {
            return Stable<UiStyleTokenId>(value);
        }

        UiStyleColor Color(const float red, const float green, const float blue) {
            return {red, green, blue, 1.0F, UiStyleColorRole::Generic};
        }

        UiStyleAssignment Assignment(const UiStylePropertyId property, UiStyleValue value) {
            return {property, UiStyleValueSource::Literal(std::move(value)), false};
        }

        UiStyleRegistryDefinition Definition() {
            const auto text = Property(1);
            const auto padding = Property(2);
            const auto border = Property(3);
            const auto opacity = Property(4);
            UiStyleAssetDefinition asset{.id = Asset(10)};
            asset.tokens.push_back({Token(1), UiStyleValueCategory::Color,
                                    UiStyleValueSource::Literal(UiStyleValue{Color(0.8F, 0.1F, 0.1F)}), false});
            asset.tokens.push_back({Token(2), UiStyleValueCategory::Color,
                                    UiStyleValueSource::Token({Asset(10), Token(1)}), false});
            asset.assignments.push_back(Assignment(text, UiStyleValue{Color(0.1F, 0.1F, 0.1F)}));
            asset.classes.push_back({Class(20), {}, false, {}, {}});
            asset.classes.back().assignments.push_back(Assignment(padding, UiStyleValue{UiStyleDimension{8}}));
            asset.classes.push_back({Class(21), {}, false, {}, {}});
            asset.classes.back().assignments.push_back(Assignment(text, UiStyleValue{Color(0.2F, 0.2F, 0.9F)}));
            asset.classes.back().states.push_back({UiVisualStateMask{UiVisualState::Invalid}, {}, UiStateLayer::Validation,
                                                   {Assignment(border, UiStyleValue{Color(0.9F, 0.0F, 0.0F)})}});
            asset.classes.back().states.push_back({UiVisualStateMask{UiVisualState::Disabled}, {}, UiStateLayer::Availability,
                                                   {Assignment(border, UiStyleValue{Color(0.0F, 0.8F, 0.0F)})}});
            return {.properties = {{text, UiStyleValueCategory::Color, UiStyleValue{Color(0.0F, 0.0F, 0.0F)},
                                     {.paint = true}, true, false},
                                    {padding, UiStyleValueCategory::Dimension, UiStyleValue{UiStyleDimension{0}},
                                     {.measure = true}, false, false},
                                    {border, UiStyleValueCategory::Color, UiStyleValue{Color(1.0F, 1.0F, 1.0F)},
                                     {.paint = true}, false, false},
                                    {opacity, UiStyleValueCategory::Scalar, UiStyleValue{UiStyleScalar{1.0F}},
                                     {.paint = true}, false, false}},
                    .assets = {std::move(asset)}};
        }

        UiStyleSourceRevisions Sources(const std::uint64_t interaction = 1) {
            return {Rev<UiDocumentRevision>(1),
                    Rev<UiRuntimeTreeRevision>(1),
                    Rev<RuntimeStyleGeneration>(1),
                    Rev<UiStyleContentRevision>(1),
                    Rev<UiStylePolicyRevision>(1),
                    Rev<UiInteractionRevision>(interaction)};
        }

        UiStyleResolver MakeResolver() {
            const auto descriptor = UiStyleResolverDescriptor{.instance = {Owner(), 1, 1},
                                                               .canvas = {Owner(), 2, 1},
                                                               .document = Stable<UiDocumentId>(1),
                                                               .elementCapacity = 2,
                                                               .propertyCapacity = 4,
                                                               .invalidationCapacity = 4,
                                                               .concurrentSnapshots = 3,
                                                               .initialRegistryGeneration = Rev<RuntimeStyleGeneration>(1),
                                                               .initialPublication = Rev<UiStylePublicationRevision>(1)};
            auto result = UiStyleResolver::Create(descriptor);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        const UiComputedStyleProperty *FindProperty(const UiComputedStyleSnapshot &snapshot, const UiElementHandle element,
                                                    const UiStylePropertyId property) {
            const auto record = snapshot.Get(element);
            REQUIRE(record.HasValue());
            for (const auto &value : snapshot.Properties(record.Value()))
                if (value.property == property)
                    return &value;
            return nullptr;
        }

        void RequireError(const auto &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }

        TEST_CASE("Runtime style registry resolves exact token categories and precedence", "[runtime_ui][style]") {
            auto registryResult = RuntimeStyleRegistry::Create(Definition(), Rev<RuntimeStyleGeneration>(1));
            REQUIRE(registryResult.HasValue());
            auto registry = std::move(registryResult).Value();
            REQUIRE(registry.ResolveToken({Asset(10), Token(2)}).HasValue());
            REQUIRE(std::get<UiStyleColor>(registry.ResolveToken({Asset(10), Token(2)}).Value()).red == 0.8F);

            auto tree = MakeTree();
            const auto root = tree.Root().Value().handle;
            const auto child = tree.Find(Stable<UiElementId>(2)).Value();
            const std::array<UiStyleClassReference, 1> rootClasses{{{Asset(10), Class(21)}}};
            const std::array<UiStyleElementInput, 2> elements{
                UiStyleElementInput{root, Asset(10), {}, rootClasses, {}, {}, UiVisualStateMask{UiVisualState::Invalid} |
                                                                                UiVisualState::Disabled},
                UiStyleElementInput{child, Asset(10), {}, {}, {}, {}, {}}};
            auto resolver = MakeResolver();
            const auto request = UiStyleUpdateRequest{Sources(), elements};
            auto snapshotResult = resolver.Update(tree, registry, request);
            REQUIRE(snapshotResult.HasValue());
            auto snapshot = std::move(snapshotResult).Value();

            const auto text = FindProperty(snapshot, root, Property(1));
            REQUIRE(text != nullptr);
            REQUIRE(std::get<UiStyleColor>(text->value).blue == 0.9F);
            const auto inherited = FindProperty(snapshot, child, Property(1));
            REQUIRE(inherited != nullptr);
            REQUIRE(inherited->provenance.origin == UiStyleOrigin::Inherited);
            REQUIRE(std::get<UiStyleColor>(inherited->value).blue == 0.9F);

            const auto border = FindProperty(snapshot, root, Property(3));
            REQUIRE(border != nullptr);
            REQUIRE(std::get<UiStyleColor>(border->value).green == 0.8F);
            REQUIRE(snapshot.Descriptor().publication == Rev<UiStylePublicationRevision>(1));
        }

        TEST_CASE("Runtime style registry rejects token cycles and category mismatch", "[runtime_ui][style][failure]") {
            auto definition = Definition();
            definition.assets[0].tokens[1].value = UiStyleValueSource::Token({Asset(10), Token(2)});
            RequireError(RuntimeStyleRegistry::Create(std::move(definition), Rev<RuntimeStyleGeneration>(1)), UiErrors::StyleCycle);

            definition = Definition();
            definition.assets[0].tokens[1].category = UiStyleValueCategory::Dimension;
            RequireError(RuntimeStyleRegistry::Create(std::move(definition), Rev<RuntimeStyleGeneration>(1)), UiErrors::StyleTypeMismatch);
        }

        TEST_CASE("Runtime style invalidation is bounded and shutdown preserves leased snapshots", "[runtime_ui][style][lifecycle]") {
            auto registryResult = RuntimeStyleRegistry::Create(Definition(), Rev<RuntimeStyleGeneration>(1));
            REQUIRE(registryResult.HasValue());
            auto registry = std::move(registryResult).Value();
            auto tree = MakeTree();
            const auto root = tree.Root().Value().handle;
            const auto child = tree.Find(Stable<UiElementId>(2)).Value();
            const std::array<UiStyleElementInput, 2> elements{
                UiStyleElementInput{root, Asset(10), {}, {}, {}, {}, {}},
                UiStyleElementInput{child, Asset(10), {}, {}, {}, {}, {}}};
            auto resolver = MakeResolver();
            auto first = resolver.Update(tree, registry, {Sources(), elements});
            REQUIRE(first.HasValue());
            auto retained = std::move(first).Value();
            REQUIRE(resolver.Invalidate({child, Rev<UiRuntimeTreeRevision>(1), UiStyleInvalidationKind::Paint}).HasValue());
            const auto changed = resolver.Update(tree, registry, {Sources(2), elements});
            REQUIRE(changed.HasValue());
            auto next = std::move(changed).Value();
            REQUIRE(next.Descriptor().publication == Rev<UiStylePublicationRevision>(2));

            auto stale = Sources(3);
            stale.document = Rev<UiDocumentRevision>(2);
            RequireError(resolver.Update(tree, registry, {stale, elements}), UiErrors::StyleSourceStale);
            REQUIRE(retained.Records().size() == 2);
            REQUIRE(resolver.BeginRetirement().HasValue());
            RequireError(resolver.Update(tree, registry, {Sources(4), elements}), UiErrors::StyleLifecycleUnavailable);
            resolver.Shutdown();
            registry.Shutdown();
            REQUIRE(retained.Records().size() == 2);
            REQUIRE(next.Records().size() == 2);
        }

        TEST_CASE("Runtime style reload publishes a new generation without mutating the retained snapshot", "[runtime_ui][style][reload]") {
            auto firstRegistryResult = RuntimeStyleRegistry::Create(Definition(), Rev<RuntimeStyleGeneration>(1));
            REQUIRE(firstRegistryResult.HasValue());
            auto firstRegistry = std::move(firstRegistryResult).Value();
            auto replacementDefinition = Definition();
            replacementDefinition.assets[0].classes[1].assignments[0] =
                Assignment(Property(1), UiStyleValue{Color(0.7F, 0.7F, 0.2F)});
            auto replacementRegistryResult =
                RuntimeStyleRegistry::Create(std::move(replacementDefinition), Rev<RuntimeStyleGeneration>(2));
            REQUIRE(replacementRegistryResult.HasValue());
            auto replacementRegistry = std::move(replacementRegistryResult).Value();

            auto tree = MakeTree();
            const auto root = tree.Root().Value().handle;
            const auto child = tree.Find(Stable<UiElementId>(2)).Value();
            const std::array<UiStyleClassReference, 1> rootClasses{{{Asset(10), Class(21)}}};
            const std::array<UiStyleElementInput, 2> elements{
                UiStyleElementInput{root, Asset(10), {}, rootClasses, {}, {}, {}},
                UiStyleElementInput{child, Asset(10), {}, {}, {}, {}, {}}};
            auto resolver = MakeResolver();
            auto first = resolver.Update(tree, firstRegistry, {Sources(), elements});
            REQUIRE(first.HasValue());
            auto retained = std::move(first).Value();

            auto reloadSources = Sources();
            reloadSources.registry = Rev<RuntimeStyleGeneration>(2);
            reloadSources.content = Rev<UiStyleContentRevision>(2);
            auto reloaded = resolver.Update(tree, replacementRegistry, {reloadSources, elements});
            REQUIRE(reloaded.HasValue());
            const auto replacement = std::move(reloaded).Value();

            const auto oldText = FindProperty(retained, root, Property(1));
            const auto newText = FindProperty(replacement, root, Property(1));
            REQUIRE(oldText != nullptr);
            REQUIRE(newText != nullptr);
            REQUIRE(std::get<UiStyleColor>(oldText->value).blue == 0.9F);
            REQUIRE(std::get<UiStyleColor>(newText->value).red == 0.7F);
            REQUIRE(replacement.Descriptor().sources.registry == Rev<RuntimeStyleGeneration>(2));
            REQUIRE(replacement.Descriptor().publication == Rev<UiStylePublicationRevision>(2));
        }
    }
}  // namespace Horo::Runtime::Ui
