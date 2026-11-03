#include "PhysicsStateRuntimeStorage.h"

#include "CanonicalPhysicsStateFpControl.h"

#include <algorithm>

namespace Horo::Physics::Detail {
    namespace {
        constexpr std::size_t RecipeReferences = sizeof(std::shared_ptr<StateDetail::ConstructionRecipePool>) + sizeof(void *);

        Result<std::size_t> RecipeCeiling(const PhysicsStateRuntimeBudget &budget) {
            using Outcome = Result<std::size_t>;
            if (!budget.maximumCaptureOwners || budget.maximumCaptureOwners > MaximumPhysicsBufferEntries)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            auto remaining = budget.maximumRetainedBytes;
            if (remaining < sizeof(PhysicsStateRuntimeInventory))
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            remaining -= sizeof(PhysicsStateRuntimeInventory);
            if (budget.maximumCaptureOwners > remaining / sizeof(std::shared_ptr<PhysicsStateResourceRecord>))
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            remaining -= budget.maximumCaptureOwners * sizeof(std::shared_ptr<PhysicsStateResourceRecord>);
            if (remaining < RecipeReferences)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Outcome::Success(remaining - RecipeReferences);
        }
    }  // namespace

    /** @copydoc PreparePhysicsStateRuntimeStorage */
    Result<PreparedPhysicsStateRuntimeStorage> PreparePhysicsStateRuntimeStorage(const PhysicsStateRuntimeBudget &budget) {
        using Outcome = Result<PreparedPhysicsStateRuntimeStorage>;
        auto ceiling = RecipeCeiling(budget);
        if (ceiling.HasError())
            return Outcome::Failure(ceiling.ErrorValue());
        auto controls = ReadCanonicalPhysicsStateFpControl();
        if (controls.HasError())
            return Outcome::Failure(controls.ErrorValue());
        auto limits = budget.construction;
        limits.maximumBytes = std::min(limits.maximumBytes, ceiling.Value());
        auto recipes = StateDetail::ConstructionRecipePool::Prepare(limits, controls.Value(), budget.constructionTracking);
        if (recipes.HasError())
            return Outcome::Failure(recipes.ErrorValue());
        auto inventory = PhysicsStateRuntimeInventory::Prepare(budget, recipes.Value()->RetainedPayloadBytes() + RecipeReferences);
        if (inventory.HasError())
            return Outcome::Failure(inventory.ErrorValue());
        return Outcome::Success({std::move(recipes).Value(), std::move(inventory).Value()});
    }
}  // namespace Horo::Physics::Detail
