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

    /** @brief Explicit world-space ray segment. The owner normalizes direction before sampling. */
    struct InspectionSurfaceRay {
        std::array<double, 3> origin{};
        std::array<double, 3> direction{};
        double maximumDistance{};
    };

    /** @brief Exact object set and optional continuation fence. */
    struct ObjectInspectionRequest {
        static constexpr std::size_t MaximumObjects = 16;
        std::vector<std::uint64_t> objects;
        std::size_t offset{};
        std::size_t limit{MaximumObjects};
        std::optional<InspectionSceneRevision> expected;
        std::optional<std::uint64_t> referenceObject; /**< Used only by the distance tool. */
        std::optional<double> gridStep;               /**< Used only by the grid-offset tool. */
        std::optional<InspectionSurfaceRay> surfaceRay;
    };

    /** @brief Authored TRS, local and world matrices, and the authored object's world pivot. */
    struct InspectedTransform {
        AgentViewportTransform local;
        std::array<float, 16> localMatrixRowMajor{};
        std::array<float, 16> worldMatrixRowMajor{};
        std::array<float, 3> worldPivot{};
    };

    /** @brief One page from the caller's ordered set of exact scene object IDs. */
    struct TransformInspectionPage {
        InspectionSceneRevision scene;
        std::vector<InspectedTransform> records;
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

    /** @brief World-space separation and orientation between a visible reference and target. */
    struct InspectedDistance {
        std::uint64_t object{};
        std::uint64_t referenceObject{};
        double gap{};
        std::array<double, 3> overlapDepth{};
        std::array<double, 3> nearestReference{};
        std::array<double, 3> nearestTarget{};
        double forwardAngleRadians{};
    };

    /** @brief Requested targets measured against the same exact reference. */
    struct DistanceInspectionPage {
        InspectionSceneRevision scene;
        std::vector<InspectedDistance> records;
        std::optional<std::size_t> nextOffset;
    };

    /** @brief Committed world position and numeric translation needed to reach the specified grid. */
    struct InspectedGridOffset {
        std::uint64_t object{};
        double gridStep{};
        std::array<double, 3> position{};
        std::array<double, 3> snappedPosition{};
        std::array<double, 3> offset{};
    };

    /** @brief Requested grid offsets in target order. */
    struct GridInspectionPage {
        InspectionSceneRevision scene;
        std::vector<InspectedGridOffset> records;
        std::optional<std::size_t> nextOffset;
    };

    /** @brief Nearest surface sample, with a geometric normal from the published triangle winding. */
    struct InspectionSurfaceSample {
        double distance{};
        std::array<double, 3> position{};
        std::array<double, 3> normal{};
        double slopeRadians{};
    };

    /** @brief A visible target with an explicit optional ray hit. */
    struct InspectedSurface {
        std::uint64_t object{};
        std::optional<InspectionSurfaceSample> sample;
    };

    /** @brief Surface samples in target order. */
    struct SurfaceInspectionPage {
        InspectionSceneRevision scene;
        std::vector<InspectedSurface> records;
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
        std::function<Result<DistanceInspectionPage>(const ObjectInspectionRequest &)> readDistances;
        std::function<Result<GridInspectionPage>(const ObjectInspectionRequest &)> readGridOffsets;
        std::function<Result<SurfaceInspectionPage>(const ObjectInspectionRequest &)> readSurfaces;
    };
}  // namespace Horo::Editor
