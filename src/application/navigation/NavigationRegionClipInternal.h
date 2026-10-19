#pragma once

#include "NavigationGeometryCanonicalization.h"
#include "NavigationRegionGeometryInternal.h"

namespace Horo::Application::NavigationBakeDetail::RegionClip {
    /** @brief Fixed convex polygon; one triangle intersected by a box has at most nine vertices. */
    struct Polygon final {
        std::array<Math::Vec3, 16> vertices;
        std::size_t count{};
    };

    /** @brief One disjoint retained source triangle in canonical world metres. */
    using Triangle = std::array<Math::Vec3, 3>;

    /** @brief Fixed scratch buffer, independently capped per original triangle without heap growth. */
    struct Fragments final {
        std::array<Triangle, NavigationSceneGeometryPolicy::MaximumRegionFragments> values;
        std::size_t count{};
    };

    /** @brief Shared work/cancellation owner; every clipping edge and emitted fragment consumes finite work. */
    struct Work final {
        std::uint64_t &remaining;
        CancellationToken cancellation;
        [[nodiscard]] Result<void> Spend(std::uint64_t units);
    };

    /** @brief Appends non-degenerate geometry within the selected fragment ceiling. */
    [[nodiscard]] Result<void> Append(Fragments &output, const Triangle &triangle, std::uint32_t maximumFragments, Work &work);
    /** @brief Partitions exactly into disjoint inside/outside pieces; null inside discards exclusions. */
    [[nodiscard]] Result<void> Split(const Triangle &triangle, const SourceRegion &region, Fragments *inside, Fragments &outside,
                                     std::uint32_t maximumFragments, Work &work);
}  // namespace Horo::Application::NavigationBakeDetail::RegionClip
