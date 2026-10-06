#include "Horo/Runtime/Ui/UiStyle.h"
#include "UiTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
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
            asset.tokens.push_back(
                {Token(1), UiStyleValueCategory::Color, UiStyleValueSource::Literal(UiStyleValue{Color(0.8F, 0.1F, 0.1F)}), false});
            asset.tokens.push_back({Token(2), UiStyleValueCategory::Color, UiStyleValueSource::Token({Asset(10), Token(1)}), false});
            asset.assignments.push_back(Assignment(text, UiStyleValue{Color(0.1F, 0.1F, 0.1F)}));
            asset.classes.push_back({Class(20), {}, false, {}, {}});
            asset.classes.back().assignments.push_back(Assignment(padding, UiStyleValue{UiStyleDimension{8}}));
            asset.classes.push_back({Class(21), {}, false, {}, {}});
            asset.classes.back().assignments.push_back(Assignment(text, UiStyleValue{Color(0.2F, 0.2F, 0.9F)}));
            asset.classes.back().states.push_back({UiVisualStateMask{UiVisualState::Invalid},
                                                   {},
                                                   UiStateLayer::Validation,
                                                   {Assignment(border, UiStyleValue{Color(0.9F, 0.0F, 0.0F)})}});
            asset.classes.back().states.push_back({UiVisualStateMask{UiVisualState::Disabled},
                                                   {},
                                                   UiStateLayer::Availability,
                                                   {Assignment(border, UiStyleValue{Color(0.0F, 0.8F, 0.0F)})}});
            return {.properties =
                        {{text, UiStyleValueCategory::Color, UiStyleValue{Color(0.0F, 0.0F, 0.0F)}, {.paint = true}, true, false},
                         {padding, UiStyleValueCategory::Dimension, UiStyleValue{UiStyleDimension{0}}, {.measure = true}, false, false},
                         {border, UiStyleValueCategory::Color, UiStyleValue{Color(1.0F, 1.0F, 1.0F)}, {.paint = true}, false, false},
                         {opacity, UiStyleValueCategory::Scalar, UiStyleValue{UiStyleScalar{1.0F}}, {.paint = true}, false, false}},
                    .assets = {std::move(asset)}};
        }

        UiStyleSourceRevisions Sources(const std::uint64_t interaction = 1) {
            return {Rev<UiDocumentRevision>(1),     Rev<UiRuntimeTreeRevision>(1), Rev<RuntimeStyleGeneration>(1),
                    Rev<UiStyleContentRevision>(1), Rev<UiStylePolicyRevision>(1), Rev<UiInteractionRevision>(interaction)};
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

        TEST_CASE("The style geometry counter used by publication rejects exhaustion without wrapping", "[runtime_ui][style][geometry]") {
            const auto maximum = Rev<UiStyleGeometryRevision>(std::numeric_limits<std::uint64_t>::max());
            const auto exhausted = maximum.Next();
            RequireError(exhausted, UiErrors::GenerationExhausted);
            REQUIRE(maximum.Value() == std::numeric_limits<std::uint64_t>::max());
        }

        TEST_CASE("Style geometry fences retain observation-only and paint-only publications", "[runtime_ui][style][geometry]") {
            auto registryResult = RuntimeStyleRegistry::Create(Definition(), Rev<RuntimeStyleGeneration>(1));
            REQUIRE(registryResult.HasValue());
            auto registry = std::move(registryResult).Value();
            auto tree = MakeTree();
            const auto root = tree.Root().Value().handle;
            const auto child = tree.Find(Stable<UiElementId>(2)).Value();
            std::array samples{UiStyleAnimationSample{Property(3), UiStyleValue{Color(0.3F, 0.4F, 0.5F)}}};
            std::array elements{UiStyleElementInput{root, Asset(10), {}, {}, {}, {}, {}, samples},
                                UiStyleElementInput{child, Asset(10), {}, {}, {}, {}, {}}};
            auto resolver = MakeResolver();
            const auto first = resolver.Update(tree, registry, {Sources(), elements});
            REQUIRE(first.HasValue());
            REQUIRE(first.Value().Descriptor().geometry == Rev<UiStyleGeometryRevision>(1));
            samples[0].value = Color(0.9F, 0.4F, 0.5F);
            const auto painted = resolver.Update(tree, registry, {Sources(2), elements});
            REQUIRE(painted.HasValue());
            REQUIRE(painted.Value().Descriptor().publication == Rev<UiStylePublicationRevision>(2));
            REQUIRE(painted.Value().Descriptor().sources.interaction == Rev<UiInteractionRevision>(2));
            REQUIRE(painted.Value().Descriptor().geometry == first.Value().Descriptor().geometry);
            samples[0] = {Property(2), UiStyleValue{UiStyleDimension{9}}};
            const auto measured = resolver.Update(tree, registry, {Sources(2), elements});
            REQUIRE(measured.HasValue());
            REQUIRE(measured.Value().Descriptor().geometry == Rev<UiStyleGeometryRevision>(2));
        }

        TEST_CASE("Style geometry effects include declared hit testing and accessibility", "[runtime_ui][style][geometry]") {
            auto definition = Definition();
            SECTION("hit testing") {
                definition.properties[2].effects.hitTest = true;
            }
            SECTION("accessibility eligibility") {
                definition.properties[2].effects.accessibility = true;
            }
            auto registryResult = RuntimeStyleRegistry::Create(std::move(definition), Rev<RuntimeStyleGeneration>(1));
            REQUIRE(registryResult.HasValue());
            auto registry = std::move(registryResult).Value();
            auto tree = MakeTree();
            const auto root = tree.Root().Value().handle;
            const auto child = tree.Find(Stable<UiElementId>(2)).Value();
            std::array samples{UiStyleAnimationSample{Property(3), UiStyleValue{Color(0.3F, 0.4F, 0.5F)}}};
            std::array elements{UiStyleElementInput{root, Asset(10), {}, {}, {}, {}, {}, samples},
                                UiStyleElementInput{child, Asset(10), {}, {}, {}, {}, {}}};
            auto resolver = MakeResolver();
            const auto first = resolver.Update(tree, registry, {Sources(), elements});
            REQUIRE(first.HasValue());
            samples[0].value = Color(0.9F, 0.4F, 0.5F);
            const auto changed = resolver.Update(tree, registry, {Sources(), elements});
            REQUIRE(changed.HasValue());
            REQUIRE(changed.Value().Descriptor().geometry == Rev<UiStyleGeometryRevision>(2));
        }

        TEST_CASE("Style geometry fences retain equal effective values across provenance changes", "[runtime_ui][style][geometry]") {
            auto registryResult = RuntimeStyleRegistry::Create(Definition(), Rev<RuntimeStyleGeneration>(1));
            REQUIRE(registryResult.HasValue());
            auto registry = std::move(registryResult).Value();
            auto tree = MakeTree();
            const auto root = tree.Root().Value().handle;
            const auto child = tree.Find(Stable<UiElementId>(2)).Value();
            std::array elements{UiStyleElementInput{root, Asset(10), {}, {}, {}, {}, {}},
                                UiStyleElementInput{child, Asset(10), {}, {}, {}, {}, {}}};
            auto resolver = MakeResolver();
            const auto first = resolver.Update(tree, registry, {Sources(), elements});
            REQUIRE(first.HasValue());
            const std::array overrides{Assignment(Property(2), UiStyleValue{UiStyleDimension{0}})};
            elements[0].inlineProperties = overrides;
            const auto observed = resolver.Update(tree, registry, {Sources(2), elements});
            REQUIRE(observed.HasValue());
            REQUIRE(observed.Value().Descriptor().publication != first.Value().Descriptor().publication);
            REQUIRE(observed.Value().Descriptor().geometry == first.Value().Descriptor().geometry);
            const auto *value = FindProperty(observed.Value(), root, Property(2));
            REQUIRE(value != nullptr);
            REQUIRE(value->provenance.origin == UiStyleOrigin::Inline);
        }

        TEST_CASE("Style geometry fences include inherited effective values and registry semantics", "[runtime_ui][style][geometry]") {
            auto definition = Definition();
            definition.properties[1].inheritsToChildren = true;
            definition.assets[0].states.push_back({UiVisualStateMask{UiVisualState::Pressed},
                                                   {},
                                                   UiStateLayer::Validation,
                                                   {Assignment(Property(2), UiStyleValue{UiStyleDimension{17}})}});
            auto registryResult = RuntimeStyleRegistry::Create(definition, Rev<RuntimeStyleGeneration>(1));
            REQUIRE(registryResult.HasValue());
            auto registry = std::move(registryResult).Value();
            auto tree = MakeTree();
            const auto root = tree.Root().Value().handle;
            const auto child = tree.Find(Stable<UiElementId>(2)).Value();
            std::array elements{UiStyleElementInput{root, Asset(10), {}, {}, {}, {}, {}},
                                UiStyleElementInput{child, Asset(10), {}, {}, {}, {}, {}}};
            auto resolver = MakeResolver();
            const auto first = resolver.Update(tree, registry, {Sources(), elements});
            REQUIRE(first.HasValue());
            elements[0].state = UiVisualStateMask{UiVisualState::Pressed};
            const auto inherited = resolver.Update(tree, registry, {Sources(), elements});
            REQUIRE(inherited.HasValue());
            const auto *value = FindProperty(inherited.Value(), child, Property(2));
            REQUIRE(value != nullptr);
            REQUIRE(std::get<UiStyleDimension>(value->value).value == 17);
            REQUIRE(value->provenance.origin == UiStyleOrigin::Inherited);
            REQUIRE(inherited.Value().Descriptor().geometry == Rev<UiStyleGeometryRevision>(2));
            auto replacementResult = RuntimeStyleRegistry::Create(std::move(definition), Rev<RuntimeStyleGeneration>(2));
            REQUIRE(replacementResult.HasValue());
            auto replacement = std::move(replacementResult).Value();
            auto sources = Sources();
            sources.registry = replacement.Generation();
            const auto changedRegistry = resolver.Update(tree, replacement, {sources, elements});
            REQUIRE(changedRegistry.HasValue());
            REQUIRE(changedRegistry.Value().Descriptor().geometry == Rev<UiStyleGeometryRevision>(3));
        }

        TEST_CASE("Runtime style samples animation after visual state while preserving policy authority",
                  "[runtime_ui][style][animation]") {
            auto registryResult = RuntimeStyleRegistry::Create(Definition(), Rev<RuntimeStyleGeneration>(1));
            REQUIRE(registryResult.HasValue());
            auto registry = std::move(registryResult).Value();
            auto tree = MakeTree();
            const auto root = tree.Root().Value().handle;
            const auto child = tree.Find(Stable<UiElementId>(2)).Value();
            const std::array classes{UiStyleClassReference{Asset(10), Class(21)}};
            std::array samples{UiStyleAnimationSample{Property(3), UiStyleValue{Color(0.3F, 0.4F, 0.5F)}}};
            std::array
                elements{UiStyleElementInput{root, Asset(10), {}, classes, {}, {}, UiVisualStateMask{UiVisualState::Disabled}, samples},
                         UiStyleElementInput{child, Asset(10), {}, {}, {}, {}, {}}};
            auto resolver = MakeResolver();
            auto first = resolver.Update(tree, registry, {Sources(), elements});
            REQUIRE(first.HasValue());
            const auto *animated = FindProperty(first.Value(), root, Property(3));
            REQUIRE(animated != nullptr);
            REQUIRE(std::get<UiStyleColor>(animated->value).red == 0.3F);
            REQUIRE(animated->provenance.origin == UiStyleOrigin::Animation);

            samples[0].value = Color(0.6F, 0.4F, 0.5F);
            auto second = resolver.Update(tree, registry, {Sources(), elements});
            REQUIRE(second.HasValue());
            REQUIRE(second.Value().Descriptor().publication == Rev<UiStylePublicationRevision>(2));
            const auto *updated = FindProperty(second.Value(), root, Property(3));
            REQUIRE(updated != nullptr);
            REQUIRE(std::get<UiStyleColor>(updated->value).red == 0.6F);
            REQUIRE(std::get<UiStyleColor>(animated->value).red == 0.3F);

            const std::array policy{Assignment(Property(3), UiStyleValue{Color(1.0F, 1.0F, 0.0F)})};
            elements[0].policyProperties = policy;
            auto policyResult = resolver.Update(tree, registry, {Sources(), elements});
            REQUIRE(policyResult.HasValue());
            const auto *resolvedPolicy = FindProperty(policyResult.Value(), root, Property(3));
            REQUIRE(resolvedPolicy != nullptr);
            REQUIRE(resolvedPolicy->provenance.origin == UiStyleOrigin::AccessibilityPolicy);
            REQUIRE(std::get<UiStyleColor>(resolvedPolicy->value).red == 1.0F);
        }

        TEST_CASE("Runtime style rejects malformed sampled animations without publishing", "[runtime_ui][style][animation]") {
            auto registryResult = RuntimeStyleRegistry::Create(Definition(), Rev<RuntimeStyleGeneration>(1));
            REQUIRE(registryResult.HasValue());
            auto registry = std::move(registryResult).Value();
            auto tree = MakeTree();
            const auto root = tree.Root().Value().handle;
            const auto child = tree.Find(Stable<UiElementId>(2)).Value();
            std::array samples{UiStyleAnimationSample{Property(4), UiStyleValue{UiStyleScalar{0.5F}}},
                               UiStyleAnimationSample{Property(4), UiStyleValue{UiStyleScalar{0.7F}}}};
            std::array elements{UiStyleElementInput{root, Asset(10), {}, {}, {}, {}, {}, samples},
                                UiStyleElementInput{child, Asset(10), {}, {}, {}, {}, {}}};
            auto resolver = MakeResolver();
            RequireError(resolver.Update(tree, registry, {Sources(), elements}), UiErrors::StyleInvalid);
            elements[0].animation = std::span<const UiStyleAnimationSample>{samples}.first(1);
            samples[0].value = UiStyleScalar{2.0F};
            RequireError(resolver.Update(tree, registry, {Sources(), elements}), UiErrors::StyleTypeMismatch);
            samples[0].value = UiStyleScalar{0.5F};
            samples[0].property = Property(99);
            RequireError(resolver.Update(tree, registry, {Sources(), elements}), UiErrors::StyleReferenceInvalid);
            samples[0].property = Property(4);
            auto valid = resolver.Update(tree, registry, {Sources(), elements});
            REQUIRE(valid.HasValue());
            REQUIRE(valid.Value().Descriptor().publication == Rev<UiStylePublicationRevision>(1));
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
            const std::array<UiStyleElementInput, 2> elements{UiStyleElementInput{root,
                                                                                  Asset(10),
                                                                                  {},
                                                                                  rootClasses,
                                                                                  {},
                                                                                  {},
                                                                                  UiVisualStateMask{UiVisualState::Invalid} |
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

        TEST_CASE("Prepared styles reserve scratch and abandon without exposing target values", "[runtime_ui][style][prepare]") {
            auto tree = MakeTree();
            const auto root = tree.Root().Value().handle;
            const auto child = tree.Find(Stable<UiElementId>(2)).Value();
            auto registryResult = RuntimeStyleRegistry::Create(Definition(), Rev<RuntimeStyleGeneration>(1));
            REQUIRE(registryResult.HasValue());
            auto registry = std::move(registryResult).Value();
            auto resolver = MakeResolver();
            const std::array classes{UiStyleClassReference{Asset(10), Class(21)}};
            std::array<UiStyleElementInput, 2> elements{
                {{root, Asset(10), {}, classes, {}, {}, {}}, {child, Asset(10), {}, {}, {}, {}, {}}}};
            auto first = resolver.Update(tree, registry, {Sources(), elements});
            REQUIRE(first.HasValue());
            auto old = std::move(first).Value();
            elements[0].state = UiVisualStateMask{UiVisualState::Disabled};
            auto prepared = resolver.Prepare(tree, registry, {Sources(2), elements});
            REQUIRE(prepared.HasValue());
            auto candidate = std::move(prepared).Value();
            REQUIRE(candidate.Candidate().Descriptor().publication == Rev<UiStylePublicationRevision>(2));
            const auto *targetBorder = FindProperty(candidate.Candidate(), root, Property(3));
            const auto *oldBorder = FindProperty(old, root, Property(3));
            REQUIRE(targetBorder != nullptr);
            REQUIRE(oldBorder != nullptr);
            REQUIRE(std::get<UiStyleColor>(targetBorder->value).green == 0.8F);
            REQUIRE(std::get<UiStyleColor>(oldBorder->value).green == 1.0F);
            RequireError(resolver.Update(tree, registry, {Sources(2), elements}), UiErrors::StyleCandidateBusy);
            RequireError(resolver.Invalidate({root, Sources().tree, UiStyleInvalidationKind::Paint}), UiErrors::StyleCandidateBusy);
            candidate.Abandon();
            candidate.Abandon();
            auto next = resolver.Update(tree, registry, {Sources(2), elements});
            REQUIRE(next.HasValue());
            REQUIRE(next.Value().Descriptor().publication == Rev<UiStylePublicationRevision>(2));
            REQUIRE(old.Descriptor().publication == Rev<UiStylePublicationRevision>(1));
        }

        TEST_CASE("Prepared styles reject foreign owners even when public revision fields collide", "[runtime_ui][style][prepare]") {
            auto tree = MakeTree();
            auto foreignTree = MakeTree();
            auto registryResult = RuntimeStyleRegistry::Create(Definition(), Rev<RuntimeStyleGeneration>(1));
            auto foreignResult = RuntimeStyleRegistry::Create(Definition(), Rev<RuntimeStyleGeneration>(1));
            REQUIRE(registryResult.HasValue());
            REQUIRE(foreignResult.HasValue());
            auto registry = std::move(registryResult).Value();
            auto foreignRegistry = std::move(foreignResult).Value();
            const std::array<UiStyleElementInput, 2> elements{{{tree.Root().Value().handle, Asset(10), {}, {}, {}, {}, {}},
                                                               {tree.Find(Stable<UiElementId>(2)).Value(), Asset(10), {}, {}, {}, {}, {}}}};
            auto resolver = MakeResolver();
            auto prepared = resolver.Prepare(tree, registry, {Sources(), elements});
            REQUIRE(prepared.HasValue());
            auto candidate = std::move(prepared).Value();
            RequireError(candidate.CanPublish(foreignTree, registry), UiErrors::StyleSourceStale);
            RequireError(candidate.CanPublish(tree, foreignRegistry), UiErrors::StyleSourceStale);
            REQUIRE(candidate.CanPublish(tree, registry).HasValue());
            auto moved = std::move(candidate);
            RequireError(candidate.CanPublish(tree, registry), UiErrors::StyleLifecycleUnavailable);
            REQUIRE(resolver.Commit(std::move(moved), tree, registry).HasValue());
        }

        TEST_CASE("Prepared style pins survive owner destruction but publication admission closes", "[runtime_ui][style][prepare]") {
            auto tree = MakeTree();
            auto registryResult = RuntimeStyleRegistry::Create(Definition(), Rev<RuntimeStyleGeneration>(1));
            REQUIRE(registryResult.HasValue());
            auto registry = std::move(registryResult).Value();
            const std::array<UiStyleElementInput, 2> elements{{{tree.Root().Value().handle, Asset(10), {}, {}, {}, {}, {}},
                                                               {tree.Find(Stable<UiElementId>(2)).Value(), Asset(10), {}, {}, {}, {}, {}}}};
            std::optional<UiStyleResolver::PreparedUpdate> candidate;
            {
                auto resolver = MakeResolver();
                auto prepared = resolver.Prepare(tree, registry, {Sources(), elements});
                REQUIRE(prepared.HasValue());
                candidate.emplace(std::move(prepared).Value());
                REQUIRE(resolver.BeginRetirement().HasValue());
                RequireError(candidate->CanPublish(tree, registry), UiErrors::StyleLifecycleUnavailable);
            }
            REQUIRE(candidate->Candidate().Records().size() == 2);
            RequireError(candidate->CanPublish(tree, registry), UiErrors::StyleLifecycleUnavailable);
            candidate->Abandon();
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
            const std::array<UiStyleElementInput, 2> elements{UiStyleElementInput{root, Asset(10), {}, {}, {}, {}, {}},
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
            replacementDefinition.assets[0].classes[1].assignments[0] = Assignment(Property(1), UiStyleValue{Color(0.7F, 0.7F, 0.2F)});
            auto replacementRegistryResult = RuntimeStyleRegistry::Create(std::move(replacementDefinition), Rev<RuntimeStyleGeneration>(2));
            REQUIRE(replacementRegistryResult.HasValue());
            auto replacementRegistry = std::move(replacementRegistryResult).Value();

            auto tree = MakeTree();
            const auto root = tree.Root().Value().handle;
            const auto child = tree.Find(Stable<UiElementId>(2)).Value();
            const std::array<UiStyleClassReference, 1> rootClasses{{{Asset(10), Class(21)}}};
            const std::array<UiStyleElementInput, 2> elements{UiStyleElementInput{root, Asset(10), {}, rootClasses, {}, {}, {}},
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
    }  // namespace
}  // namespace Horo::Runtime::Ui
