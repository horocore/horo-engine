#include "Horo/Runtime/Ui/UiStyle.h"
#include "UiTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <exception>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime::Ui;
    using Test::Stable;

    template <class T> T Revision() {
        return T::Create(1).Value();
    }

    template <class T> T Take(Result<T> result) {
        if (result.HasError())
            UNSCOPED_INFO(result.ErrorValue().domain.Value()
                          << '/' << result.ErrorValue().code.Value() << ": " << result.ErrorValue().message);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    UiOwnershipGeneration Owner() {
        return UiOwnershipGeneration::Create(61).Value();
    }

    UiStylePropertyId ColorProperty() {
        return Stable<UiStylePropertyId>(1);
    }

    UiStylePropertyId PaddingProperty() {
        return Stable<UiStylePropertyId>(2);
    }

    RuntimeStyleAssetId Asset() {
        return Stable<RuntimeStyleAssetId>(10);
    }

    UiStyleClassReference Class() {
        return {Asset(), Stable<UiStyleClassId>(20)};
    }

    UiStyleTokenReference Token() {
        return {Asset(), Stable<UiStyleTokenId>(1)};
    }

    UiStyleClassReference TokenClass() {
        return {Asset(), Stable<UiStyleClassId>(21)};
    }

    UiStyleRegistryDefinition Definition(const bool namespaced) {
        UiStyleRegistryDefinition
            definition{{{ColorProperty(), UiStyleValueCategory::Color, UiStyleColor{0, 0, 0, 1}, {.paint = true}, true},
                        {PaddingProperty(), UiStyleValueCategory::Dimension, UiStyleDimension{0}, {.measure = true}}},
                       {}};
        if (namespaced) {
            UiStyleAssetDefinition asset{.id = Asset()};
            asset.assignments.push_back({ColorProperty(), UiStyleValueSource::Literal(UiStyleColor{0.1F, 0, 0, 1}), false});
            asset.tokens.push_back(
                {Token().id, UiStyleValueCategory::Color, UiStyleValueSource::Literal(UiStyleColor{0.8F, 0, 0, 1}), false});
            asset.classes.push_back(
                {.id = Class().id, .assignments = {{PaddingProperty(), UiStyleValueSource::Literal(UiStyleDimension{8}), false}}});
            asset.classes.push_back({.id = TokenClass().id, .assignments = {{ColorProperty(), UiStyleValueSource::Token(Token()), false}}});
            definition.assets.push_back(std::move(asset));
        }
        return definition;
    }

    UiElementTree Tree() {
        auto allocator = Take(UiElementSlotAllocator::Create(Owner()));
        const UiElementTreeDescriptor descriptor{{Owner(), 1, 1},
                                                 {Owner(), 2, 1},
                                                 Stable<UiDocumentId>(1),
                                                 Revision<UiDocumentRevision>(),
                                                 Revision<UiRuntimeTreeRevision>(),
                                                 {2, 2, 2}};
        const std::array elements{UiElementDescriptor{Stable<UiElementId>(1), {}},
                                  UiElementDescriptor{Stable<UiElementId>(2), Stable<UiElementId>(1)}};
        return Take(UiElementTree::Create(allocator, descriptor, elements));
    }

    struct Fixture final {
        RuntimeStyleRegistry registry;
        UiElementTree tree{Tree()};
        UiStyleResolver resolver;

        explicit Fixture(const bool namespaced = false)
            : registry(Take(RuntimeStyleRegistry::Create(Definition(namespaced), Revision<RuntimeStyleGeneration>()))),
              resolver(Take(UiStyleResolver::Create({tree.Instance(), tree.Canvas(), tree.SourceDocument(), 2, 2, 2, 3,
                                                     Revision<RuntimeStyleGeneration>(), Revision<UiStylePublicationRevision>()}))) {}

        UiElementHandle Root() const {
            return tree.Root().Value().handle;
        }

        UiElementHandle Child() const {
            return tree.Find(Stable<UiElementId>(2)).Value();
        }

        UiStyleSourceRevisions Sources() const {
            return {Revision<UiDocumentRevision>(),     Revision<UiRuntimeTreeRevision>(), Revision<RuntimeStyleGeneration>(),
                    Revision<UiStyleContentRevision>(), Revision<UiStylePolicyRevision>(), Revision<UiInteractionRevision>()};
        }

        std::array<UiStyleElementInput, 2> Inputs() const {
            return {{{.element = Root()}, {.element = Child()}}};
        }
    };

    const UiComputedStyleProperty &Property(const UiComputedStyleSnapshot &snapshot, UiElementHandle element, UiStylePropertyId id) {
        const auto record = snapshot.Get(element);
        REQUIRE(record.HasValue());
        for (const auto &property : snapshot.Properties(record.Value()))
            if (property.property == id)
                return property;
        FAIL("Expected an actual computed property");
        std::terminate();
    }

    void RequireError(const auto &result, const ErrorCodeDescriptor &expected) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
        CHECK(result.ErrorValue().code.Value() == expected.code.Value());
    }
}  // namespace

