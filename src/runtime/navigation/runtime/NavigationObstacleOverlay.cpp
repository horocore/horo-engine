#include "Horo/Navigation/NavigationObstacleOverlay.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <type_traits>
#include <variant>

namespace Horo::Navigation {
    namespace {
        using Region = NavigationObstacleOverlayChangedRegion;
        using Record = NavigationObstacleRecord;

        [[nodiscard]] Region Bounds(const Record &record) noexcept {
            Region region{.obstacle = record.id};
            std::visit([&](const auto &shape) {
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

        [[nodiscard]] Region ChangedBounds(const Record *previous, const Record *current) noexcept {
            Region region = Bounds(current && current->enabled ? *current : *previous);
            if (previous && previous->enabled && current && current->enabled) {
                const auto older = Bounds(*previous);
                region.minimumX = std::min(region.minimumX, older.minimumX);
                region.minimumZ = std::min(region.minimumZ, older.minimumZ);
                region.maximumX = std::max(region.maximumX, older.maximumX);
                region.maximumZ = std::max(region.maximumZ, older.maximumZ);
            }
            return region;
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
            const double maximumY = static_cast<double>(cylinder.center.y) + cylinder.halfHeight + radius;
            if (std::max(static_cast<double>(from.y), static_cast<double>(to.y)) < minimumY ||
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
            return std::visit([&](const auto &shape) {
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(shape)>, NavigationDynamicBoxShape>)
                    return IntersectsBox(shape, from, to, radius);
                else
                    return IntersectsCylinder(shape, from, to, radius);
            }, record.shape);
        }

        template <typename Emit>
        void ForEachChanged(const std::span<const Record *> previous, const std::span<const Record *> current, Emit &&emit) {
            std::size_t oldIndex{};
            std::size_t newIndex{};
            while (oldIndex < previous.size() || newIndex < current.size()) {
                const Record *older = oldIndex < previous.size() ? previous[oldIndex] : nullptr;
                const Record *newer = newIndex < current.size() ? current[newIndex] : nullptr;
                if (older && (!newer || older->id < newer->id)) {
                    ++oldIndex;
                    newer = nullptr;
                } else if (newer && (!older || newer->id < older->id)) {
                    ++newIndex;
                    older = nullptr;
                } else {
                    ++oldIndex;
                    ++newIndex;
                }
                if ((!older || !older->enabled) && (!newer || !newer->enabled))
                    continue;
                if (older && newer && older->handle == newer->handle && older->revision == newer->revision)
                    continue;
                emit(ChangedBounds(older, newer));
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
        ForEachChanged(oldSpan, newSpan, [&](const Region &) {
            ++required;
        });
        if (required > output.size())
            return Result<std::size_t>::Failure(MakeError(NavigationErrors::DynamicRegistryCapacityExceeded));
        std::size_t written{};
        ForEachChanged(oldSpan, newSpan, [&](const Region &region) {
            output[written++] = region;
        });
        return Result<std::size_t>::Success(written);
    }
}  // namespace Horo::Navigation
