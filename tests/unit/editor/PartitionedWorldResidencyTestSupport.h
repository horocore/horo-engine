#pragma once
#include "Horo/Editor/PartitionedWorldWorkspace.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace Horo::Editor::WorldResidencyTests {
    namespace WS = WorldStreaming;

    /** @brief Creates deterministic distinct identities for isolated test owners/pages. */
    inline Assets::AssetId Asset(const std::uint8_t value = 1) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return Assets::AssetId::FromBytes(bytes);
    }

    /** @brief Creates real page-scoped source metadata over two individually loadable pages. */
    inline PartitionedWorldDocument Document() {
        const auto world = WS::WorldPartitionId::Create(Asset().Bytes()).Value();
        const auto grid = WS::WorldCellQuantizationPolicy::Create({}, 1000, {-1, 1, -1, 1, -1, 1}, 1).Value();
        PartitionedWorldDocument document{.settings = {.partition = world, .bounds = {}, .grid = grid}};
        document.layers.push_back({.id = WS::StreamingLayerId::Create(0).Value(), .name = "Terrain"});
        for (const auto id : {Asset(2), Asset(3)}) {
            PartitionedWorldPage page{
                .head = {.partition = world, .sourceAsset = id, .revision = WS::WorldAuthoringRevision::Create(1).Value()}};
            page.objects.push_back(
                {.descriptor = {.address = {.page = id, .object = 1}, .revision = page.head.revision, .sourceAsset = Asset(4)},
                 .layer = document.layers.front().id});
            document.pages.push_back(page);
        }
        return document;
    }

    /** @brief Creates canonical Scene bytes containing actual editable content. */
    inline std::string Source(const std::string_view name = "Box") {
        return std::string{R"({"schemaVersion":1,"objects":[{"id":1,"parent":null,"name":")"} + std::string{name} +
               R"(","transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},"primitiveMesh":null,"components":{},"editor":{"visible":false,"locked":true}}]})";
    }

    /** @brief Advances all metadata revision evidence for a coherent content successor. */
    inline PartitionedWorldPage Next(PartitionedWorldPage page) {
        page.head.revision = WS::NextWorldAuthoringRevision(page.head.revision).Value();
        for (auto &object : page.objects)
            object.descriptor.revision = page.head.revision;
        return page;
    }

    /** @brief Asserts the stable typed error without treating failure as a successful publication. */
    template <typename T> void ErrorIs(const Result<T> &result, const std::string_view code) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == code);
    }

    /** @brief Reads a current root without hiding query failure. */
    inline PartitionedWorldSnapshotLease Current(PartitionedWorldWorkspace &workspace) {
        auto result = workspace.Snapshot();
        REQUIRE(result.HasValue());
        return result.Value();
    }
}  // namespace Horo::Editor::WorldResidencyTests