TEST_CASE("Property-only style registries preserve registered defaults inheritance inline and policy provenance",
          "[runtime_ui][style][unassigned]") {
    Fixture fixture;
    CHECK(fixture.registry.Assets().empty());
    CHECK_FALSE(fixture.registry.HasAsset({}));
    auto inputs = fixture.Inputs();
    auto initial = Take(fixture.resolver.Update(fixture.tree, fixture.registry, {fixture.Sources(), inputs}));
    const auto &registered = Property(initial, fixture.Root(), ColorProperty());
    CHECK(registered.provenance.origin == UiStyleOrigin::RegisteredDefault);
    CHECK_FALSE(registered.provenance.asset.IsValid());
    const std::array inlineValues{
        UiStyleAssignment{ColorProperty(), UiStyleValueSource::Literal(UiStyleColor{0.3F, 0.7F, 0.2F, 1}), false}};
    const std::array policyValues{UiStyleAssignment{PaddingProperty(), UiStyleValueSource::Literal(UiStyleDimension{9}), false}};
    inputs[0].inlineProperties = inlineValues;
    inputs[0].policyProperties = policyValues;
    auto updated = Take(fixture.resolver.Update(fixture.tree, fixture.registry, {fixture.Sources(), inputs}));
    const auto &inlineColor = Property(updated, fixture.Root(), ColorProperty());
    CHECK(inlineColor.provenance.origin == UiStyleOrigin::Inline);
    CHECK_FALSE(inlineColor.provenance.asset.IsValid());
    CHECK(std::get<UiStyleColor>(inlineColor.value).green == 0.7F);
    const auto &inherited = Property(updated, fixture.Child(), ColorProperty());
    CHECK(inherited.provenance.origin == UiStyleOrigin::Inherited);
    CHECK(std::get<UiStyleColor>(inherited.value).green == 0.7F);
    const auto &policy = Property(updated, fixture.Root(), PaddingProperty());
    CHECK(policy.provenance.origin == UiStyleOrigin::AccessibilityPolicy);
    CHECK(std::get<UiStyleDimension>(policy.value).value == 9);
    CHECK(std::get<UiStyleDimension>(Property(updated, fixture.Child(), PaddingProperty()).value).value == 0);
}

TEST_CASE("Absent base style assets retain independently qualified admitted classes and tokens", "[runtime_ui][style][unassigned]") {
    Fixture fixture{true};
    CHECK_FALSE(fixture.registry.HasAsset({}));
    REQUIRE(fixture.registry.HasAsset(Asset()));
    auto inputs = fixture.Inputs();
    inputs[0].typeClass = Class();
    auto classOnly = Take(fixture.resolver.Update(fixture.tree, fixture.registry, {fixture.Sources(), inputs}));
    const auto &padding = Property(classOnly, fixture.Root(), PaddingProperty());
    CHECK(padding.provenance.origin == UiStyleOrigin::ElementTypeClass);
    CHECK(padding.provenance.asset == Asset());
    CHECK(std::get<UiStyleDimension>(padding.value).value == 8);
    CHECK(Property(classOnly, fixture.Root(), ColorProperty()).provenance.origin == UiStyleOrigin::RegisteredDefault);
    const std::array classes{TokenClass()};
    inputs[0].classes = classes;
    auto token = Take(fixture.resolver.Update(fixture.tree, fixture.registry, {fixture.Sources(), inputs}));
    const auto &color = Property(token, fixture.Root(), ColorProperty());
    CHECK(color.provenance.origin == UiStyleOrigin::AuthoredClass);
    CHECK(color.provenance.asset == Asset());
    CHECK(color.provenance.token == Token().id);
    CHECK(std::get<UiStyleColor>(color.value).red == 0.8F);
}

TEST_CASE("Unassigned style policy still rejects every explicitly unknown namespace class and token", "[runtime_ui][style][unassigned]") {
    Fixture fixture;
    auto inputs = fixture.Inputs();
    const std::array classes{Class()};
    const std::array tokens{UiStyleAssignment{ColorProperty(), UiStyleValueSource::Token(Token()), false}};
    const ErrorCodeDescriptor *expected = &UiErrors::StyleReferenceInvalid;
    SECTION("assigned namespace") {
        inputs[0].asset = Asset();
        expected = &UiErrors::StyleInvalid;
    }
    SECTION("qualified type class") {
        inputs[0].typeClass = Class();
    }
    SECTION("authored class") {
        inputs[0].classes = classes;
    }
    SECTION("token") {
        inputs[0].inlineProperties = tokens;
        expected = &UiErrors::StyleInvalid;  // Inline and policy inputs are literals, not authored namespace token declarations.
    }
    RequireError(fixture.resolver.Update(fixture.tree, fixture.registry, {fixture.Sources(), inputs}), *expected);
}

TEST_CASE("Unassigned base assets do not admit unknown tokens through qualified class definitions", "[runtime_ui][style][unassigned]") {
    auto definition = Definition(true);
    definition.assets[0].classes.back().assignments[0].value = UiStyleValueSource::Token({Asset(), Stable<UiStyleTokenId>(99)});
    RequireError(RuntimeStyleRegistry::Create(std::move(definition), Revision<RuntimeStyleGeneration>()), UiErrors::StyleReferenceInvalid);
}

TEST_CASE("Property-only registries retain complete registered schema validation", "[runtime_ui][style][unassigned]") {
    auto definition = Definition(false);
    SECTION("empty properties") {
        definition.properties.clear();
    }
    SECTION("invalid identity") {
        definition.properties[0].id = {};
    }
    SECTION("invalid category default") {
        definition.properties[0].defaultValue = UiStyleDimension{2};
    }
    RequireError(RuntimeStyleRegistry::Create(std::move(definition), Revision<RuntimeStyleGeneration>()), UiErrors::StyleInvalid);
}
