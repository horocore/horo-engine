#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateConstructionNative.h"

namespace Horo::Physics::Detail {
    Result<void> ConfigureCanonicalConstructionRecipes(CanonicalRuntimeHandle, StateDetail::ConstructionRecipePool *) {
        return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    Result<StateDetail::ConstructionRecipeLease> AcquireCanonicalSceneShapeRecipe(CanonicalWorldHandle, ShapeHandle) {
        return Result<StateDetail::ConstructionRecipeLease>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    Result<void> ValidateCanonicalRecipeAdmission(const StateDetail::ConstructionRecipePool *,
                                                  const StateDetail::ConstructionRecipeLease *) {
        return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    Result<StateDetail::ConstructionRecipeLease> ValidateCanonicalCreatedRecipe(const StateDetail::ConstructionRecipePool *,
                                                                                const StateDetail::ConstructionRecipeLease *,
                                                                                const std::optional<PhysicsStateFpControl> &,
                                                                                const Sha256Digest &) {
        return Result<StateDetail::ConstructionRecipeLease>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }
}  // namespace Horo::Physics::Detail
