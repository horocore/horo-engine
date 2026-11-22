#pragma once

/** @file EditorInspectionCommandEndpoint.h @brief Private workspace lease for numeric Scene inspection. */

#include "Horo/Editor/AgentSceneSelectionSnapshot.h"
#include "Horo/Foundation/Result.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Horo::Editor {
    /** @brief Identifies one authored Scene publication in the admitted workspace. */
    struct InspectionSceneRevision {
        std::uint64_t epoch{};
        std::uint64_t revision{};
        bool operator==(const InspectionSceneRevision &) const = default;
    };

    /** @brief Exact object set and optional continuation fence. */
    struct ObjectInspectionRequest {
        static constexpr std::size_t MaximumObjects = 16;
        std::vector<std::uint64_t> objects;
        std::size_t offset{};
        std::size_t limit{MaximumObjects};
        std::optional<InspectionSceneRevision> expected;
    };

    /** @brief One page from the caller's ordered set of exact scene object IDs. */
    struct TransformInspectionPage {
        InspectionSceneRevision scene;
        std::vector<AgentViewportTransform> records;
        std::optional<std::size_t> nextOffset;
    };

    /** @brief Axis-aligned bounds of an authored object's viewport geometry in world space. */
    struct InspectedBounds {
        std::uint64_t object{};
        std::array<float, 3> minimum{};
        std::array<float, 3> maximum{};
    };

    /** @brief One page from the ordered bounds request. */
    struct BoundsInspectionPage {
        InspectionSceneRevision scene;
        std::vector<InspectedBounds> records;
        std::optional<std::size_t> nextOffset;
    };

    /** @brief Numeric inventory of authored components. No component payload or asset identity crosses this port. */
    struct InspectedComponents {
        static constexpr std::array<std::string_view, 21> Names{"camera",
                                                                "animation",
                                                                "light",
                                                                "triggerVolume",
                                                                "audioSource",
                                                                "audioListener",
                                                                "uiCanvas",
                                                                "navigationSurface",
                                                                "navigationRegion",
                                                                "navigationModifier",
                                                                "navigationLink",
                                                                "navigationAgent",
                                                                "aiAgent",
                                                                "aiController",
                                                                "characterController",
                                                                "rigidBody",
                                                                "colliders",
                                                                "physicsConstraints",
                                                                "behaviors",
                                                                "gameplayComponents",
                                                                "navigationSources"};
        std::uint64_t object{};
        std::array<std::size_t, Names.size()> counts{};
    };

    /** @brief Exact count of authored geometry submitted per object by the current committed viewport extraction. */
    struct InspectedGeometry {
        std::uint64_t object{};
        std::size_t instances{};
        std::size_t vertices{};
        std::size_t triangles{};
    };

    /** @brief One page of authored component counts. */
    struct ComponentInspectionPage {
        InspectionSceneRevision scene;
        std::vector<InspectedComponents> records;
        std::optional<std::size_t> nextOffset;
    };

    /** @brief One page of published viewport geometry counts. */
    struct GeometryInspectionPage {
        InspectionSceneRevision scene;
        std::vector<InspectedGeometry> records;
        std::optional<std::size_t> nextOffset;
    };

    /** @brief Synchronous owner-lane query port shared by local and embedded adapters. */
    struct EditorInspectionCommandEndpoint {
        std::string project;
        std::uint64_t authorizationRevision{};
        std::thread::id owner;
        std::function<Result<TransformInspectionPage>(const ObjectInspectionRequest &)> readTransforms;
        std::function<Result<BoundsInspectionPage>(const ObjectInspectionRequest &)> readBounds;
        std::function<Result<ComponentInspectionPage>(const ObjectInspectionRequest &)> readComponents;
        std::function<Result<GeometryInspectionPage>(const ObjectInspectionRequest &)> readGeometry;
    };
}  // namespace Horo::Editor
