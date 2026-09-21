#include "Horo/Physics/PhysicsWorldBudgets.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <array>
#include <string>

namespace Horo::Physics {
    namespace {
        /** @brief One typed numeric limit plus diagnostic label; labels never select behavior. */
        struct ResourceLimit final {
            std::uint64_t requested;
            std::uint64_t maximum;
            const char *name;
        };
    }  // namespace

    /** @copydoc ValidatePhysicsWorldBudgets */
    Result<void> ValidatePhysicsWorldBudgets(const PhysicsWorldBudgets &budgets) {
        if (budgets.scratchExhaustion != PhysicsScratchExhaustionPolicy::FatalProcess)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::OperationUnsupported, "The pinned scratch allocator supports fatal process exhaustion only."));
        if (budgets.eventOverflow > PhysicsEventOverflowPolicy::FailTick)
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown Physics event overflow policy."));
        const std::array limits{
            ResourceLimit{budgets.maximumShapes, MaximumPhysicsResourceRecords, "shape count"},
            ResourceLimit{budgets.maximumContactPairs, MaximumPhysicsResourceRecords, "contact pair count"},
            ResourceLimit{budgets.maximumContactConstraints, MaximumPhysicsResourceRecords, "contact constraint count"},
            ResourceLimit{budgets.maximumInFlightPairs, MaximumPhysicsResourceRecords, "in-flight pair count"},
            ResourceLimit{budgets.maximumCommands, MaximumPhysicsBufferEntries, "command capacity"},
            ResourceLimit{budgets.maximumEvents, MaximumPhysicsBufferEntries, "event capacity"},
            ResourceLimit{budgets.maximumQueries, MaximumPhysicsBufferEntries, "query capacity"},
            ResourceLimit{budgets.maximumCommandsPerTick, MaximumPhysicsBufferEntries, "per-tick command count"},
            ResourceLimit{budgets.maximumQueriesPerTick, MaximumPhysicsBufferEntries, "per-tick query count"},
            ResourceLimit{budgets.scratchBytes, MaximumPhysicsScratchBytes, "scratch bytes"},
            ResourceLimit{budgets.residentShapeBytes, MaximumPhysicsResidentShapeBytes, "resident shape bytes"},
        };
        for (const auto &limit : limits) {
            if (limit.requested == 0 || limit.requested > limit.maximum)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::CapacityExceeded,
                              std::string("Physics ") + limit.name + " must be positive and within the resource profile."));
        }
        if (budgets.maximumInFlightPairs > budgets.maximumContactPairs || budgets.maximumCommandsPerTick > budgets.maximumCommands ||
            budgets.maximumQueriesPerTick > budgets.maximumQueries)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid,
                                                   "In-flight pairs and per-tick command/query work cannot exceed their capacities."));
        return Result<void>::Success();
    }
}  // namespace Horo::Physics
