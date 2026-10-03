#include "Horo/Physics/PhysicsDebugSnapshot.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>

namespace Horo::Physics {
    namespace {
        // Every category uses variant storage. Charge the full retained slot, not the active alternative.
        constexpr std::size_t RecordBytes = sizeof(PhysicsDebugRecord);

        /** @brief Checks the world ownership of every handle carried by one copied record. */
        [[nodiscard]] bool ValidRecord(const PhysicsDebugRecord &record, const PhysicsWorldId world,
                                       const std::uint64_t simulationTick) noexcept {
            return std::visit([&]<typename T>(const T &value) {
                if constexpr (std::is_same_v<T, PhysicsDebugBody>)
                    return value.body.IsValid() && value.body.world == world;
                else if constexpr (std::is_same_v<T, PhysicsDebugShape>)
                    return value.shape.IsValid() && value.shape.world == world && (!value.body.IsValid() || value.body.world == world);
                else if constexpr (std::is_same_v<T, PhysicsDebugContact>)
                    return value.event.simulationTick == simulationTick && value.event.pair.first.body.IsValid() &&
                           value.event.pair.second.body.IsValid() && value.event.pair.first.body.world == world &&
                           value.event.pair.second.body.world == world;
                else if constexpr (std::is_same_v<T, PhysicsDebugConstraint>)
                    return value.constraint.IsValid() && value.constraint.world == world && value.first.IsValid() &&
                           value.first.world == world && (!value.second.IsValid() || value.second.world == world);
                else if constexpr (std::is_same_v<T, PhysicsDebugBroadphase>)
                    return value.first.IsValid() && value.second.IsValid() && value.first.world == world && value.second.world == world;
                else if constexpr (std::is_same_v<T, PhysicsDebugQuery>)
                    return value.hit.body.IsValid() && value.hit.shape.IsValid() && value.hit.body.world == world &&
                           value.hit.shape.world == world;
                else
                    return true;
            }, record);
        }

        /** @brief Keeps externally supplied counts representable in the bounded output. */
        [[nodiscard]] bool ValidBudget(const PhysicsDebugBudget &budget) noexcept {
            if (budget.maximumPayloadBytes == 0 || budget.maximumPayloadBytes > MaximumPhysicsDebugPayloadBytes)
                return false;
            return std::ranges::all_of(budget.categories, [](const PhysicsDebugCategoryBudget &category) {
                return category.maximumRecords <= MaximumPhysicsDebugRecords &&
                       category.maximumPayloadBytes <= MaximumPhysicsDebugPayloadBytes;
            });
        }

