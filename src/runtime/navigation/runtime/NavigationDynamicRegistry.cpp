#include "NavigationDynamicRegistryInternal.h"

#include <cmath>
#include <new>
#include <utility>

namespace Horo::Navigation {
    /** @copydoc NavigationDynamicProvenance::IsValid */
    bool NavigationDynamicProvenance::IsValid() const noexcept {
        if (!world.IsValid() || !scene.IsValid() || !sceneGeneration.IsValid() || !owner.IsValid() || !ownerGeneration.IsValid() ||
            !sourceRevision.IsValid() || source >= NavigationDynamicSourceKind::Count)
            return false;
        if (source == NavigationDynamicSourceKind::AuthoredModifier)
            return authoredModifier.IsValid();
        return !authoredModifier.IsValid();
    }

    /** @copydoc NavigationDynamicBoxShape::IsValid */
    bool NavigationDynamicBoxShape::IsValid() const noexcept {
        return Math::IsFinite(center) && Math::IsFinite(halfExtents) && halfExtents.x > 0.0F && halfExtents.y > 0.0F &&
               halfExtents.z > 0.0F;
    }

    /** @copydoc NavigationDynamicCylinderShape::IsValid */
    bool NavigationDynamicCylinderShape::IsValid() const noexcept {
        return Math::IsFinite(center) && std::isfinite(radius) && std::isfinite(halfHeight) && radius > 0.0F && halfHeight > 0.0F;
    }

    /** @copydoc NavigationObstacleDescriptor::IsValid */
    bool NavigationObstacleDescriptor::IsValid() const noexcept {
        return id.IsValid() && provenance.IsValid() && Detail::ValidShape(shape) && !layers.Empty() && Detail::ValidPriority(priority) &&
               updateTick != 0;
    }

    /** @copydoc NavigationModifierDescriptor::IsValid */
    bool NavigationModifierDescriptor::IsValid() const noexcept {
        return id.IsValid() && provenance.IsValid() && Detail::ValidShape(shape) && !layers.Empty() && Detail::ValidPriority(priority) &&
               operation < NavigationDynamicModifierOperation::Count && Detail::ValidOverride(*this) && updateTick != 0;
    }

    /** @copydoc NavigationDynamicRegistryLimits::IsValid */
    bool NavigationDynamicRegistryLimits::IsValid() const noexcept {
        return maximumObstacles > 0 && maximumObstacles <= NavigationDynamicRegistryHardLimits::Obstacles && maximumModifiers > 0 &&
               maximumModifiers <= NavigationDynamicRegistryHardLimits::Modifiers && maximumPendingCommands > 0 &&
               maximumPendingCommands <= NavigationDynamicRegistryHardLimits::PendingCommands && maximumMutationsPerCommit > 0 &&
               maximumMutationsPerCommit <= NavigationDynamicRegistryHardLimits::MutationsPerCommit &&
               maximumMutationsPerCommit <= maximumPendingCommands && minimumUpdateIntervalTicks > 0 &&
               minimumUpdateIntervalTicks <= NavigationDynamicRegistryHardLimits::MaximumUpdateIntervalTicks;
    }

    NavigationDynamicRegistry::NavigationDynamicRegistry(std::unique_ptr<Detail::NavigationDynamicRegistryState> state) noexcept
        : state_(std::move(state)) {}

    /** @copydoc NavigationDynamicRegistry::Create */
    Result<NavigationDynamicRegistry> NavigationDynamicRegistry::Create(const NavigationDynamicRegistryLimits &limits) {
        if (!limits.IsValid())
            return Detail::Failure<NavigationDynamicRegistry>(NavigationErrors::DynamicRegistryInvalid);
        try {
            return Result<NavigationDynamicRegistry>::Success(
                NavigationDynamicRegistry{std::make_unique<Detail::NavigationDynamicRegistryState>(limits)});
        } catch (const std::bad_alloc &) {
            return Detail::Failure<NavigationDynamicRegistry>(NavigationErrors::DynamicRegistryCapacityExceeded);
        }
    }

