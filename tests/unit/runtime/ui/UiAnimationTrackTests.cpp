#include "UiAnimationLayoutProjection.h"
#include "UiAnimationTrackSampling.h"
#include "UiTestUtils.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::AnimationInternal;
    using Test::Stable;

    constexpr auto End = std::numeric_limits<std::uint32_t>::max();

    UiElementTree Tree() {
        const auto owner = UiOwnershipGeneration::Create(83).Value();
        const UiElementTreeDescriptor descriptor{.instance = {owner, 1, 1},
                                                 .canvas = {owner, 2, 1},
                                                 .document = Stable<UiDocumentId>(1),
                                                 .documentRevision = UiDocumentRevision::Create(1).Value(),
                                                 .treeRevision = UiRuntimeTreeRevision::Create(1).Value(),
                                                 .limits = {1, 1, 1}};
        const std::array elements{UiElementDescriptor{Stable<UiElementId>(1), {}}};
        auto issued = UiElementSlotAllocator::Create(owner);
        REQUIRE(issued.HasValue());
        auto allocator = std::move(issued).Value();
        auto result = UiElementTree::Create(allocator, descriptor, elements);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    RuntimeStyleRegistry Registry(const bool dimensions = false) {
        UiStyleRegistryDefinition definition;
        definition.properties.push_back({.id = Stable<UiStylePropertyId>(1),
                                         .category = dimensions ? UiStyleValueCategory::Dimension : UiStyleValueCategory::Scalar,
                                         .defaultValue = dimensions ? UiStyleValue{UiStyleDimension{0}} : UiStyleValue{UiStyleScalar{0}},
                                         .effects = {.measure = dimensions, .paint = true},
                                         .maximumScalar = 200});
        definition.assets.push_back({.id = Stable<RuntimeStyleAssetId>(1)});
        auto result = RuntimeStyleRegistry::Create(std::move(definition), RuntimeStyleGeneration::Create(1).Value());
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    UiAnimationPropertyTrack Track() {
        return {Stable<UiElementId>(1),
                Stable<UiStylePropertyId>(1),
                {{0, UiStyleScalar{0}}, {100, UiStyleScalar{100}}, {End, UiStyleScalar{200}}}};
    }

    UiComputedStyleSnapshot ComputedDimension(const UiElementTree &tree, const RuntimeStyleRegistry &registry, const std::int32_t value,
                                              const std::span<const UiStyleAssignment> policy = {}) {
        const auto owner = UiOwnershipGeneration::Create(83).Value();
        auto created = UiStyleResolver::Create({{owner, 1, 1},
                                                {owner, 2, 1},
                                                Stable<UiDocumentId>(1),
                                                1,
                                                1,
                                                1,
                                                3,
                                                RuntimeStyleGeneration::Create(1).Value(),
                                                UiStylePublicationRevision::Create(1).Value()});
        REQUIRE(created.HasValue());
        auto resolver = std::move(created).Value();
        const auto root = tree.Root();
        REQUIRE(root.HasValue());
        const std::array animation{UiStyleAnimationSample{Stable<UiStylePropertyId>(1), UiStyleDimension{value}}};
        const std::array elements{
            UiStyleElementInput{root.Value().handle, Stable<RuntimeStyleAssetId>(1), {}, {}, {}, policy, {}, animation}};
        const UiStyleSourceRevisions sources{UiDocumentRevision::Create(1).Value(),     UiRuntimeTreeRevision::Create(1).Value(),
                                             RuntimeStyleGeneration::Create(1).Value(), UiStyleContentRevision::Create(1).Value(),
                                             UiStylePolicyRevision::Create(1).Value(),  UiInteractionRevision::Create(1).Value()};
        auto computed = resolver.Update(tree, registry, {sources, elements});
        REQUIRE(computed.HasValue());
        return std::move(computed).Value();
    }
}  // namespace

TEST_CASE("Typed animation admission binds the actual retained element and immutable property schema", "[runtime_ui][animation][tracks]") {
    auto tree = Tree();
    auto registry = Registry();
    auto track = Track();
    const auto admitted = ValidatePropertyTrack(track, tree, registry, 3);
    REQUIRE(admitted.HasValue());
    const auto actual = tree.Find(track.target);
    REQUIRE(actual.HasValue());
    CHECK(admitted.Value() == actual.Value());
    SECTION("missing authored target") {
        track.target = Stable<UiElementId>(2);
    }
    SECTION("unknown property") {
        track.property = Stable<UiStylePropertyId>(2);
    }
    SECTION("missing first endpoint") {
        track.keyframes.front().position = 1;
    }
    SECTION("missing last endpoint") {
        track.keyframes.back().position = End - 1;
    }
    SECTION("duplicate positions") {
        track.keyframes[1].position = 0;
    }
    SECTION("category mismatch") {
        track.keyframes[1].value = UiStyleDimension{100};
    }
    SECTION("nonfinite literal") {
        track.keyframes[1].value = UiStyleScalar{std::numeric_limits<float>::infinity()};
    }
    SECTION("unsupported easing") {
        track.keyframes[1].easing = static_cast<UiAnimationEasing>(255);
    }
    CHECK(ValidatePropertyTrack(track, tree, registry, 3).HasError());
}

TEST_CASE("Admitted animation tracks preserve exact keyframes and select the correct incoming segment", "[runtime_ui][animation][tracks]") {
    const auto track = Track();
    for (const auto &[position, expected] : std::array<std::pair<std::uint32_t, float>, 4>{{{0, 0}, {50, 50}, {100, 100}, {End, 200}}}) {
        const auto sampled = SamplePropertyTrack(track, position);
        REQUIRE(sampled.HasValue());
        const auto *scalar = std::get_if<UiStyleScalar>(&sampled.Value());
        REQUIRE(scalar != nullptr);
        CHECK(scalar->value == Catch::Approx(expected));
    }
    const auto beforeMiddle = SamplePropertyTrack(track, 99);
    REQUIRE(beforeMiddle.HasValue());
    CHECK(std::get<UiStyleScalar>(beforeMiddle.Value()).value == Catch::Approx(99));
    auto tree = Tree();
    auto registry = Registry();
    CHECK(ValidatePropertyTrack(track, tree, registry, 2).HasError());
    REQUIRE(tree.BeginRetirement().HasValue());
    CHECK(ValidatePropertyTrack(track, tree, registry, 3).HasError());
}

TEST_CASE("Actual computed animation dimensions project into declarative layout after accessibility precedence",
          "[runtime_ui][animation][layout]") {
    auto tree = Tree();
    auto registry = Registry(true);
    const auto root = tree.Root();
    REQUIRE(root.HasValue());
    std::array descriptors{UiLayoutElementDescriptor{root.Value().handle, {}}};
    const std::array bindings{LayoutBinding{root.Value().handle, Stable<UiStylePropertyId>(1), UiAnimationLayoutField::Width}};
    const auto animated = ComputedDimension(tree, registry, 128);
    REQUIRE(ProjectLayout(animated, bindings, descriptors).HasValue());
    CHECK(descriptors.front().style.width == UiLength::Dip(128));
    auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors);
    REQUIRE(evaluator.HasValue());
    const auto measured = evaluator.Value().Measure({root.Value().handle, {{0, 0}, {1024, 1024}}, {}, false, {}});
    REQUIRE(measured.HasValue());
    CHECK(measured.Value().desired.width == 128);
    const std::array policy{UiStyleAssignment{Stable<UiStylePropertyId>(1), UiStyleValueSource::Literal(UiStyleDimension{64}), false}};
    const auto accessible = ComputedDimension(tree, registry, 256, policy);
    REQUIRE(ProjectLayout(accessible, bindings, descriptors).HasValue());
    CHECK(descriptors.front().style.width == UiLength::Dip(64));
    CHECK(descriptors.front().style.width != UiLength::Dip(256));
}
