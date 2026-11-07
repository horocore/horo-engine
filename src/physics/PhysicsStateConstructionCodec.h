#pragma once
#include "Horo/Physics/PhysicsStateConstruction.h"
#include "PhysicsStateWireInternal.h"

namespace Horo::Physics::StateDetail {
    /** @brief Private staging spans; failed decode may write these scratch values but issues no live view/publication. */
    struct ConstructionShapeStorage final {
        std::span<PhysicsStateShapeRecipe> nodes;
        std::span<PhysicsStateShapeRecipeEdge> edges;
        std::span<PhysicsStateShapeRecipeRoot> roots;
    };

    /** @brief Canonical shape subsection of the SINGLE Physics snapshot, not an application Save format. */
    struct ConstructionShapeHeader final {
        std::uint32_t tag{0x31504853U};  // Explicit little-endian SHP1 grammar tag.
        std::uint32_t nodes{}, edges{}, roots{};
    };

    void ConstructionShapeHeaderFields(Wire &, ConstructionShapeHeader &) noexcept;
    [[nodiscard]] bool ConstructionShapeNodeFields(Wire &, PhysicsStateShapeRecipe &, bool reading) noexcept;
    void ConstructionShapeEdgeFields(Wire &, PhysicsStateShapeRecipeEdge &) noexcept;
    void ConstructionShapeRootFields(Wire &, PhysicsStateShapeRecipeRoot &) noexcept;
    [[nodiscard]] Result<std::size_t> EncodeConstructionShapes(const PhysicsStateShapeRecipes &, const PhysicsStateConstructionLimits &,
                                                               PhysicsStateFpControl current, std::span<std::uint32_t> depths,
                                                               std::span<std::byte> output);
    [[nodiscard]] Result<PhysicsStateShapeRecipes> DecodeConstructionShapes(std::span<const std::byte> input,
                                                                            const PhysicsStateConstructionLimits &,
                                                                            PhysicsStateFpControl current, std::span<std::uint32_t> depths,
                                                                            ConstructionShapeStorage privateStorage);
}  // namespace Horo::Physics::StateDetail
