#pragma once

/**
 * @file RenderMemoryTypes.h
 * @brief Backend-neutral renderer memory requirement, placement, and identity value types.
 */

#include "Horo/Runtime/Render/RenderResource.h"

#include <compare>
#include <cstddef>
#include <cstdint>

namespace Horo::Render {
    /** @brief Backend-neutral compatibility class for one renderer backing pool. */
    enum class RenderMemoryClass : std::uint8_t {
        PersistentDevice,
        Upload,
        Readback,
        Transient
    };

    /** @brief Declares whether a native requirement is exact or conservatively estimated. */
    enum class RenderMemoryCostProvenance : std::uint8_t {
        Exact,
        Estimated
    };

    /** @brief Selects shared-block suballocation or one dedicated backing allocation. */
    enum class RenderMemoryAllocationClass : std::uint8_t {
        Suballocated,
        Dedicated
    };

    /** @brief Backend-neutral compatibility identity for requirements that may share one backing pool. */
    struct RenderMemoryCompatibilityId {
        std::uint64_t value{0}; /**< Stable backend-defined compatibility value; zero is invalid. */

        /** @brief Returns whether the compatibility identity is usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMemoryCompatibilityId &) const noexcept = default;
    };

    /** @brief Identifies one admitted host/editor/world/service scope incarnation. */
    struct RenderMemoryScopeId {
        std::uint64_t owner{0};       /**< Stable logical owner identity; zero is invalid. */
        std::uint64_t incarnation{0}; /**< Owner generation that prevents stale-scope reuse; zero is invalid. */

        /** @brief Returns whether both scope identity components are usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner != 0 && incarnation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMemoryScopeId &) const noexcept = default;
    };

    /** @brief Opaque identity of one backend-neutral compatible memory pool. */
    struct RenderMemoryPoolId {
        RenderResourceOwnerId renderer; /**< Renderer incarnation that owns the pool. */
        std::uint64_t value{0};         /**< Pool-local identity; zero is invalid. */

        /** @brief Returns whether the renderer and pool-local identities are usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return renderer.IsValid() && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMemoryPoolId &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one outstanding memory reservation. */
    struct RenderMemoryReservationId {
        RenderResourceOwnerId renderer; /**< Renderer incarnation that owns the reservation. */
        std::uint64_t value{0};         /**< Reservation-local identity; zero is invalid. */

        /** @brief Returns whether the renderer and reservation-local identities are usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return renderer.IsValid() && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMemoryReservationId &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one committed suballocation or dedicated allocation. */
    struct RenderMemoryAllocationId {
        RenderResourceOwnerId renderer; /**< Renderer incarnation that owns the allocation. */
        std::uint64_t value{0};         /**< Allocation-local identity; zero is invalid. */

        /** @brief Returns whether the renderer and allocation-local identities are usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return renderer.IsValid() && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMemoryAllocationId &) const noexcept = default;
    };

    /** @brief Native-free requirements returned by a selected backend before allocation. */
    struct RenderMemoryCostPlan {
        RenderMemoryClass memoryClass{RenderMemoryClass::PersistentDevice};                     /**< Intended host-visible memory class. */
        RenderMemoryAllocationClass allocationClass{RenderMemoryAllocationClass::Suballocated}; /**< Placement policy. */
        RenderMemoryCostProvenance provenance{RenderMemoryCostProvenance::Exact};               /**< Requirement confidence. */
        RenderMemoryCompatibilityId compatibility;                                              /**< Pool compatibility class. */
        std::size_t payloadBytes{0};                                                            /**< Logical resource payload in bytes. */
        std::size_t requiredBytes{0};                                                           /**< Native backing requirement in bytes. */
        std::size_t alignment{1}; /**< Required power-of-two placement alignment. */

