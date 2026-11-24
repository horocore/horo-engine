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
                state_->cachedSnapshot = std::make_shared<Detail::NavigationDynamicRegistrySnapshotState>(
                    Detail::NavigationDynamicRegistrySnapshotState{.obstacles = Detail::CaptureRecords(state_->obstacles),
                                                                   .modifiers = Detail::CaptureRecords(state_->modifiers)});
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
