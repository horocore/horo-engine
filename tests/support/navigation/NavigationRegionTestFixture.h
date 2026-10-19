#pragma once

#include "navigation/NavigationProjectBakeInputFixture.h"

#include <catch2/catch_approx.hpp>
#include <cmath>

namespace Horo::Application::RegionTestSupport {
    /** @brief Creates one exact authored box region without inventing geometry or semantic source defaults. */
    inline SceneSource::SceneObjectSnapshot Region(std::uint64_t id, Runtime::NavigationRegionMode mode, Math::Vec3 center,
                                                   Math::Vec3 halfExtents) {
        SceneSource::SceneObjectSnapshot object{.id = {100 + id}, .name = "Region"};
        object.components.navigationRegion =
            Runtime::NavigationRegionComponent{.id = Navigation::TestSupport::Id<Navigation::NavigationRegionId>(id),
                                               .surface = Navigation::TestSupport::Id<Navigation::SurfaceId>(10),
                                               .localBounds = {center, halfExtents},
                                               .mode = mode};
        return object;
    }

    /** @brief Writes genuine multi-object Scene documents using the single production codec. */
    inline void Scenes(const NavigationInputTestSupport::Project &project, std::span<const SceneSource::SceneObjectSnapshot> objects) {
        const auto text = SceneSource::EncodeSceneSource({objects, {}});
        project.source.Write("Assets/first.horo", text);
        project.source.Write("Assets/second.horo", text);
    }

    /** @brief Captures one exact Scene through the actual graph, Physics and region selection path. */
    inline auto First(const NavigationInputTestSupport::Project &project, const NavigationSceneGeometryPolicy &policy = {}) {
        const auto source = project.Capture();
        return CaptureNavigationSceneColliderGeometry(source->Scenes().front(), source->Definitions().front(), policy);
    }

    /** @brief Measures only the upward horizontal floor at Y=0; duplicated overlap would increase this value. */
    inline double TopArea(const NavigationPrimitiveGeometry &geometry) {
        double area{};
        for (const auto &triangle : geometry.triangles) {
            const auto a = geometry.vertices[triangle.vertexIndices[0]];
            const auto b = geometry.vertices[triangle.vertexIndices[1]];
            const auto c = geometry.vertices[triangle.vertexIndices[2]];
            if (std::abs(a.y) > 1.0e-5F || std::abs(b.y) > 1.0e-5F || std::abs(c.y) > 1.0e-5F)
                continue;
            const double cross = (static_cast<double>(b.z) - a.z) * (static_cast<double>(c.x) - a.x) -
                                 (static_cast<double>(b.x) - a.x) * (static_cast<double>(c.z) - a.z);
            if (cross > 0)
                area += cross / 2;
        }
        return area;
    }
}  // namespace Horo::Application::RegionTestSupport
