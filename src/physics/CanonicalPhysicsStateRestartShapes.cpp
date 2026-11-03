#include "CanonicalPhysicsStateRestartShapes.h"

#include "CanonicalPhysicsStateShapeCreation.h"
#include "CanonicalPhysicsStateShapeRecipe.h"
#include "PhysicsStateConstructionNative.h"

#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

namespace Horo::Physics::Detail {
    namespace {
        Result<StateDetail::ConstructionRecipeLease> CompoundRecipe(StateDetail::ConstructionRecipePool &pool,
                                                                    const PhysicsStateShapeRecipes &manifest,
                                                                    const CanonicalRestartShapes &previous,
                                                                    PhysicsStateCompoundRecipe group, PhysicsStateFpControl control) {
            std::vector<StateDetail::ConstructionRecipeChild> children;
            children.reserve(group.count);
            for (const auto &edge : manifest.edges.subspan(group.begin, group.count))
                children.push_back({previous.recipes[edge.child], edge.localPose, edge.userData});
            return pool.AddCompound(children, control);
        }

        Result<StateDetail::ConstructionRecipeLease> Recipe(StateDetail::ConstructionRecipePool &pool,
                                                            const PhysicsStateShapeRecipes &manifest,
                                                            const CanonicalRestartShapes &previous, const PhysicsStateShapeRecipe &recipe) {
            if (const auto *analytic = std::get_if<PhysicsShapeDescriptor>(&recipe.geometry))
                return pool.AddAnalytic(*analytic, recipe.floatingPoint);
            return CompoundRecipe(pool, manifest, previous, std::get<PhysicsStateCompoundRecipe>(recipe.geometry), recipe.floatingPoint);
        }

