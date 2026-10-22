#pragma once
/** @file UiLayoutWork.h
 * @brief Value-only invalidation and consumed layout-work provenance, owned by the ordinary layout engine.
 */
#include <compare>
#include <cstdint>
#include <optional>

namespace Horo::Runtime::Ui {
    /** @brief Closed invalidation strength; stronger values include weaker work. */
    enum class UiLayoutDirtyKind : std::uint8_t {
        Arrange,
        Measure,
        Structure,
        All,
    };

    /** @brief Causes encountered while deriving work for one successfully evaluated layout candidate. */
    enum class UiLayoutWorkCause : std::uint16_t {
        None = 0,
        Topology = 1U << 0U,             /**< Initial topology or retained-tree revision replacement. */
        RootInputs = 1U << 1U,           /**< Root constraints, assignment, or font scale changed. */
        SourceRevision = 1U << 2U,       /**< Source changed without precise queued invalidations. */
        ExplicitInvalidation = 1U << 3U, /**< Element, structure, or all invalidation admitted by the owner. */
        AncestorMeasure = 1U << 4U,      /**< Descendant measure invalidation propagated to this ancestor. */
        InheritedConstraints = 1U << 5U, /**< Parent resolved a different child measure range. */
        MeasurementChanged = 1U << 6U,   /**< New measurement required arrangement. */
        ParentAssignment = 1U << 7U,     /**< Parent assigned a different content rectangle. */
        ParentDependency = 1U << 8U,     /**< Assigned extent required the bounded dependent remeasure. */
    };

    /**
     * @brief Value-only evidence of actual work in the candidate that produced a layout generation.
     * @details Counts include both ordinary and bounded retry passes (at most two calls per operation per element).
     * Causes accumulate before dirty flags clear; they explain scheduled work, not pending dirtiness after publication.
     * Reused cached nodes have zero counts and no causes in a fresh candidate. Reusing an unchanged snapshot retains
     * its original publication evidence. Failed or abandoned candidates never change an earlier snapshot's evidence.
     */
    struct UiLayoutWork final {
        UiLayoutWorkCause causes{UiLayoutWorkCause::None}; /**< Union of causes encountered by this candidate. */
        std::optional<UiLayoutDirtyKind> invalidation; /**< Strongest admitted invalidation affecting this node, absent for derived work. */
        std::uint8_t constraintResolutions{};          /**< Actual direct-child constraint resolver calls. */
        std::uint8_t measurements{};                   /**< Actual measure calls, including retries. */
        std::uint8_t arrangements{};                   /**< Actual arrange calls, including retries. */
        std::uint8_t remeasurements{};                 /**< Measure calls specifically made during the bounded retry. */

        /** @brief Checks for one cause in the accumulated evidence. @param cause Cause to query. @return Whether present. */
        [[nodiscard]] constexpr bool HasCause(const UiLayoutWorkCause cause) const noexcept {
            const auto bits = static_cast<std::uint16_t>(cause);
            return bits != 0 && (static_cast<std::uint16_t>(causes) & bits) == bits;
        }

        [[nodiscard]] auto operator<=>(const UiLayoutWork &) const noexcept = default;
    };

}  // namespace Horo::Runtime::Ui