        /** @brief Copies one category while charging its record and byte limits to the aggregate budget. */
        [[nodiscard]] Result<void> CopyCategory(const PhysicsDebugSource &source, const PhysicsDebugBudget &budget, const std::size_t index,
                                                PhysicsDebugCategoryEvidence &evidence, std::vector<PhysicsDebugRecord> &output,
                                                std::uint32_t &payloadBytes, std::uint32_t &totalRecords) {
            const PhysicsDebugSourceCategory &input = source.categories[index];
            const PhysicsDebugCategoryBudget &limit = budget.categories[index];
            if (input.availability > PhysicsDebugAvailability::Available ||
                (input.availability == PhysicsDebugAvailability::Unavailable &&
                 (!input.records.empty() || input.truncatedBeforeCapture != 0 || input.droppedBeforeCapture != 0)))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Malformed Physics debug source category."));
            evidence.availability = input.availability;
            evidence.dropped = input.droppedBeforeCapture;
            if (input.availability == PhysicsDebugAvailability::Unavailable)
                return Result<void>::Success();
            if (input.records.size() > std::numeric_limits<std::uint32_t>::max())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded, "Physics debug source count is unrepresentable."));
            const std::size_t permitted = std::min({input.records.size(), static_cast<std::size_t>(limit.maximumRecords),
                                                    static_cast<std::size_t>(MaximumPhysicsDebugRecords - totalRecords),
                                                    static_cast<std::size_t>(limit.maximumPayloadBytes) / RecordBytes,
                                                    static_cast<std::size_t>(budget.maximumPayloadBytes - payloadBytes) / RecordBytes});
            output.reserve(permitted);
            for (std::size_t recordIndex = 0; recordIndex < permitted; ++recordIndex) {
                const PhysicsDebugRecord &record = input.records[recordIndex];
                if (record.index() != index || !ValidRecord(record, source.world, source.simulationTick))
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Malformed Physics debug record."));
                output.push_back(record);
            }
            evidence.captured = static_cast<std::uint32_t>(output.size());
            const std::uint64_t additionallyTruncated = input.records.size() - output.size();
            if (input.truncatedBeforeCapture > std::numeric_limits<std::uint64_t>::max() - additionallyTruncated)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Physics debug truncation count overflow."));
            evidence.truncated = input.truncatedBeforeCapture + additionallyTruncated;
            evidence.payloadBytes = static_cast<std::uint32_t>(output.size() * RecordBytes);
            payloadBytes += evidence.payloadBytes;
            totalRecords += evidence.captured;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PhysicsDebugSnapshot::Records */
    std::span<const PhysicsDebugRecord> PhysicsDebugSnapshot::Records(const PhysicsDebugCategory category) const noexcept {
        const auto index = static_cast<std::size_t>(category);
        return index < records_.size() ? std::span<const PhysicsDebugRecord>{records_[index]} : std::span<const PhysicsDebugRecord>{};
    }

    /** @copydoc PhysicsDebugSnapshot::Evidence */
    PhysicsDebugCategoryEvidence PhysicsDebugSnapshot::Evidence(const PhysicsDebugCategory category) const noexcept {
        const auto index = static_cast<std::size_t>(category);
        return index < evidence_.size() ? evidence_[index] : PhysicsDebugCategoryEvidence{};
    }

    /** @copydoc PhysicsDebugSnapshot::Matches */
    bool PhysicsDebugSnapshot::Matches(const PhysicsWorldId world, const PhysicsPublishedTick &published) const noexcept {
        return world_ == world && simulationTick_ != 0 && simulationTick_ == published.completedTick &&
               publicationRevision_ == published.publicationRevision;
    }

    /** @copydoc CapturePhysicsDebugSnapshot */
    Result<std::shared_ptr<const PhysicsDebugSnapshot>> CapturePhysicsDebugSnapshot(const PhysicsDebugSource &source,
                                                                                    const PhysicsPublishedTick &published,
                                                                                    const PhysicsDebugBudget &budget) {
        using SnapshotResult = Result<std::shared_ptr<const PhysicsDebugSnapshot>>;
        if (!source.world.IsValid() || source.simulationTick == 0 || source.publicationRevision == 0 ||
            source.simulationTick != published.completedTick || source.publicationRevision != published.publicationRevision)
            return SnapshotResult::Failure(
                MakeError(PhysicsErrors::QuerySnapshotStale, "Physics debug source is not the current completed tick."));
        if (!ValidBudget(budget))
            return SnapshotResult::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Physics debug budget is outside the bounded profile."));

        try {
            auto snapshot = std::make_shared<PhysicsDebugSnapshot>();
            snapshot->world_ = source.world;
            snapshot->simulationTick_ = source.simulationTick;
            snapshot->publicationRevision_ = source.publicationRevision;
            std::uint32_t totalRecords{};
            for (std::size_t index = 0; index < PhysicsDebugCategoryCount; ++index) {
                const auto copied = CopyCategory(source, budget, index, snapshot->evidence_[index], snapshot->records_[index],
                                                 snapshot->payloadBytes_, totalRecords);
                if (copied.HasError())
                    return SnapshotResult::Failure(copied.ErrorValue());
            }
            return SnapshotResult::Success(std::move(snapshot));
        } catch (const std::bad_alloc &) {
            return SnapshotResult::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Unable to allocate bounded Physics debug snapshot."));
        }
    }
}  // namespace Horo::Physics
