#include "../../PhysicsWorldInternal.h"
#include "../canonical/abi/CanonicalPhysicsStateFpControl.h"
#include "../canonical/backend/CanonicalPhysicsStateBackend.h"
#include "../construction/storage/PhysicsStateConstructionNative.h"
#include "PhysicsStateRuntimeStorage.h"

namespace Horo::Physics {
    /** @copydoc PhysicsReplayConfiguration::Fingerprint */
    Sha256Digest PhysicsReplayConfiguration::Fingerprint() const noexcept {
        std::array<std::byte, 9> arithmetic{};
        arithmetic[0] = static_cast<std::byte>(floatingPoint.architecture);
        for (std::size_t index = 0; index < sizeof(floatingPoint.value); ++index)
            arithmetic[index + 1] = static_cast<std::byte>(floatingPoint.value >> (8 * index));
        const std::array<std::span<const std::byte>, 7> parts{std::as_bytes(std::span{build.bytes}),
                                                              std::as_bytes(std::span{solver.bytes}),
                                                              std::as_bytes(std::span{platform.bytes}),
                                                              std::as_bytes(std::span{filters.bytes}),
                                                              std::as_bytes(std::span{surfaces.bytes}),
                                                              std::as_bytes(std::span{settings.digest.bytes}),
                                                              arithmetic};
        return ComputeSha256Fragments(parts);
    }

    /** @copydoc PhysicsWorld::ReadReplayConfiguration */
    Result<PhysicsReplayConfiguration> PhysicsWorld::ReadReplayConfiguration() const {
        using Output = Result<PhysicsReplayConfiguration>;
        const auto &world = *impl_;
        if (world.runtime->ownerThread != std::this_thread::get_id())
            return Output::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (world.state != PhysicsWorldState::ActiveSolver || world.stepping || world.runtime->state != PhysicsRuntimeState::Ready)
            return Output::Failure(MakeError(PhysicsErrors::InvalidState));
        if (!world.runtime->stateBuild)
            return Output::Failure(MakeError(PhysicsErrors::CapabilityUnavailable, "Replay requires a verified Physics solver image."));
        const auto &build = *world.runtime->stateBuild;
        const auto fp = Detail::ReadCanonicalPhysicsStateFpControl();
        if (fp.HasError())
            return Output::Failure(fp.ErrorValue());
        if (fp.Value() != build.floatingPoint)
            return Output::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        const auto simulation = Detail::ReadCanonicalReplaySimulation(world.native);
        if (simulation.HasError())
            return Output::Failure(simulation.ErrorValue());
        return Output::Success({build.build, build.solver, build.platform, simulation.Value().filters, simulation.Value().surfaces,
                                world.settings.Identity(), fp.Value()});
    }

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
