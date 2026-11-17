#include "PhysicsStateConstructionNative.h"
#include "PhysicsStateRuntimeStorage.h"
#include "PhysicsWorldInternal.h"

namespace Horo::Physics {
    /** @copydoc PhysicsRuntime::BindStateSnapshotBuild */
    Result<void> PhysicsRuntime::BindStateSnapshotBuild(const PhysicsStateBuildBinding &binding, const PhysicsStateRuntimeBudget &budget) {
        if (impl_->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state != PhysicsRuntimeState::Ready || !impl_->identities.empty() || impl_->stateInventory)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (impl_->mode != PhysicsRuntimeMode::Canonical || !impl_->native.value)
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        auto identity = Detail::PrepareCanonicalPhysicsStateBuild(binding);
        if (identity.HasError())
            return Result<void>::Failure(identity.ErrorValue());
        auto storage = Detail::PreparePhysicsStateRuntimeStorage(budget);
        if (storage.HasError())
            return Result<void>::Failure(storage.ErrorValue());
        auto prepared = std::move(storage).Value();
        if (auto native = Detail::ConfigureCanonicalConstructionRecipes(impl_->native, prepared.recipes.get()); native.HasError())
            return native;
        impl_->stateBuildBinding = binding;
        impl_->stateBuild = identity.Value();
        impl_->stateInventory = std::move(prepared.inventory);
        impl_->stateRecipes = std::move(prepared.recipes);
        return Result<void>::Success();
    }
}  // namespace Horo::Physics
