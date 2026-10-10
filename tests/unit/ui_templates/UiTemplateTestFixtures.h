#pragma once

#include "Horo/UiTemplates/UiTemplateModel.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::UiTemplates::TestSupport {
    /** @brief Fixed persistent fixture identity; independent of runtime ownership and declaration order. */
    template <typename Id> Id Identity(const std::uint8_t marker) {
        Runtime::Ui::SerializedUiId bytes{};
        bytes.back() = marker;
        return Id::Create(bytes).Value();
    }

    /** @brief Builds the owner fixture's ordinary authority through production validation, preserving each test's inputs. */
    inline Runtime::Ui::UiDocument OwnerOrdinaryDocument(const std::span<const Runtime::Ui::UiDocumentElement> elements,
                                                         const std::span<const Runtime::Ui::UiAssetDependency> dependencies = {}) {
        using namespace Runtime::Ui;
        UiDocumentBuilder builder{Identity<UiDocumentId>(1), UiDocumentRevision::Create(1).Value()};
        REQUIRE(builder.AddCanvas({Identity<UiCanvasId>(1), Identity<UiElementId>(99)}).HasValue());
        for (const auto &element : elements)
            REQUIRE(builder.AddElement(element).HasValue());
        for (const auto &dependency : dependencies)
            REQUIRE(builder.RequireAsset(dependency).HasValue());
        auto ordinary = std::move(builder).Build();
        REQUIRE(ordinary.HasValue());
        return std::move(ordinary).Value();
    }
}  // namespace Horo::UiTemplates::TestSupport
