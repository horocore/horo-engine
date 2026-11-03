#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateFpControl.h"
#include "PhysicsStateConstructionNative.h"

namespace Horo::Physics::Detail {
    namespace {
        bool Required(const StateDetail::ConstructionRecipePool &pool) noexcept {
            return pool.Tracking() == PhysicsStateConstructionTracking::Required;
        }

        Result<StateDetail::ConstructionRecipeLease> Missing(StateDetail::ConstructionRecipePool &pool, const ErrorCodeDescriptor &error) {
            return Required(pool) ? Result<StateDetail::ConstructionRecipeLease>::Failure(MakeError(error))
                                  : Result<StateDetail::ConstructionRecipeLease>::Success({});
        }
    }  // namespace

    namespace {
        Result<void> AdmitRecipeProfile(StateDetail::ConstructionRecipePool &pool, const StateDetail::ConstructionRecipeLease &recipe) {
            auto actual = pool.Read(recipe);
            if (actual.HasError())
                return Result<void>::Failure(actual.ErrorValue());
            auto fp = ReadCanonicalPhysicsStateFpControl();
            if (fp.HasError())
                return Required(pool) ? Result<void>::Failure(fp.ErrorValue()) : Result<void>::Success();
            return !Required(pool) || fp.Value() == actual.Value().floatingPoint
                       ? Result<void>::Success()
                       : Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        }
    }  // namespace

    Result<void> ValidateCanonicalRecipeAdmission(StateDetail::ConstructionRecipePool *pool,
                                                  const StateDetail::ConstructionRecipeLease *recipe) {
        if (!pool)
            return recipe && recipe->IsValid() ? Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid))
                                               : Result<void>::Success();
        if (!recipe || !recipe->IsValid())
            return Required(*pool) ? Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable)) : Result<void>::Success();
        return AdmitRecipeProfile(*pool, *recipe);
    }

    Result<StateDetail::ConstructionRecipeLease> ValidateCanonicalCreatedRecipe(StateDetail::ConstructionRecipePool *pool,
                                                                                const StateDetail::ConstructionRecipeLease *recipe,
                                                                                const std::optional<PhysicsStateFpControl> &control,
                                                                                const Sha256Digest &actualRecipe) {
        using Outcome = Result<StateDetail::ConstructionRecipeLease>;
        if (!pool)
            return Outcome::Success({});
        if (!recipe || !recipe->IsValid() || !control)
            return Missing(*pool, PhysicsErrors::CapabilityUnavailable);
        auto input = pool->Read(*recipe);
        if (input.HasError())
            return Outcome::Failure(input.ErrorValue());
        if (input.Value().floatingPoint != *control)
            return Missing(*pool, PhysicsErrors::OperationUnsupported);
        auto expected = pool->Digest(*recipe);
        if (expected.HasError())
            return Outcome::Failure(expected.ErrorValue());
        if (expected.Value() != actualRecipe)
            return Missing(*pool, PhysicsErrors::DescriptorInvalid);
        return Outcome::Success(*recipe);
    }

    Result<StateDetail::ConstructionRecipeLease> AcquireCanonicalSceneShapeRecipe(CanonicalWorldHandle handle, ShapeHandle shape) {
        using Outcome = Result<StateDetail::ConstructionRecipeLease>;
        if (!handle.value)
            return Outcome::Failure(MakeError(PhysicsErrors::WorldInvalid));
        const auto &world = *static_cast<const CanonicalWorld *>(handle.value);
        auto found = std::ranges::find(world.scene.shapes, shape, &CanonicalSceneShapeRecord::handle);
        if (found == world.scene.shapes.end())
            return Outcome::Failure(MakeError(PhysicsErrors::HandleStale));
        if (!found->construction.IsValid())
            return Outcome::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        return Outcome::Success(found->construction);
    }
}  // namespace Horo::Physics::Detail
