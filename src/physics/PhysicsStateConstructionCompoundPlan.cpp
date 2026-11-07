#include "PhysicsStateConstructionPlanInternal.h"

#include <new>

namespace Horo::Physics::Detail {
    namespace {
        Result<StateDetail::ConstructionRecipeLease> Compound(StateDetail::ConstructionRecipePool &pool, CanonicalWorldHandle world,
                                                              std::span<const PhysicsSceneShapeInstance> inputs,
                                                              PhysicsStateFpControl control) {
            using Outcome = Result<StateDetail::ConstructionRecipeLease>;
            if (auto quota = pool.CanReserve(1, inputs.size()); quota.HasError())
                return Outcome::Failure(quota.ErrorValue());
            std::vector<StateDetail::ConstructionRecipeChild> children;
            children.reserve(inputs.size());
            std::uint64_t ordinal{};
            for (const auto &input : inputs) {
                auto child = AcquireCanonicalSceneShapeRecipe(world, input.shape);
                if (child.HasError())
                    return Outcome::Failure(child.ErrorValue());
                children.push_back({std::move(child).Value(), input.localPose, ++ordinal});
            }
            return pool.AddCompound(children, control);
        }
    }  // namespace

    Result<StateDetail::ConstructionRecipeLease> PrepareCompoundConstructionRecipe(
        const std::shared_ptr<StateDetail::ConstructionRecipePool> &pool, CanonicalWorldHandle world,
        std::span<const PhysicsSceneShapeInstance> inputs) {
        using Outcome = Result<StateDetail::ConstructionRecipeLease>;
        if (!pool)
            return Outcome::Success({});
        auto control = BeginConstructionPlan(*pool);
        if (control.HasError())
            return Outcome::Failure(control.ErrorValue());
        if (!control.Value())
            return Outcome::Success({});
        try {
            return AdaptConstructionRecipe(*pool, Compound(*pool, world, inputs, *control.Value()));
        } catch (const std::bad_alloc &) {
            return AdaptConstructionRecipe(*pool, Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded)));
        }
    }
}  // namespace Horo::Physics::Detail
