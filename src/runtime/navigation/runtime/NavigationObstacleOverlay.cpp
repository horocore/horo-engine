#include "Horo/Navigation/NavigationObstacleOverlay.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <variant>

namespace Horo::Navigation {
    namespace {
        using Region = NavigationObstacleOverlayChangedRegion;
        using Record = NavigationObstacleRecord;

        [[nodiscard]] Region Bounds(const Record &record) noexcept {
            Region region{.obstacle = record.id};
            std::visit([&]<typename Shape>(const Shape &shape) {
                const double halfX = [&] {
                    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(shape)>, NavigationDynamicBoxShape>)
                        return static_cast<double>(shape.halfExtents.x);
                    else
                        return static_cast<double>(shape.radius);
                }();
                const double halfZ = [&] {
                    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(shape)>, NavigationDynamicBoxShape>)
                        return static_cast<double>(shape.halfExtents.z);
                    else
                        return static_cast<double>(shape.radius);
                }();
                region.minimumX = static_cast<double>(shape.center.x) - halfX;
                region.minimumZ = static_cast<double>(shape.center.z) - halfZ;
                region.maximumX = static_cast<double>(shape.center.x) + halfX;
                region.maximumZ = static_cast<double>(shape.center.z) + halfZ;
            }, record.shape);
            return region;
        }

        [[nodiscard]] Region ChangedBounds(const Record &previous, const Record *current) noexcept {
            Region region = Bounds(previous);
            if (current && current->enabled) {
                const Region newer = Bounds(*current);
                region.minimumX = std::min(region.minimumX, newer.minimumX);
                region.minimumZ = std::min(region.minimumZ, newer.minimumZ);
                region.maximumX = std::max(region.maximumX, newer.maximumX);
                region.maximumZ = std::max(region.maximumZ, newer.maximumZ);
            }
            return region;
        }

        [[nodiscard]] std::pair<double, double> VerticalBounds(const Record &record) noexcept {
            return std::visit([]<typename Shape>(const Shape &shape) {
                const double halfHeight = [&] {
                    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(shape)>, NavigationDynamicBoxShape>)
                        return static_cast<double>(shape.halfExtents.y);
                    else
                        return static_cast<double>(shape.halfHeight);
                }();
                return std::pair{static_cast<double>(shape.center.y) - halfHeight, static_cast<double>(shape.center.y) + halfHeight};
            }, record.shape);
        }

        [[nodiscard]] bool OnSurface(const Record &record, const NavigationObstacleOverlaySurface &surface) noexcept {
            if (const Region footprint = Bounds(record);
                footprint.maximumX < surface.bounds.minimum.x || footprint.minimumX > surface.bounds.maximum.x ||
                footprint.maximumZ < surface.bounds.minimum.z || footprint.minimumZ > surface.bounds.maximum.z)
                return false;
            const auto [minimumY, maximumY] = VerticalBounds(record);
            return maximumY >= surface.bounds.minimum.y && minimumY <= surface.bounds.maximum.y;
        }

        [[nodiscard]] bool SweptHeightIntersectsSurface(const Record *previous, const Record *current,
                                                        const NavigationObstacleOverlaySurface &surface) noexcept {
            double minimum = std::numeric_limits<double>::infinity();
            double maximum = -std::numeric_limits<double>::infinity();
            const auto include = [&](const Record *record) {
                if (!record || !record->enabled)
                    return;
                const auto [from, to] = VerticalBounds(*record);
                minimum = std::min(minimum, from);
                maximum = std::max(maximum, to);
            };
            include(previous);
            include(current);
            return maximum >= surface.bounds.minimum.y && minimum <= surface.bounds.maximum.y;
        }

        [[nodiscard]] bool ValidSurface(const NavigationDynamicRegistrySnapshot &snapshot,
                                        const NavigationObstacleOverlaySurface &surface) noexcept {
            return snapshot.IsValid() && surface.id.IsValid() && surface.world == snapshot.Binding().world && surface.topology.IsValid() &&
                   surface.bounds.IsValid();
        }

        [[nodiscard]] Region OnSurfaceBounds(Region region, const NavigationObstacleOverlaySurface &surface) noexcept {
            region.surface = surface.id;
            region.minimumX = std::max(region.minimumX, static_cast<double>(surface.bounds.minimum.x));
            region.minimumZ = std::max(region.minimumZ, static_cast<double>(surface.bounds.minimum.z));
            region.maximumX = std::min(region.maximumX, static_cast<double>(surface.bounds.maximum.x));
            region.maximumZ = std::min(region.maximumZ, static_cast<double>(surface.bounds.maximum.z));
            return region;
        }

        [[nodiscard]] bool WithinSurface(const Math::Vec3 point, const NavigationObstacleOverlaySurface &surface) noexcept {
            return point.x >= surface.bounds.minimum.x && point.x <= surface.bounds.maximum.x && point.z >= surface.bounds.minimum.z &&
                   point.z <= surface.bounds.maximum.z;
        }

        [[nodiscard]] std::optional<Region> SurfaceChange(const Region &region, const Record *previous, const Record *current,
                                                          const NavigationObstacleOverlaySurface &surface) noexcept {
            if (!SweptHeightIntersectsSurface(previous, current, surface))
                return std::nullopt;
            const auto clipped = OnSurfaceBounds(region, surface);
            if (clipped.minimumX > clipped.maximumX || clipped.minimumZ > clipped.maximumZ)
                return std::nullopt;
            return clipped;
        }

        [[nodiscard]] bool IntersectsBox(const NavigationDynamicBoxShape &box, const Math::Vec3 from, const Math::Vec3 to,
                                         const double radius) noexcept {
            const std::array<double, 3> start{from.x, from.y, from.z};
            const std::array<double, 3> end{to.x, to.y, to.z};
            const std::array<double, 3> center{box.center.x, box.center.y, box.center.z};
            const std::array<double, 3> half{box.halfExtents.x, box.halfExtents.y, box.halfExtents.z};
            double near = 0.0;
            double far = 1.0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const double minimum = center[axis] - half[axis] - radius;
                const double maximum = center[axis] + half[axis] + radius;
                const double delta = end[axis] - start[axis];
                if (delta == 0.0) {
                    if (start[axis] < minimum || start[axis] > maximum)
                        return false;
                    continue;
                }
                const double first = (minimum - start[axis]) / delta;
                const double second = (maximum - start[axis]) / delta;
                near = std::max(near, std::min(first, second));
                far = std::min(far, std::max(first, second));
                if (near > far)
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool IntersectsCylinder(const NavigationDynamicCylinderShape &cylinder, const Math::Vec3 from, const Math::Vec3 to,
                                              const double radius) noexcept {
            const double minimumY = static_cast<double>(cylinder.center.y) - cylinder.halfHeight - radius;
            if (const double maximumY = static_cast<double>(cylinder.center.y) + cylinder.halfHeight + radius;
                std::max(static_cast<double>(from.y), static_cast<double>(to.y)) < minimumY ||
                std::min(static_cast<double>(from.y), static_cast<double>(to.y)) > maximumY)
                return false;
            const double dx = static_cast<double>(to.x) - from.x;
            const double dz = static_cast<double>(to.z) - from.z;
            const double lengthSquared = dx * dx + dz * dz;
            const double fraction = lengthSquared > 0.0 ? std::clamp(((static_cast<double>(cylinder.center.x) - from.x) * dx +
                                                                      (static_cast<double>(cylinder.center.z) - from.z) * dz) /
                                                                         lengthSquared,
                                                                     0.0, 1.0)
                                                        : 0.0;
            const double separationX = static_cast<double>(from.x) + fraction * dx - cylinder.center.x;
            const double separationZ = static_cast<double>(from.z) + fraction * dz - cylinder.center.z;
            const double clearance = static_cast<double>(cylinder.radius) + radius;
            return separationX * separationX + separationZ * separationZ <= clearance * clearance;
        }

        [[nodiscard]] bool Intersects(const Record &record, const Math::Vec3 from, const Math::Vec3 to, const double radius) noexcept {
            return std::visit([&]<typename Shape>(const Shape &shape) {
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(shape)>, NavigationDynamicBoxShape>)
                    return IntersectsBox(shape, from, to, radius);
                else
                    return IntersectsCylinder(shape, from, to, radius);
            }, record.shape);
        }

        [[nodiscard]] std::pair<const Record *, const Record *> NextChangedPair(const std::span<const Record *> previous,
                                                                                const std::span<const Record *> current,
                                                                                std::size_t &oldIndex, std::size_t &newIndex) noexcept {
            const Record *older = oldIndex < previous.size() ? previous[oldIndex] : nullptr;
            const Record *newer = newIndex < current.size() ? current[newIndex] : nullptr;
            if (older && (!newer || older->id < newer->id)) {
                ++oldIndex;
                return {older, nullptr};
            }
            if (newer && (!older || newer->id < older->id)) {
                ++newIndex;
                return {nullptr, newer};
            }
            ++oldIndex;
            ++newIndex;
            return {older, newer};
        }

        template <typename Emit>
        void ForEachChanged(const std::span<const Record *> previous, const std::span<const Record *> current, Emit &&emit) {
            std::size_t oldIndex{};
            std::size_t newIndex{};
            while (oldIndex < previous.size() || newIndex < current.size()) {
                const auto [older, newer] = NextChangedPair(previous, current, oldIndex, newIndex);
                if ((!older || !older->enabled) && (!newer || !newer->enabled))
                    continue;
                if (older && newer && older->handle == newer->handle && older->revision == newer->revision)
                    continue;
                if (older && older->enabled)
                    emit(ChangedBounds(*older, newer), older, newer);
                else if (newer && newer->enabled)
                    emit(Bounds(*newer), older, newer);
            }
        }

        [[nodiscard]] std::size_t OrderedRecords(const std::span<const Record> records,
                                                 std::array<const Record *, NavigationDynamicRegistryHardLimits::Obstacles> &ordered) {
            for (std::size_t index = 0; index < records.size(); ++index)
                ordered[index] = &records[index];
            std::ranges::sort(std::span{ordered}.first(records.size()), {}, [](const Record *record) {
                return record->id;
            });
            return records.size();
        }
    }  // namespace

    /** @copydoc ProbeNavigationObstacleOverlay */
    Result<NavigationObstacleOverlayProbe> ProbeNavigationObstacleOverlay(const NavigationDynamicRegistrySnapshot &snapshot,
                                                                          const Math::Vec3 from, const Math::Vec3 to,
                                                                          const float radiusMeters, const NavigationDynamicLayerMask layers,
                                                                          const std::size_t maximumChecks) {
        if (!snapshot.IsValid() || !Math::IsFinite(from) || !Math::IsFinite(to) || !std::isfinite(radiusMeters) || radiusMeters < 0.0F ||
            layers.Empty() || maximumChecks == 0 || maximumChecks > NavigationDynamicRegistryHardLimits::Obstacles)
            return Result<NavigationObstacleOverlayProbe>::Failure(MakeError(NavigationErrors::DynamicRegistryInvalid));
        if (snapshot.Obstacles().size() > maximumChecks)
            return Result<NavigationObstacleOverlayProbe>::Failure(MakeError(NavigationErrors::DynamicRegistryCapacityExceeded));
        NavigationObstacleOverlayProbe result{.binding = snapshot.Binding(), .revision = snapshot.Revision()};
        for (const auto &obstacle : snapshot.Obstacles()) {
            if (obstacle.enabled && obstacle.layers.Intersects(layers) && Intersects(obstacle, from, to, radiusMeters)) {
                result.blockingObstacle = obstacle.id;
                result.blocked = true;
                break;
            }
        }
        return Result<NavigationObstacleOverlayProbe>::Success(result);
    }

    /** @copydoc CollectNavigationObstacleOverlayChanges */
    Result<std::size_t> CollectNavigationObstacleOverlayChanges(const NavigationDynamicRegistrySnapshot &previous,
                                                                const NavigationDynamicRegistrySnapshot &current,
                                                                const std::span<NavigationObstacleOverlayChangedRegion> output) {
        if (!current.IsValid())
            return Result<std::size_t>::Failure(MakeError(NavigationErrors::DynamicRegistryInvalid));
        if (previous.IsValid() && (previous.Binding() != current.Binding() || previous.Revision() > current.Revision()))
            return Result<std::size_t>::Failure(MakeError(NavigationErrors::DynamicRegistryStale));
        std::array<const Record *, NavigationDynamicRegistryHardLimits::Obstacles> older{};
        std::array<const Record *, NavigationDynamicRegistryHardLimits::Obstacles> newer{};
        const auto oldCount = previous.IsValid() ? OrderedRecords(previous.Obstacles(), older) : 0;
        const auto newCount = OrderedRecords(current.Obstacles(), newer);
        const auto oldSpan = std::span{older}.first(oldCount);
        const auto newSpan = std::span{newer}.first(newCount);
        std::size_t required{};
        ForEachChanged(oldSpan, newSpan, [&required](const Region &, const Record *, const Record *) {
            ++required;
        });
        if (required > output.size())
            return Result<std::size_t>::Failure(MakeError(NavigationErrors::DynamicRegistryCapacityExceeded));
        std::size_t written{};
        ForEachChanged(oldSpan, newSpan, [&output, &written](const Region &region, const Record *, const Record *) {
            output[written++] = region;
        });
        return Result<std::size_t>::Success(written);
    }

    /** @copydoc ProjectNavigationObstacleOverlaySurface */
    Result<NavigationObstacleOverlaySurfaceProjection> ProjectNavigationObstacleOverlaySurface(
        const NavigationDynamicRegistrySnapshot &snapshot, const NavigationObstacleOverlaySurface &surface,
        const std::span<NavigationObstacleOverlaySurfaceRecord> output) {
        if (!ValidSurface(snapshot, surface))
            return Result<NavigationObstacleOverlaySurfaceProjection>::Failure(MakeError(NavigationErrors::DynamicRegistryInvalid));
        std::size_t required{};
        for (const auto &record : snapshot.Obstacles()) {
            if (record.enabled && OnSurface(record, surface))
                ++required;
        }
        if (required > output.size())
            return Result<NavigationObstacleOverlaySurfaceProjection>::Failure(
                MakeError(NavigationErrors::DynamicRegistryCapacityExceeded));
        std::size_t written{};
        for (const auto &record : snapshot.Obstacles()) {
            if (!record.enabled || !OnSurface(record, surface))
                continue;
            output[written++] = {.surface = surface.id,
                                 .obstacle = record.id,
                                 .shape = record.shape,
                                 .layers = record.layers,
                                 .bounds = OnSurfaceBounds(Bounds(record), surface)};
        }
        return Result<NavigationObstacleOverlaySurfaceProjection>::Success({.binding = snapshot.Binding(),
                                                                            .revision = snapshot.Revision(),
                                                                            .surface = surface.id,
                                                                            .topology = surface.topology,
                                                                            .count = written});
    }

    /** @copydoc ProbeNavigationObstacleOverlaySurface */
    Result<NavigationObstacleOverlayProbe> ProbeNavigationObstacleOverlaySurface(const NavigationDynamicRegistrySnapshot &snapshot,
                                                                                 const NavigationObstacleOverlaySurfaceQuery &query) {
        if (!ValidSurface(snapshot, query.surface) || !Math::IsFinite(query.from) || !Math::IsFinite(query.to) ||
            !std::isfinite(query.radiusMeters) || query.radiusMeters < 0.0F || query.layers.Empty() || query.maximumChecks == 0 ||
            query.maximumChecks > NavigationDynamicRegistryHardLimits::Obstacles || !WithinSurface(query.from, query.surface) ||
            !WithinSurface(query.to, query.surface))
            return Result<NavigationObstacleOverlayProbe>::Failure(MakeError(NavigationErrors::DynamicRegistryInvalid));
        std::size_t checked{};
        NavigationObstacleOverlayProbe result{.binding = snapshot.Binding(),
                                              .revision = snapshot.Revision(),
                                              .surface = query.surface.id,
                                              .topology = query.surface.topology};
        for (const auto &obstacle : snapshot.Obstacles()) {
            if (!obstacle.enabled || !OnSurface(obstacle, query.surface))
                continue;
            if (++checked > query.maximumChecks)
                return Result<NavigationObstacleOverlayProbe>::Failure(MakeError(NavigationErrors::DynamicRegistryCapacityExceeded));
            if (obstacle.layers.Intersects(query.layers) && Intersects(obstacle, query.from, query.to, query.radiusMeters)) {
                result.blockingObstacle = obstacle.id;
                result.blocked = true;
                break;
            }
        }
        return Result<NavigationObstacleOverlayProbe>::Success(result);
    }

    /** @copydoc CollectNavigationObstacleOverlaySurfaceChanges */
    Result<NavigationObstacleOverlaySurfaceProjection> CollectNavigationObstacleOverlaySurfaceChanges(
        const NavigationDynamicRegistrySnapshot &previous, const NavigationDynamicRegistrySnapshot &current,
        const NavigationObstacleOverlaySurface &surface, const std::span<Region> output) {
        if (!ValidSurface(current, surface))
            return Result<NavigationObstacleOverlaySurfaceProjection>::Failure(MakeError(NavigationErrors::DynamicRegistryInvalid));
        if (previous.IsValid() && (previous.Binding() != current.Binding() || previous.Revision() > current.Revision()))
            return Result<NavigationObstacleOverlaySurfaceProjection>::Failure(MakeError(NavigationErrors::DynamicRegistryStale));
        std::array<const Record *, NavigationDynamicRegistryHardLimits::Obstacles> older{};
        std::array<const Record *, NavigationDynamicRegistryHardLimits::Obstacles> newer{};
        const auto oldCount = previous.IsValid() ? OrderedRecords(previous.Obstacles(), older) : 0;
        const auto newCount = OrderedRecords(current.Obstacles(), newer);
        const auto oldSpan = std::span{older}.first(oldCount);
        const auto newSpan = std::span{newer}.first(newCount);
        std::size_t required{};
        ForEachChanged(oldSpan, newSpan, [&required, &surface](const Region &region, const Record *oldRecord, const Record *newRecord) {
            if (SurfaceChange(region, oldRecord, newRecord, surface))
                ++required;
        });
        if (required > output.size())
            return Result<NavigationObstacleOverlaySurfaceProjection>::Failure(
                MakeError(NavigationErrors::DynamicRegistryCapacityExceeded));
        std::size_t written{};
        ForEachChanged(oldSpan, newSpan,
                       [&output, &written, &surface](const Region &region, const Record *oldRecord, const Record *newRecord) {
            if (const auto changed = SurfaceChange(region, oldRecord, newRecord, surface))
                output[written++] = *changed;
        });
        return Result<NavigationObstacleOverlaySurfaceProjection>::Success({.binding = current.Binding(),
                                                                            .revision = current.Revision(),
                                                                            .surface = surface.id,
                                                                            .topology = surface.topology,
                                                                            .count = written});
    }
}  // namespace Horo::Navigation
