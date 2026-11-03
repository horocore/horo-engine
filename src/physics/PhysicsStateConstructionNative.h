#pragma once
#include "CanonicalPhysicsRuntime.h"
#include "PhysicsStateConstructionStorage.h"

namespace Horo::Physics::Detail {
    /** @brief Binds the same cold Runtime-owned recipe storage before World creation; owner lane only.
     * Successful call is followed only by no-fail Model ownership transfer. Native stores no second owner/registry.
     */
    [[nodiscard]] Result<void> ConfigureCanonicalConstructionRecipes(CanonicalRuntimeHandle, StateDetail::ConstructionRecipePool *);
    [[nodiscard]] Result<StateDetail::ConstructionRecipeLease> AcquireCanonicalSceneShapeRecipe(CanonicalWorldHandle, ShapeHandle);
    [[nodiscard]] Result<void> ValidateCanonicalRecipeAdmission(StateDetail::ConstructionRecipePool *,
                                                                const StateDetail::ConstructionRecipeLease *);
    [[nodiscard]] Result<StateDetail::ConstructionRecipeLease> ValidateCanonicalCreatedRecipe(
        StateDetail::ConstructionRecipePool *, const StateDetail::ConstructionRecipeLease *,
        const std::optional<PhysicsStateFpControl> &actualControl, const Sha256Digest &actualRecipe);
}  // namespace Horo::Physics::Detail
