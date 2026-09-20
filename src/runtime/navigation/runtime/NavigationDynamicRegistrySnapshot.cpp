#include "NavigationDynamicRegistryInternal.h"

#include <new>
#include <utility>

namespace Horo::Navigation {
    NavigationDynamicRegistrySnapshot::NavigationDynamicRegistrySnapshot(
        std::shared_ptr<const Detail::NavigationDynamicRegistrySnapshotState> records) noexcept
        : records_(std::move(records)) {}

    /** @copydoc NavigationDynamicRegistrySnapshot::IsValid */
    bool NavigationDynamicRegistrySnapshot::IsValid() const noexcept {
        return binding_.IsValid() && revision_.IsValid() && records_ != nullptr;
    }

    /** @copydoc NavigationDynamicRegistry::Snapshot */
    Result<NavigationDynamicRegistrySnapshot> NavigationDynamicRegistry::Snapshot() const {
        if (shutdown_ || !state_)
            return Detail::Failure<NavigationDynamicRegistrySnapshot>(NavigationErrors::DynamicRegistryShuttingDown);
        if (!state_->bound)
            return Detail::Failure<NavigationDynamicRegistrySnapshot>(NavigationErrors::NoNavigationData);
        if (!state_->cachedSnapshot) {
            try {
                auto records = std::make_shared<Detail::NavigationDynamicRegistrySnapshotState>();
                for (const auto &slot : state_->obstacles) {
                    if (slot.active)
                        records->obstacles[records->obstacleCount++] = slot.record;
                }
                for (const auto &slot : state_->modifiers) {
                    if (slot.active)
                        records->modifiers[records->modifierCount++] = slot.record;
                }
                Detail::SortRecords(records->obstacles, records->obstacleCount);
                Detail::SortRecords(records->modifiers, records->modifierCount);
                state_->cachedSnapshot = std::move(records);
            } catch (const std::bad_alloc &) {
                return Detail::Failure<NavigationDynamicRegistrySnapshot>(NavigationErrors::DynamicRegistryCapacityExceeded);
            }
        }
        NavigationDynamicRegistrySnapshot snapshot{state_->cachedSnapshot};
        snapshot.binding_ = state_->binding;
        snapshot.revision_ = revision_;
        return Result<NavigationDynamicRegistrySnapshot>::Success(std::move(snapshot));
    }

    /** @copydoc NavigationDynamicRegistrySnapshot::Obstacles */
    std::span<const NavigationObstacleRecord> NavigationDynamicRegistrySnapshot::Obstacles() const noexcept {
        return Detail::SnapshotRecords<NavigationObstacleRecord>(records_);
    }

    /** @copydoc NavigationDynamicRegistrySnapshot::Modifiers */
    std::span<const NavigationModifierRecord> NavigationDynamicRegistrySnapshot::Modifiers() const noexcept {
        return Detail::SnapshotRecords<NavigationModifierRecord>(records_);
    }

    /** @copydoc NavigationDynamicRegistrySnapshot::FindObstacle */
    Result<NavigationObstacleRecord> NavigationDynamicRegistrySnapshot::FindObstacle(const NavigationObstacleHandle handle) const {
        return Detail::FindSnapshotRecord(Obstacles(), handle);
    }

    /** @copydoc NavigationDynamicRegistrySnapshot::FindModifier */
    Result<NavigationModifierRecord> NavigationDynamicRegistrySnapshot::FindModifier(const NavigationModifierHandle handle) const {
        return Detail::FindSnapshotRecord(Modifiers(), handle);
    }
}  // namespace Horo::Navigation
