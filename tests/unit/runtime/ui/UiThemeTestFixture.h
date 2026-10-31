#pragma once

#include "Horo/Runtime/Ui/UiElementCatalog.h"
#include "Horo/Runtime/Ui/UiTheme.h"
#include "UiTestUtils.h"
#include "support/AllocationProbe.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <optional>
#include <utility>

namespace Horo::Runtime::Ui::ThemeTest {
    using Test::Stable;

    template <typename T> T Rev(const std::uint64_t value = 1) {
        return T::Create(value).Value();
    }

    inline UiOwnershipGeneration Owner() {
        return UiOwnershipGeneration::Create(729).Value();
    }

    inline RuntimeStyleAssetId Theme(const std::uint8_t value = 10) {
        return Stable<RuntimeStyleAssetId>(value);
    }

    inline UiStylePropertyId Property(const std::uint8_t value) {
        return Stable<UiStylePropertyId>(value);
    }

    inline UiStyleTokenReference Token() {
        return {Theme(), Stable<UiStyleTokenId>(1)};
    }

    inline UiStyleColor Color(const float red) {
        return {red, 0.2F, 0.3F, 1.0F};
    }

    inline UiStyleAssignment Literal(const std::uint8_t property, UiStyleValue value) {
        return {Property(property), UiStyleValueSource::Literal(std::move(value)), false};
    }

    inline void ErrorIs(const auto &result, const ErrorCodeDescriptor &error) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == error.code.Value());
    }

    inline UiStyleRegistryDefinition Definition(const float red = 0.2F, const std::int32_t width = 128) {
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

    inline UiThemeRuntimeDescriptor Descriptor(const std::uint32_t renderSlots = 5) {
        const RuntimeUiInstanceId instance{Owner(), 1, 1};
        const UiCanvasInstanceId canvas{Owner(), 2, 1};
        const auto document = Stable<UiDocumentId>(1);
        return {{instance, canvas, document, 2, 5, 4, 5, Rev<RuntimeStyleGeneration>(), Rev<UiStylePublicationRevision>()},
                {instance, canvas, document, 2, 4, 5, Rev<UiInteractionRevision>()},
                {{Owner(), 9, 1}, {2, 1, 1, 1, 1, 1, 1}, renderSlots},
                {Property(1), Property(2), Property(3), Property(4), Property(5)},
                2};
    }

    inline UiElementTree Tree() {
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

    inline UiThemeRuntime Runtime(const std::uint32_t renderSlots = 5) {
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
            request.layout = {{tree.SourceDocumentRevision(), tree.Revision(), Rev<UiLayoutContentRevision>(), Rev<UiLayoutStyleRevision>(),
                               Rev<UiLayoutIntrinsicRevision>(), Rev<UiLayoutCanvasRevision>(), Rev<UiLayoutPolicyRevision>()},
                              {{0, 0}, {6400, 6400}},
                              {{0, 0}, {6400, 6400}},
                              nullptr,
                              {}};
            request.surfaces = surfaces;
        }
    };

    inline float Red(const UiThemeSnapshot &snapshot) {
        return std::get<UiSolidDraw>(snapshot.render.Commands()[1].payload).color.red;
    }

}  // namespace Horo::Runtime::Ui::ThemeTest
