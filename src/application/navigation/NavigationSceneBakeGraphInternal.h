#pragma once

#include "Horo/Application/NavigationSceneBakeGeometry.h"
#include "NavigationRegionGeometryInternal.h"

#include <map>

namespace Horo::Application::NavigationBakeDetail {
    /** @brief Private graph node; pointers are borrowed only during the synchronous capture. */
    struct Node final {
        const SceneSource::SceneObjectSnapshot *object{};
        std::optional<std::size_t> parent;
        Math::Mat4 local;
        Math::Mat4 world;
        std::size_t depth{};
        std::uint8_t state{};
    };

    /** @brief Complete borrowed graph, with resolved world transforms and unique authored surface/object indices. */
    struct Graph final {
        std::vector<Node> nodes;
        std::map<std::uint64_t, std::size_t> ids;
        std::map<Navigation::SurfaceId, std::size_t> surfaces;
        std::map<Navigation::SurfaceId, SourceRegions> regions;
    };

    /** @brief Qualifies definition coordinates against the Physics/Scene owner's canonical right-handed Y-up metres.
     * @param definition Authored source policy. @return Success or typed incompatible source geometry failure.
     */
    [[nodiscard]] Result<void> ValidateSceneCoordinatePolicy(const Navigation::NavigationDefinition &definition);

    /** @brief Validates and resolves one complete borrowed Scene graph under the shared capture bounds. */
    [[nodiscard]] Result<Graph> CaptureSceneGraph(const NavigationSceneBakeSource &scene, const NavigationDefinitionBakeSource &definition,
                                                  const NavigationSceneGeometryPolicy &policy, CancellationToken cancellation);
    /** @brief Captures through one already validated graph; all borrows end at return. */
    [[nodiscard]] Result<std::vector<NavigationCapturedCollider>> CaptureSceneColliders(const NavigationSceneBakeSource &scene,
                                                                                        const NavigationDefinitionBakeSource &definition,
                                                                                        const Graph &graph,
                                                                                        const NavigationSceneGeometryPolicy &policy,
                                                                                        CancellationToken cancellation);
    /** @brief Shares one finite region-work budget across all project Scene captures. */
    [[nodiscard]] Result<std::vector<NavigationCapturedCollider>> CaptureSceneColliders(
        const NavigationSceneBakeSource &scene, const NavigationDefinitionBakeSource &definition, const Graph &graph,
        const NavigationSceneGeometryPolicy &policy, std::uint64_t &remainingRegionWork, CancellationToken cancellation);
    /** @brief Resolves exact enabled region frames once per validated Scene, before any source selection. */
    [[nodiscard]] Result<void> CaptureSceneRegions(Graph &graph, const NavigationDefinitionBakeSource &definition,
                                                   const NavigationSceneGeometryPolicy &policy, CancellationToken cancellation);
}  // namespace Horo::Application::NavigationBakeDetail
