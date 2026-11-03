#include "PhysicsStateConstructionPlanInternal.h"

namespace Horo::Physics::Detail {
    bool OptionalRecipePolicy(const StateDetail::ConstructionRecipePool &pool) noexcept {
        return pool.Tracking() == PhysicsStateConstructionTracking::Optional;
    }

    bool OptionalRecipeFailure(const StateDetail::ConstructionRecipePool &pool, const Error &error) noexcept {
        if (!OptionalRecipePolicy(pool))
            return false;
        const auto code = error.code.Value();
        return code == PhysicsErrors::CapacityExceeded.code.Value() || code == PhysicsErrors::OperationUnsupported.code.Value() ||
               code == PhysicsErrors::CapabilityUnavailable.code.Value();
    }

    Result<std::optional<PhysicsStateFpControl>> BeginConstructionPlan(StateDetail::ConstructionRecipePool &pool) {
        using Outcome = Result<std::optional<PhysicsStateFpControl>>;
        if (auto collected = pool.Collect(64); collected.HasError())
            return Outcome::Failure(collected.ErrorValue());
        auto control = ReadCanonicalPhysicsStateFpControl();
        if (control.HasError())
            return OptionalRecipeFailure(pool, control.ErrorValue()) ? Outcome::Success({}) : Outcome::Failure(control.ErrorValue());
        if (!OptionalRecipePolicy(pool) && control.Value() != pool.Profile())
            return Outcome::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        return Outcome::Success(control.Value());
    }

    Result<StateDetail::ConstructionRecipeLease> AdaptConstructionRecipe(StateDetail::ConstructionRecipePool &pool,
                                                                         Result<StateDetail::ConstructionRecipeLease> result) {
        if (result.HasError() && OptionalRecipeFailure(pool, result.ErrorValue()))
            return Result<StateDetail::ConstructionRecipeLease>::Success({});
        return result;
    }

    Result<StateDetail::ConstructionRecipeLease> PrepareAnalyticConstructionRecipe(
        const std::shared_ptr<StateDetail::ConstructionRecipePool> &pool, const PhysicsShapeDescriptor &shape) {
        using Outcome = Result<StateDetail::ConstructionRecipeLease>;
        if (!pool)
            return Outcome::Success({});
        auto control = BeginConstructionPlan(*pool);
        if (control.HasError())
            return Outcome::Failure(control.ErrorValue());
        if (!control.Value())
            return Outcome::Success({});
        return AdaptConstructionRecipe(*pool, pool->AddAnalytic(shape, *control.Value()));
    }
}  // namespace Horo::Physics::Detail
