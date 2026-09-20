#pragma once

/** @file PhysicsWorldBudgets.h
 * @brief Bounded world resource reservations, per-tick admission and local-space limits.
 */

#include "Horo/Foundation/Result.h"

#include <cstdint>

namespace Horo::Physics {
    /** @brief Maximum resident shape identities or contact work records in the initial resource profile. */
    inline constexpr std::uint32_t MaximumPhysicsResourceRecords = 1'048'576;
    /** @brief Maximum entries in each independent command, event or query buffer. */
    inline constexpr std::uint32_t MaximumPhysicsBufferEntries = 65'536;
    /** @brief Maximum dedicated temporary solver storage per world, in bytes. */
    inline constexpr std::uint64_t MaximumPhysicsScratchBytes = 256ULL * 1024 * 1024;
    /** @brief Maximum aggregate resident immutable shape storage charged to a world, in bytes. */
    inline constexpr std::uint64_t MaximumPhysicsResidentShapeBytes = 1024ULL * 1024 * 1024;

    /** @brief Explicit temporary-storage overflow behavior; the pinned native allocator cannot unwind a failed tick. */
    enum class PhysicsScratchExhaustionPolicy : std::uint8_t {
        FatalProcess, /**< Supported native behavior: terminate rather than publish or continue corrupted work. */
        FailTick      /**< Requires a future qualified recoverable allocator/solver path; currently unsupported. */
    };

    /** @brief Policy applied after canonical lifecycle ordering when the published event buffer is full. */
    enum class PhysicsEventOverflowPolicy : std::uint8_t {
        DropNewest, /**< Retain the canonical prefix, count the omitted records and keep lifecycle state coherent. */
        FailTick    /**< Suppress this tick and fail the world before publishing a partial event batch. */
    };

    /**
     * @brief Requested per-world limits, not allocations or estimates of native resident memory.
     *
     * Counts and bytes are positive and bounded by the named maxima. In-flight pairs cannot exceed
     * the pair capacity; command/query per-tick work cannot exceed their corresponding capacities.
     * Capacities are independent, never added using unchecked arithmetic. Bodies, collider slots,
     * constraints and scene-plan bytes are separately owned by PhysicsWorldCapacity.
     *
     * Admission that would exceed shape/command/query capacity rejects before mutation. Event evidence
     * and publication overflow follows eventOverflow: DropNewest retains a canonical prefix and
     * advances a coherent reduced lifecycle; FailTick suppresses publication and fails the world.
     * Scratch exhaustion follows the explicit policy below: the pinned allocator terminates the process.
     * No fallback heap growth or unbounded callback buffer is permitted. Resident shape bytes include
     * the world's share of retained leases, not just handles. Native overhead and whole-process
     * budgets still require allocation accounting; these bounds do not promise allocation success.
     */
    struct PhysicsWorldBudgets final {
        std::uint32_t maximumShapes{65'536};
        std::uint32_t maximumContactPairs{65'536};
        std::uint32_t maximumContactConstraints{10'240};
        std::uint32_t maximumInFlightPairs{16'384};
        std::uint32_t maximumCommands{4'096};
        std::uint32_t maximumEvents{8'192};
        std::uint32_t maximumQueries{1'024};
        std::uint32_t maximumCommandsPerTick{4'096};
        std::uint32_t maximumQueriesPerTick{1'024};
        std::uint64_t scratchBytes{64ULL * 1024 * 1024};
        std::uint64_t residentShapeBytes{256ULL * 1024 * 1024};
        PhysicsScratchExhaustionPolicy scratchExhaustion{PhysicsScratchExhaustionPolicy::FatalProcess};
        PhysicsEventOverflowPolicy eventOverflow{PhysicsEventOverflowPolicy::DropNewest};
        bool operator==(const PhysicsWorldBudgets &) const noexcept = default;
    };

    /**
     * @brief Validates bounded positive reservations and cross-field per-tick/in-flight limits.
     * @param budgets Captured resource policy in record counts and bytes.
     * @return Success, CapacityExceeded for an invalid independent limit or DescriptorInvalid for
     * a cross-field mismatch; OperationUnsupported when scratch overflow cannot use the requested policy.
     * Inputs are never changed and nothing is allocated except diagnostics.
     */
    [[nodiscard]] Result<void> ValidatePhysicsWorldBudgets(const PhysicsWorldBudgets &budgets);
}  // namespace Horo::Physics
