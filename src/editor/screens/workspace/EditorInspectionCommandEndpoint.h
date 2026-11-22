#pragma once

/** @file EditorInspectionCommandEndpoint.h @brief Private workspace lease for numeric Scene inspection. */

#include "Horo/Editor/AgentSceneSelectionSnapshot.h"
#include "Horo/Foundation/Result.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
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

    /** @brief Synchronous owner-lane query port shared by local and embedded adapters. */
    struct EditorInspectionCommandEndpoint {
        std::string project;
        std::uint64_t authorizationRevision{};
        std::thread::id owner;
        std::function<Result<TransformInspectionPage>(const ObjectInspectionRequest &)> readTransforms;
        std::function<Result<BoundsInspectionPage>(const ObjectInspectionRequest &)> readBounds;
    };
}  // namespace Horo::Editor
