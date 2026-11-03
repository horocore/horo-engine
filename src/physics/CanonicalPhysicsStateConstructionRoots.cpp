#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateConstructionExport.h"

namespace Horo::Physics::Detail {
    namespace {
        Result<std::size_t> SceneRoots(const CanonicalWorld &world, const CanonicalPhysicsStateTopologyMap &map,
                                       std::span<StateDetail::ConstructionRecipeRootBinding> roots) {
            if (map.shapeRecords.size() > roots.size())
                return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            for (std::size_t i = 0; i < map.shapeRecords.size(); ++i) {
                const auto &mapping = map.shapeRecords[i];
                if (mapping.index >= world.scene.shapes.size())
                    return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapabilityStale));
                const auto &record = world.scene.shapes[mapping.index];
                if (record.handle != mapping.handle || !record.construction.IsValid())
                    return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
                roots[i] = {mapping.id, record.construction};
            }
            return Result<std::size_t>::Success(map.shapeRecords.size());
        }
    }  // namespace

    /** @brief Exports actual retained root leases, never recapturing recipes from descriptor labels/current geometry. */
    Result<PhysicsStateShapeRecipes> ExportCanonicalConstructionRoots(const CanonicalWorld &world,
                                                                      const CanonicalPhysicsStateTopologyMap &map,
                                                                      StateDetail::ConstructionManifestOwner &owner) {
        using Outcome = Result<PhysicsStateShapeRecipes>;
        auto *pool = world.owner.constructionRecipes;
        if (!pool)
            return Outcome::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        auto roots = owner.RootBindings();
        auto scene = SceneRoots(world, map, roots);
        if (scene.HasError())
            return Outcome::Failure(scene.ErrorValue());
        auto count = scene.Value();
        if (world.query.fixtures.size() > roots.size() - count)
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        for (const auto &fixture : world.query.fixtures) {
            if (!fixture.construction.IsValid())
                return Outcome::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
            roots[count++] = {FindStateShapeId(map, fixture.fixture.shape), fixture.construction};
        }
        roots = roots.first(count);
        std::ranges::sort(roots, {}, &StateDetail::ConstructionRecipeRootBinding::id);
        return pool->Export(roots, owner.ExportStorage());
    }
}  // namespace Horo::Physics::Detail
