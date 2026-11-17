#include "CanonicalPhysicsRuntimeInternal.h"
#include "PhysicsStateConstructionNative.h"

namespace Horo::Physics::Detail {
    /** @copydoc ConfigureCanonicalConstructionRecipes */
    Result<void> ConfigureCanonicalConstructionRecipes(CanonicalRuntimeHandle handle, StateDetail::ConstructionRecipePool *recipes) {
        if (!handle.value || !recipes)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        auto &runtime = *static_cast<CanonicalRuntime *>(handle.value);
        if (!runtime.typesRegistered || runtime.constructionRecipes || runtime.resources.worlds != 0)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        runtime.constructionRecipes = recipes;
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
