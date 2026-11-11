#include "PhysicsStateConstructionPlanInternal.h"

#include <new>

namespace Horo::Physics::Detail {
    namespace {
        Result<std::size_t> EdgeCount(std::span<const PhysicsSceneGroupShape> inputs, std::size_t maximum) {
            std::size_t total{};
            for (std::size_t index = 0; index < inputs.size(); ++index) {
                const auto *children = std::get_if<std::vector<PhysicsSceneGroupShape::Child>>(&inputs[index].geometry);
                if (!children)
                    continue;
                if (children->empty() || children->size() > 256)
                    return Result<std::size_t>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                if (children->size() > maximum - total)
                    return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
                for (const auto &child : *children)
                    if (child.shape >= index)
                        return Result<std::size_t>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                total += children->size();
            }
            return Result<std::size_t>::Success(total);
        }

        Result<StateDetail::ConstructionRecipeLease> Row(StateDetail::ConstructionRecipePool &pool, const PhysicsSceneGroupShape &input,
                                                         const ConstructionPlan &previous, PhysicsStateFpControl control) {
            if (const auto *analytic = std::get_if<PhysicsShapeDescriptor>(&input.geometry))
                return pool.AddAnalytic(*analytic, control);
            const auto &rows = std::get<std::vector<PhysicsSceneGroupShape::Child>>(input.geometry);
            std::vector<StateDetail::ConstructionRecipeChild> children;
            children.reserve(rows.size());
            for (std::size_t index = 0; index < rows.size(); ++index)
                children.emplace_back(previous[rows[index].shape], rows[index].localPose, index + 1);
            return pool.AddCompound(children, control);
        }

        Result<ConstructionPlan> Group(StateDetail::ConstructionRecipePool &pool, std::span<const PhysicsSceneGroupShape> inputs,
                                       PhysicsStateFpControl control) {
            using Outcome = Result<ConstructionPlan>;
            if (inputs.size() > pool.Limits().maximumNodes)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            auto edges = EdgeCount(inputs, pool.Limits().maximumEdges);
            if (edges.HasError())
                return Outcome::Failure(edges.ErrorValue());
            if (auto quota = pool.CanReserve(inputs.size(), edges.Value()); quota.HasError())
                return Outcome::Failure(quota.ErrorValue());
            ConstructionPlan plan;
            plan.reserve(inputs.size());
            for (const auto &input : inputs) {
                auto next = Row(pool, input, plan, control);
                if (next.HasError())
                    return Outcome::Failure(next.ErrorValue());
                plan.push_back(std::move(next).Value());
            }
            return Outcome::Success(std::move(plan));
        }

        Result<ConstructionPlan> Adapt(StateDetail::ConstructionRecipePool &pool, Result<ConstructionPlan> result) {
            if (result.HasError() && OptionalRecipeFailure(pool, result.ErrorValue()))
                return Result<ConstructionPlan>::Success({});
            return result;
        }
    }  // namespace

    Result<ConstructionPlan> PrepareGroupConstructionRecipes(const std::shared_ptr<StateDetail::ConstructionRecipePool> &pool,
                                                             std::span<const PhysicsSceneGroupShape> inputs) {
        using Outcome = Result<ConstructionPlan>;
        if (!pool)
            return Outcome::Success({});
        auto control = BeginConstructionPlan(*pool);
        if (control.HasError())
            return Outcome::Failure(control.ErrorValue());
        if (!control.Value())
            return Outcome::Success({});
        try {
            return Adapt(*pool, Group(*pool, inputs, *control.Value()));
        } catch (const std::bad_alloc &) {
            return Adapt(*pool, Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded)));
        }
    }
}  // namespace Horo::Physics::Detail
