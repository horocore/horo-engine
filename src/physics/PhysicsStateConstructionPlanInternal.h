#pragma once
#include "CanonicalPhysicsStateFpControl.h"
#include "PhysicsStateConstructionNative.h"
#include "PhysicsStateConstructionPlans.h"

namespace Horo::Physics::Detail {
    [[nodiscard]] bool OptionalRecipePolicy(const StateDetail::ConstructionRecipePool &) noexcept;
    [[nodiscard]] bool OptionalRecipeFailure(const StateDetail::ConstructionRecipePool &, const Error &) noexcept;
    [[nodiscard]] Result<std::optional<PhysicsStateFpControl>> BeginConstructionPlan(StateDetail::ConstructionRecipePool &);
    [[nodiscard]] Result<StateDetail::ConstructionRecipeLease> AdaptConstructionRecipe(StateDetail::ConstructionRecipePool &,
                                                                                       Result<StateDetail::ConstructionRecipeLease>);
}  // namespace Horo::Physics::Detail