    NavigationDynamicRegistry::NavigationDynamicRegistry(NavigationDynamicRegistry &&other) noexcept = default;
    NavigationDynamicRegistry::~NavigationDynamicRegistry() = default;

    /** @copydoc NavigationDynamicRegistry::StageRegisterObstacle */
    Result<NavigationObstacleHandle> NavigationDynamicRegistry::StageRegisterObstacle(const NavigationObstacleDescriptor &descriptor) {
        if (!state_)
            return Detail::Failure<NavigationObstacleHandle>(NavigationErrors::DynamicRegistryShuttingDown);
        return Detail::StageRegistration<NavigationObstacleHandle, Detail::PendingObstacleRegistration>(*state_, pendingCount_, shutdown_,
                                                                                                        descriptor);
    }

    /** @copydoc NavigationDynamicRegistry::StageRegisterModifier */
    Result<NavigationModifierHandle> NavigationDynamicRegistry::StageRegisterModifier(const NavigationModifierDescriptor &descriptor) {
        if (!state_)
            return Detail::Failure<NavigationModifierHandle>(NavigationErrors::DynamicRegistryShuttingDown);
        return Detail::StageRegistration<NavigationModifierHandle, Detail::PendingModifierRegistration>(*state_, pendingCount_, shutdown_,
                                                                                                        descriptor);
    }

    /** @copydoc NavigationDynamicRegistry::StageUpdateObstacle */
    Result<void> NavigationDynamicRegistry::StageUpdateObstacle(const NavigationObstacleHandle handle,
                                                                const NavigationDynamicRecordRevision expectedRevision,
                                                                const NavigationObstacleDescriptor &descriptor) {
        if (!state_)
            return Detail::Failure<void>(NavigationErrors::DynamicRegistryShuttingDown);
        return Detail::StageUpdate<NavigationObstacleHandle, Detail::PendingObstacleUpdate>(*state_, pendingCount_, shutdown_, handle,
                                                                                            expectedRevision, descriptor);
    }

    /** @copydoc NavigationDynamicRegistry::StageUpdateModifier */
    Result<void> NavigationDynamicRegistry::StageUpdateModifier(const NavigationModifierHandle handle,
                                                                const NavigationDynamicRecordRevision expectedRevision,
                                                                const NavigationModifierDescriptor &descriptor) {
        if (!state_)
            return Detail::Failure<void>(NavigationErrors::DynamicRegistryShuttingDown);
        return Detail::StageUpdate<NavigationModifierHandle, Detail::PendingModifierUpdate>(*state_, pendingCount_, shutdown_, handle,
                                                                                            expectedRevision, descriptor);
    }

    /** @copydoc NavigationDynamicRegistry::StageRemoveObstacle */
    Result<void> NavigationDynamicRegistry::StageRemoveObstacle(const NavigationObstacleHandle handle,
                                                                const NavigationDynamicRecordRevision expectedRevision) {
        if (!state_)
            return Detail::Failure<void>(NavigationErrors::DynamicRegistryShuttingDown);
        return Detail::StageRemoval<NavigationObstacleHandle, Detail::PendingObstacleRemoval>(*state_, pendingCount_, shutdown_, handle,
                                                                                              expectedRevision);
    }

    /** @copydoc NavigationDynamicRegistry::StageRemoveModifier */
    Result<void> NavigationDynamicRegistry::StageRemoveModifier(const NavigationModifierHandle handle,
                                                                const NavigationDynamicRecordRevision expectedRevision) {
        if (!state_)
            return Detail::Failure<void>(NavigationErrors::DynamicRegistryShuttingDown);
        return Detail::StageRemoval<NavigationModifierHandle, Detail::PendingModifierRemoval>(*state_, pendingCount_, shutdown_, handle,
                                                                                              expectedRevision);
    }

    /** @copydoc NavigationDynamicRegistry::BeginShutdown */
    void NavigationDynamicRegistry::BeginShutdown() noexcept {
        if (shutdown_)
            return;
        ClearStaged();
        shutdown_ = true;
    }
}  // namespace Horo::Navigation
