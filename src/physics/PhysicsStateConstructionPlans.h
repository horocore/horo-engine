#pragma once
#include "CanonicalPhysicsRuntime.h"
#include "PhysicsStateConstructionStorage.h"

#include <vector>

namespace Horo::Physics::Detail {
    using ConstructionPlan = std::vector<StateDetail::ConstructionRecipeLease>;
    [[nodiscard]] Result<StateDetail::ConstructionRecipeLease> PrepareAnalyticConstructionRecipe(
        const std::shared_ptr<StateDetail::ConstructionRecipePool> &, const PhysicsShapeDescriptor &);
    [[nodiscard]] Result<StateDetail::ConstructionRecipeLease> PrepareCompoundConstructionRecipe(
        const std::shared_ptr<StateDetail::ConstructionRecipePool> &, CanonicalWorldHandle, std::span<const PhysicsSceneShapeInstance>);
    [[nodiscard]] Result<ConstructionPlan> PrepareGroupConstructionRecipes(const std::shared_ptr<StateDetail::ConstructionRecipePool> &,
                                                                           std::span<const PhysicsSceneGroupShape>);
    [[nodiscard]] Result<StateDetail::ConstructionRecipeLease> PrepareQueryConstructionRecipe(
        const std::shared_ptr<StateDetail::ConstructionRecipePool> &, const PhysicsQueryFixtureShape &);
}  // namespace Horo::Physics::Detail
