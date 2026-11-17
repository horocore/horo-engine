#pragma once

/** @file PhysicsStateConstruction.h
 * @brief Physics-owned immutable construction provenance within the canonical state contract.
 */
#include "Horo/Physics/PhysicsShapeDescriptor.h"
#include "Horo/Physics/PhysicsStateConstructionTypes.h"

#include <variant>

namespace Horo::Physics {
    /** @brief A compound's contiguous edge slice; edge order is original constructor/user-data order. */
    struct PhysicsStateCompoundRecipe final {
        std::uint32_t begin{}, count{};
    };

    /** @brief Actual immutable constructor inputs. Recipe row indices are local graph addresses,
     * never persistent body/shape identities or native indices. Every edge references an earlier row.
     * Root traversal starts in stable shape-ID order, preserving each original child order.
     */
    struct PhysicsStateShapeRecipe final {
        PhysicsStateFpControl floatingPoint;
        std::variant<PhysicsShapeDescriptor, PhysicsStateCompoundRecipe> geometry;
    };

    /** @brief Typed constructor edge retaining an exact child recipe independently of retired handles. */
    struct PhysicsStateShapeRecipeEdge final {
        std::uint32_t child{};
        PhysicsPose localPose;
        std::uint64_t userData{};
    };

    /** @brief Durable caller-bound shape identity mapped into this one canonical recipe graph. */
    struct PhysicsStateShapeRecipeRoot final {
        PhysicsStateObjectId shape{};
        std::uint32_t recipe{};
    };

    /** @brief Borrowed immutable shape construction graph; no native or asset authority is implied. */
    struct PhysicsStateShapeRecipes final {
        std::span<const PhysicsStateShapeRecipe> nodes;
        std::span<const PhysicsStateShapeRecipeEdge> edges;
        std::span<const PhysicsStateShapeRecipeRoot> roots;
    };

    /** @brief Independent cold graph/work ceilings, not native memory/RSS estimates. */
    struct PhysicsStateConstructionLimits final {
        std::uint32_t maximumNodes{}, maximumEdges{}, maximumRoots{}, maximumDepth{};
        std::size_t maximumBytes{};
    };

    /** @brief Validates complete closed supported constructor data without native creation or input mutation.
     * @param recipes Complete graph. @param limits Explicit bounded profile.
     * @param current Actual currently admitted reconstruction FP controls; mismatch is unsupported, never normalized.
     * @param depths Caller-owned u32 per node. No allocation is performed; scratch must not alias input.
     * @return Exact malformed/budget/unsupported failure before any receiver state or output mutation.
     * @details Cooked asset hull/mesh/heightfield/compound is not admitted by this analytic vocabulary.
     */
    [[nodiscard]] Result<void> ValidatePhysicsStateShapeRecipes(const PhysicsStateShapeRecipes &recipes,
                                                                const PhysicsStateConstructionLimits &limits, PhysicsStateFpControl current,
                                                                std::span<std::uint32_t> depths);
}  // namespace Horo::Physics