        Result<JPH::Ref<JPH::Shape>> CompoundNative(const PhysicsStateShapeRecipes &manifest, const CanonicalRestartShapes &previous,
                                                    PhysicsStateCompoundRecipe group) {
            JPH::StaticCompoundShapeSettings settings;
            for (const auto &edge : manifest.edges.subspan(group.begin, group.count))
                settings.AddShape(ToNative(edge.localPose.translation), ToNative(edge.localPose.rotation),
                                  previous.native[edge.child].GetPtr(), static_cast<JPH::uint32>(edge.userData));
            auto created = settings.Create();
            if (created.HasError())
                return Result<JPH::Ref<JPH::Shape>>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid));
            return Result<JPH::Ref<JPH::Shape>>::Success(created.Get());
        }

        Result<CanonicalPhysicsStateCreatedShape> Native(const PhysicsStateShapeRecipes &manifest, const CanonicalRestartShapes &previous,
                                                         const PhysicsStateShapeRecipe &recipe) {
            return ObserveCanonicalPhysicsShapeCreation([&]() -> Result<JPH::Ref<JPH::Shape>> {
                if (const auto *analytic = std::get_if<PhysicsShapeDescriptor>(&recipe.geometry))
                    return CreateNativeShape(*analytic);
                return CompoundNative(manifest, previous, std::get<PhysicsStateCompoundRecipe>(recipe.geometry));
            });
        }

        Sha256Digest ActualDigest(const PhysicsStateShapeRecipes &manifest, const CanonicalRestartShapes &previous,
                                  const PhysicsStateShapeRecipe &recipe, const std::optional<PhysicsStateFpControl> &actual) noexcept {
            if (const auto *analytic = std::get_if<PhysicsShapeDescriptor>(&recipe.geometry))
                return BindCanonicalShapeCreationRecipe(CanonicalPrimitiveRecipe(*analytic), actual);
            const auto group = std::get<PhysicsStateCompoundRecipe>(recipe.geometry);
            auto hash = BeginCanonicalCompoundRecipe(group.count);
            for (const auto &edge : manifest.edges.subspan(group.begin, group.count)) {
                hash.Digest(previous.digests[edge.child]);
                hash.Pose(edge.localPose);
            }
            return BindCanonicalShapeCreationRecipe(hash.Finish(), actual);
        }

        Result<std::size_t> Reservation(const PhysicsStateShapeRecipes &manifest, const CanonicalRestartShapes &previous,
                                        const PhysicsStateShapeRecipe &recipe, std::size_t own) {
            if (const auto *group = std::get_if<PhysicsStateCompoundRecipe>(&recipe.geometry))
                for (const auto &edge : manifest.edges.subspan(group->begin, group->count))
                    if (!AddCanonicalPhysicsShapeReservation(previous.requestedBytes[edge.child], own))
                        return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Result<std::size_t>::Success(own);
        }

        Result<void> Append(CanonicalRestartShapes &out, StateDetail::ConstructionRecipePool &pool,
                            const PhysicsStateShapeRecipes &manifest, const PhysicsStateShapeRecipe &recipe) {
            auto planned = Recipe(pool, manifest, out, recipe);
            if (planned.HasError())
                return Result<void>::Failure(planned.ErrorValue());
            if (auto admitted = ValidateCanonicalRecipeAdmission(&pool, &planned.Value()); admitted.HasError())
                return admitted;
            auto created = Native(manifest, out, recipe);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            auto shape = std::move(created).Value();
            const auto digest = ActualDigest(manifest, out, recipe, shape.creationControl);
            auto witness = ValidateCanonicalCreatedRecipe(&pool, &planned.Value(), shape.creationControl, digest);
            if (witness.HasError())
                return Result<void>::Failure(witness.ErrorValue());
            auto reserved = Reservation(manifest, out, recipe, shape.nativeRequestedBytes);
            if (reserved.HasError())
                return Result<void>::Failure(reserved.ErrorValue());
            out.native.push_back(std::move(shape.shape));
            out.requestedBytes.push_back(reserved.Value());
            out.recipes.push_back(std::move(witness).Value());
            out.digests.push_back(digest);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PrepareCanonicalRestartShapes */
    Result<CanonicalRestartShapes> PrepareCanonicalRestartShapes(CanonicalRuntime &runtime, const PhysicsStateShapeRecipes &manifest,
                                                                 const CancellationToken &cancellation) {
        using Outcome = Result<CanonicalRestartShapes>;
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(PhysicsErrors::StateCancelled));
        auto *pool = runtime.constructionRecipes;
        if (!pool || pool->Tracking() != PhysicsStateConstructionTracking::Required)
            return Outcome::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (auto collected = pool->Collect(pool->Limits().maximumNodes); collected.HasError())
            return Outcome::Failure(collected.ErrorValue());
        if (auto quota = pool->CanReserve(manifest.nodes.size(), manifest.edges.size()); quota.HasError())
            return Outcome::Failure(quota.ErrorValue());
        // Only caller-private bounded staging is allocated. Validation finishes before the first native constructor.
        std::vector<std::uint32_t> depths(manifest.nodes.size());
        if (auto valid = ValidatePhysicsStateShapeRecipes(manifest, pool->Limits(), pool->Profile(), depths); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        CanonicalRestartShapes result;
        result.native.reserve(manifest.nodes.size());
        result.requestedBytes.reserve(manifest.nodes.size());
        result.recipes.reserve(manifest.nodes.size());
        result.digests.reserve(manifest.nodes.size());
        for (const auto &recipe : manifest.nodes) {
            if (cancellation.IsCancellationRequested())
                return Outcome::Failure(MakeError(PhysicsErrors::StateCancelled));
            if (auto appended = Append(result, *pool, manifest, recipe); appended.HasError())
                return Outcome::Failure(appended.ErrorValue());
        }
        return Outcome::Success(std::move(result));
    }

    /** @copydoc CanonicalRestartShapes::HoroPayloadBytes */
    std::size_t CanonicalRestartShapes::HoroPayloadBytes() const noexcept {
        return sizeof(*this) + native.capacity() * sizeof(native.front()) + requestedBytes.capacity() * sizeof(requestedBytes.front()) +
               recipes.capacity() * sizeof(recipes.front()) + digests.capacity() * sizeof(digests.front());
    }
}  // namespace Horo::Physics::Detail