        /** @brief Returns whether the plan is internally consistent and bounded. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            const bool memoryClassValid = static_cast<std::uint8_t>(memoryClass) <= static_cast<std::uint8_t>(RenderMemoryClass::Transient);
            const bool allocationClassValid =
                static_cast<std::uint8_t>(allocationClass) <= static_cast<std::uint8_t>(RenderMemoryAllocationClass::Dedicated);
            const bool provenanceValid =
                static_cast<std::uint8_t>(provenance) <= static_cast<std::uint8_t>(RenderMemoryCostProvenance::Estimated);
            return memoryClassValid && allocationClassValid && provenanceValid && compatibility.IsValid() && payloadBytes > 0 &&
                   requiredBytes >= payloadBytes && alignment > 0 && (alignment & (alignment - 1U)) == 0;
        }
    };

    /** @brief Immutable placement returned only after a reservation is committed. */
    struct RenderMemoryAllocation {
        RenderMemoryAllocationId id;                                        /**< Committed allocation identity. */
        RenderMemoryPoolId pool;                                            /**< Accounted backing pool identity. */
        RenderMemoryScopeId scope;                                          /**< Charged logical scope. */
        ResourceOperationId attempt;                                        /**< Resource creation attempt that consumed the reservation. */
        RenderMemoryClass memoryClass{RenderMemoryClass::PersistentDevice}; /**< Accounted memory class. */
        RenderMemoryCompatibilityId compatibility;                          /**< Pool compatibility class. */
        RenderMemoryCostProvenance provenance{RenderMemoryCostProvenance::Exact}; /**< Requirement confidence. */
        std::uint64_t budgetRevision{0};                                          /**< Ledger revision that admitted the reservation. */
        std::size_t offsetBytes{0};                                               /**< Byte offset within the backing allocation. */
        std::size_t payloadBytes{0};                                              /**< Logical resource payload in bytes. */
        std::size_t requiredBytes{0};                                             /**< Native backing requirement in bytes. */
        std::size_t backingBytes{0};                                              /**< Total bytes charged for the backing allocation. */
        RenderMemoryAllocationClass allocationClass{RenderMemoryAllocationClass::Suballocated}; /**< Placement policy. */
    };

    /** @brief Immutable native-free placement of one admitted but unconsumed reservation. */
    struct RenderMemoryPlacement {
        RenderMemoryPoolId pool;                                            /**< Accounted backing pool identity. */
        RenderMemoryScopeId scope;                                          /**< Charged logical scope. */
        ResourceOperationId attempt;                                        /**< Resource creation attempt authorized by this placement. */
        RenderMemoryClass memoryClass{RenderMemoryClass::PersistentDevice}; /**< Accounted memory class. */
        RenderMemoryCompatibilityId compatibility;                          /**< Pool compatibility class. */
        RenderMemoryCostProvenance provenance{RenderMemoryCostProvenance::Exact}; /**< Requirement confidence. */
        std::uint64_t budgetRevision{0};                                          /**< Ledger revision that admitted the reservation. */
        std::size_t offsetBytes{0};                                               /**< Byte offset within the backing allocation. */
        std::size_t payloadBytes{0};                                              /**< Logical resource payload in bytes. */
        std::size_t requiredBytes{0};                                             /**< Native backing requirement in bytes. */
        std::size_t backingBytes{0};                                              /**< Total bytes charged for the backing allocation. */
        RenderMemoryAllocationClass allocationClass{RenderMemoryAllocationClass::Suballocated}; /**< Placement policy. */

        /** @brief Returns whether the placement is structurally valid and fits its backing allocation. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            const bool memoryClassValid = static_cast<std::uint8_t>(memoryClass) <= static_cast<std::uint8_t>(RenderMemoryClass::Transient);
            const bool allocationClassValid =
                static_cast<std::uint8_t>(allocationClass) <= static_cast<std::uint8_t>(RenderMemoryAllocationClass::Dedicated);
            const bool provenanceValid =
                static_cast<std::uint8_t>(provenance) <= static_cast<std::uint8_t>(RenderMemoryCostProvenance::Estimated);
            return memoryClassValid && allocationClassValid && provenanceValid && pool.IsValid() && scope.IsValid() && attempt.IsValid() &&
                   compatibility.IsValid() && budgetRevision != 0 && payloadBytes > 0 && requiredBytes >= payloadBytes &&
                   backingBytes >= requiredBytes && offsetBytes <= backingBytes - requiredBytes;
        }
    };
}  // namespace Horo::Render
