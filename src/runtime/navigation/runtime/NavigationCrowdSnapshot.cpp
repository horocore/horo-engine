#include "Horo/Navigation/NavigationCrowdSnapshot.h"

#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationCrowdSnapshotInternal.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        [[nodiscard]] Result<void> InvalidCapture() {
            return Result<void>::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        }

        [[nodiscard]] Result<void> CapacityExceeded() {
            return Result<void>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }

        [[nodiscard]] bool ValidLimits(const NavigationCrowdSnapshotLimits &limits) noexcept {
            return limits.maximumAgents > 0 && limits.maximumAgents <= 4096 && limits.maximumBoundarySegments > 0 &&
                   limits.maximumBoundarySegments <= 16'384 && limits.maximumCellEntries > 0 && limits.maximumCellEntries <= 65'536 &&
                   limits.maximumNeighborFacts > 0 && limits.maximumNeighborFacts <= 131'072 && limits.maximumBoundaryFacts > 0 &&
                   limits.maximumBoundaryFacts <= 98'304 && limits.maximumPairChecks > 0 && limits.maximumPairChecks <= 16'777'216 &&
                   limits.maximumBoundaryChecks > 0 && limits.maximumBoundaryChecks <= 67'108'864 && std::isfinite(limits.cellSizeMeters) &&
                   limits.cellSizeMeters > 0.0F && limits.mode < AvoidanceExecutionMode::Count;
        }

        [[nodiscard]] bool ValidProfile(const NavigationCrowdProfileFacts &profile) noexcept {
            return profile.profile.IsValid() && std::isfinite(profile.radiusMeters) && profile.radiusMeters > 0.0F &&
                   std::isfinite(profile.neighborRadiusMeters) && profile.neighborRadiusMeters > 0.0F &&
                   profile.neighborRadiusMeters >= profile.radiusMeters && profile.maximumNeighbors <= 32 &&
                   profile.maximumBoundarySegments <= 24 && !profile.obstacleLayers.Empty();
        }

        [[nodiscard]] const NavigationCrowdProfileFacts *FindProfile(const Detail::NavigationCrowdSnapshotStorage &storage,
                                                                     const NavigationAgentProfileId profile) {
            const auto found = std::ranges::lower_bound(storage.profiles, profile, {}, &NavigationCrowdProfileFacts::profile);
            return found != storage.profiles.end() && found->profile == profile ? std::to_address(found) : nullptr;
        }

        /** @brief Validates one complete, stable layer table before admitting any agent facts. */
        [[nodiscard]] Result<void> CopyAvoidanceLayers(const std::span<const NavigationAvoidanceLayerDescriptor> layers,
                                                       Detail::NavigationCrowdSnapshotStorage &storage) {
            if (layers.size() > 64)
                return CapacityExceeded();
            if (layers.empty())
                storage.avoidanceLayers.push_back({.id = NavigationAvoidanceLayerId::Create(1).Value()});
            else
                storage.avoidanceLayers.assign(layers.begin(), layers.end());
            std::ranges::sort(storage.avoidanceLayers, {}, &NavigationAvoidanceLayerDescriptor::bitIndex);
            for (const auto &layer : storage.avoidanceLayers) {
                if (!layer.id.IsValid() || layer.bitIndex >= 64 || (storage.avoidanceLayerBits & (std::uint64_t{1} << layer.bitIndex)) != 0)
                    return InvalidCapture();
                storage.avoidanceLayerBits |= std::uint64_t{1} << layer.bitIndex;
            }
            for (std::size_t index = 0; index < storage.avoidanceLayers.size(); ++index)
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (storage.avoidanceLayers[index].id == storage.avoidanceLayers[previous].id)
                        return InvalidCapture();
            return Result<void>::Success();
        }

        /** @brief Rejects undeclared mask bits and non-finite or out-of-range right-of-way policy. */
        [[nodiscard]] bool ValidAvoidancePolicy(const NavigationAvoidanceAgentPolicy &policy, const std::uint64_t declaredBits) noexcept {
            return policy.layerBit < 64 && (declaredBits & (std::uint64_t{1} << policy.layerBit)) != 0 && policy.avoidsLayers != 0 &&
                   (policy.avoidsLayers & ~declaredBits) == 0 && std::isfinite(policy.priority) && policy.priority >= 0.0F &&
                   policy.priority <= 1.0F;
        }

        [[nodiscard]] Result<void> CopyAgents(const NavigationAgentSnapshot &source, std::span<const NavigationCrowdMotionSample> motions,
                                              const NavigationCrowdSnapshotLimits &limits,
                                              Detail::NavigationCrowdSnapshotStorage &storage) {
            std::vector<NavigationAgentRecord> enabled;
            enabled.reserve(source.Agents().size());
            for (const NavigationAgentRecord &record : source.Agents()) {
                if (record.enabled)
                    enabled.push_back(record);
            }
            if (enabled.size() > limits.maximumAgents || motions.size() > limits.maximumAgents)
                return CapacityExceeded();
            if (enabled.size() != motions.size())
                return InvalidCapture();

            std::ranges::sort(enabled, {}, &NavigationAgentRecord::handle);
            std::vector<NavigationCrowdMotionSample> ordered{motions.begin(), motions.end()};
            std::ranges::sort(ordered, {}, &NavigationCrowdMotionSample::handle);
            storage.agents.reserve(enabled.size());
            for (std::size_t index = 0; index < enabled.size(); ++index) {
                const auto &record = enabled[index];
                const auto &motion = ordered[index];
                const auto *profile = FindProfile(storage, record.profile);
                if (motion.handle != record.handle || !profile || !Math::IsFinite(motion.position) || !Math::IsFinite(motion.velocity) ||
                    !ValidAvoidancePolicy(motion.avoidance, storage.avoidanceLayerBits))
                    return InvalidCapture();
                const float radius = record.radiusOverride.value_or(profile->radiusMeters);
                if (!std::isfinite(radius) || radius <= 0.0F || radius > profile->neighborRadiusMeters)
                    return InvalidCapture();
                if (std::int32_t cell{};
                    !Detail::CrowdCellCoordinate(static_cast<double>(motion.position.x) - profile->neighborRadiusMeters,
                                                 limits.cellSizeMeters, cell) ||
                    !Detail::CrowdCellCoordinate(static_cast<double>(motion.position.x) + profile->neighborRadiusMeters,
                                                 limits.cellSizeMeters, cell) ||
                    !Detail::CrowdCellCoordinate(static_cast<double>(motion.position.z) - profile->neighborRadiusMeters,
                                                 limits.cellSizeMeters, cell) ||
                    !Detail::CrowdCellCoordinate(static_cast<double>(motion.position.z) + profile->neighborRadiusMeters,
                                                 limits.cellSizeMeters, cell))
                    return InvalidCapture();
                storage.agents.push_back({.handle = record.handle,
                                          .profile = record.profile,
                                          .position = motion.position,
                                          .velocity = motion.velocity,
                                          .radiusMeters = radius,
                                          .priority = motion.priority,
                                          .avoidance = motion.avoidance});
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc NavigationCrowdSnapshot::NavigationCrowdSnapshot */
    NavigationCrowdSnapshot::NavigationCrowdSnapshot(std::shared_ptr<const Detail::NavigationCrowdSnapshotStorage> storage) noexcept
        : storage_(std::move(storage)) {}

    /** @copydoc NavigationCrowdSnapshot::IsValid */
    bool NavigationCrowdSnapshot::IsValid() const noexcept {
        return static_cast<bool>(storage_);
    }

    /** @copydoc NavigationCrowdSnapshot::Binding */
    NavigationAgentSceneBinding NavigationCrowdSnapshot::Binding() const noexcept {
        return storage_ ? storage_->binding : NavigationAgentSceneBinding{};
    }

    /** @copydoc NavigationCrowdSnapshot::DynamicRevision */
    NavigationDynamicRegistryRevision NavigationCrowdSnapshot::DynamicRevision() const noexcept {
        return storage_ ? storage_->dynamicRevision : NavigationDynamicRegistryRevision{};
    }

    /** @copydoc NavigationCrowdSnapshot::CaptureTick */
    std::uint64_t NavigationCrowdSnapshot::CaptureTick() const noexcept {
        return storage_ ? storage_->captureTick : 0;
    }

    /** @copydoc NavigationCrowdSnapshot::Mode */
    AvoidanceExecutionMode NavigationCrowdSnapshot::Mode() const noexcept {
        return storage_ ? storage_->mode : AvoidanceExecutionMode::Disabled;
    }

    /** @copydoc NavigationCrowdSnapshot::Agents */
    std::span<const NavigationCrowdAgentFact> NavigationCrowdSnapshot::Agents() const noexcept {
        return storage_ ? std::span<const NavigationCrowdAgentFact>{storage_->agents} : std::span<const NavigationCrowdAgentFact>{};
    }

    /** @copydoc NavigationCrowdSnapshot::NeighborIndices */
    std::span<const std::uint32_t> NavigationCrowdSnapshot::NeighborIndices() const noexcept {
        return storage_ ? std::span<const std::uint32_t>{storage_->neighborIndices} : std::span<const std::uint32_t>{};
    }

    /** @copydoc NavigationCrowdSnapshot::BoundaryIndices */
    std::span<const std::uint32_t> NavigationCrowdSnapshot::BoundaryIndices() const noexcept {
        return storage_ ? std::span<const std::uint32_t>{storage_->boundaryIndices} : std::span<const std::uint32_t>{};
    }

    /** @copydoc NavigationCrowdSnapshot::BoundarySegments */
    std::span<const NavigationCrowdBoundarySegment> NavigationCrowdSnapshot::BoundarySegments() const noexcept {
        return storage_ ? std::span<const NavigationCrowdBoundarySegment>{storage_->segments}
                        : std::span<const NavigationCrowdBoundarySegment>{};
    }

    /** @copydoc NavigationCrowdSnapshot::Cells */
    std::span<const NavigationCrowdCell> NavigationCrowdSnapshot::Cells() const noexcept {
        return storage_ ? std::span<const NavigationCrowdCell>{storage_->cells} : std::span<const NavigationCrowdCell>{};
    }

    /** @copydoc NavigationCrowdSnapshot::CellAgentIndices */
    std::span<const std::uint32_t> NavigationCrowdSnapshot::CellAgentIndices() const noexcept {
        return storage_ ? std::span<const std::uint32_t>{storage_->cellAgentIndices} : std::span<const std::uint32_t>{};
    }

    /** @copydoc NavigationCrowdSnapshot::ProfileTruncation */
    std::span<const NavigationCrowdProfileTruncation> NavigationCrowdSnapshot::ProfileTruncation() const noexcept {
        return storage_ ? std::span<const NavigationCrowdProfileTruncation>{storage_->truncation}
                        : std::span<const NavigationCrowdProfileTruncation>{};
    }

    /** @copydoc NavigationCrowdSnapshot::AvoidanceLayers */
    std::span<const NavigationAvoidanceLayerDescriptor> NavigationCrowdSnapshot::AvoidanceLayers() const noexcept {
        return storage_ ? std::span<const NavigationAvoidanceLayerDescriptor>{storage_->avoidanceLayers}
                        : std::span<const NavigationAvoidanceLayerDescriptor>{};
    }

    /** @copydoc BuildNavigationCrowdSnapshot */
    Result<NavigationCrowdSnapshot> BuildNavigationCrowdSnapshot(
        const NavigationAgentSnapshot &agents, const NavigationDynamicRegistrySnapshot &dynamic,
        const std::span<const NavigationCrowdMotionSample> motions, const std::span<const NavigationCrowdProfileFacts> profiles,
        const NavigationCrowdSnapshotLimits &limits, const std::uint64_t captureTick,
        const std::span<const NavigationAvoidanceLayerDescriptor> avoidanceLayers) {
        if (!ValidLimits(limits) || captureTick == 0 || !agents.IsValid() || !dynamic.IsValid())
            return Result<NavigationCrowdSnapshot>::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        if (agents.Agents().size() > limits.maximumAgents || motions.size() > limits.maximumAgents ||
            profiles.size() > limits.maximumAgents)
            return Result<NavigationCrowdSnapshot>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        const auto &agentBinding = agents.Binding();
        if (const auto &dynamicBinding = dynamic.Binding(); agentBinding.world != dynamicBinding.world ||
                                                            agentBinding.scene != dynamicBinding.scene ||
                                                            agentBinding.sceneGeneration != dynamicBinding.sceneGeneration)
            return Result<NavigationCrowdSnapshot>::Failure(MakeError(NavigationErrors::StaleSnapshot));

        auto storage = std::make_shared<Detail::NavigationCrowdSnapshotStorage>();
        storage->binding = agentBinding;
        storage->dynamicRevision = dynamic.Revision();
        storage->captureTick = captureTick;
        storage->mode = limits.mode;
        if (const auto copiedLayers = CopyAvoidanceLayers(avoidanceLayers, *storage); copiedLayers.HasError())
            return Result<NavigationCrowdSnapshot>::Failure(copiedLayers.ErrorValue());
        storage->profiles.assign(profiles.begin(), profiles.end());
        std::ranges::sort(storage->profiles, {}, &NavigationCrowdProfileFacts::profile);
        for (std::size_t index = 0; index < storage->profiles.size(); ++index) {
            if (!ValidProfile(storage->profiles[index]) ||
                (index > 0 && storage->profiles[index - 1].profile == storage->profiles[index].profile))
                return Result<NavigationCrowdSnapshot>::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
            storage->truncation.push_back({.profile = storage->profiles[index].profile});
        }

        if (const auto copied = CopyAgents(agents, motions, limits, *storage); copied.HasError())
            return Result<NavigationCrowdSnapshot>::Failure(copied.ErrorValue());
        if (const auto partitioned = Detail::PartitionCrowdAgents(limits, *storage); partitioned.HasError())
            return Result<NavigationCrowdSnapshot>::Failure(partitioned.ErrorValue());
        if (limits.mode != AvoidanceExecutionMode::Disabled) {
            if (const auto boundaries = Detail::CaptureCrowdBoundaries(dynamic, limits, *storage); boundaries.HasError())
                return Result<NavigationCrowdSnapshot>::Failure(boundaries.ErrorValue());
            if (const auto gathered = Detail::GatherCrowdFacts(limits, *storage); gathered.HasError())
                return Result<NavigationCrowdSnapshot>::Failure(gathered.ErrorValue());
        }
        return Result<NavigationCrowdSnapshot>::Success(NavigationCrowdSnapshot{std::move(storage)});
    }
}  // namespace Horo::Navigation
