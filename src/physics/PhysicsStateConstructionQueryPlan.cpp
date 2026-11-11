#include "PhysicsStateConstructionPlanInternal.h"

#include <new>
#include <type_traits>

namespace Horo::Physics::Detail {
    namespace {
        Result<StateDetail::ConstructionRecipeLease> QueryCompound(StateDetail::ConstructionRecipePool &pool,
                                                                   const PhysicsCompoundShapeDescriptor &compound,
                                                                   PhysicsStateFpControl control) {
            using Outcome = Result<StateDetail::ConstructionRecipeLease>;
            if (compound.children.empty() || compound.children.size() > 256)
                return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            if (auto quota = pool.CanReserve(compound.children.size() + 1, compound.children.size()); quota.HasError())
                return Outcome::Failure(quota.ErrorValue());
            std::vector<StateDetail::ConstructionRecipeChild> children;
            children.reserve(compound.children.size());
            for (const auto &child : compound.children) {
                auto analytic = pool.AddAnalytic(child.geometry, control);
                if (analytic.HasError())
                    return Outcome::Failure(analytic.ErrorValue());
                children.emplace_back(std::move(analytic).Value(), child.localPose, children.size() + 1);
            }
            return pool.AddCompound(children, control);
        }
    }  // namespace

    Result<StateDetail::ConstructionRecipeLease> PrepareQueryConstructionRecipe(
        const std::shared_ptr<StateDetail::ConstructionRecipePool> &pool, const PhysicsQueryFixtureShape &shape) {
        using Outcome = Result<StateDetail::ConstructionRecipeLease>;
        if (!pool)
            return Outcome::Success({});
        auto control = BeginConstructionPlan(*pool);
        if (control.HasError())
            return Outcome::Failure(control.ErrorValue());
        if (!control.Value())
            return Outcome::Success({});
        try {
            auto result = std::visit([&](const auto &value) -> Outcome {
                if constexpr (std::is_same_v<std::decay_t<decltype(value)>, PhysicsCompoundShapeDescriptor>)
                    return QueryCompound(*pool, value, *control.Value());
                else
                    return pool->AddAnalytic(PhysicsShapeDescriptor{value}, *control.Value());
            }, shape);
            return AdaptConstructionRecipe(*pool, std::move(result));
        } catch (const std::bad_alloc &) {
            return AdaptConstructionRecipe(*pool, Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded)));
        }
    }
}  // namespace Horo::Physics::Detail
