#include "CanonicalPhysicsStateRestartBindings.h"

#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        bool IsQueryRoot(const PhysicsStateConstructionManifest &manifest, PhysicsStateObjectId shape) noexcept {
            return std::ranges::any_of(manifest.fixtures, [shape](const auto &fixture) {
                return fixture.shape == shape;
            });
        }

        std::vector<PhysicsSceneShapeInstance> ChildPoses(const PhysicsStateShapeRecipes &graph, std::uint32_t index) {
            std::vector<PhysicsSceneShapeInstance> result;
            const auto *group = std::get_if<PhysicsStateCompoundRecipe>(&graph.nodes[index].geometry);
            if (!group)
                return result;
            result.reserve(group->count);
            for (const auto &edge : graph.edges.subspan(group->begin, group->count))
                result.push_back({ShapeHandle{}, edge.localPose});
            // Original process-local child handles are deliberately not reconstructed. Native refs and full recipes
            // own the real DAG; this existing metadata preserves exact ordered collider-pose validation after restart.
            return result;
        }

        Result<void> Admit(const CanonicalWorld &world, PhysicsWorldId owner, const PhysicsStateConstructionManifest &manifest,
                           const CanonicalRestartShapes &prepared) {
            if (!owner.IsValid() || !world.scene.shapes.empty() || !world.scene.bodies.empty() || !world.query.fixtures.empty())
                return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
            if (prepared.native.size() != manifest.shapes.nodes.size() || prepared.recipes.size() != prepared.native.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            std::size_t sceneCount{};
            for (const auto &root : manifest.shapes.roots)
                sceneCount += !IsQueryRoot(manifest, root.shape);
            if (sceneCount > world.scene.maximumShapes || sceneCount > std::numeric_limits<std::uint32_t>::max() - world.nextResourceSlot)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc RegisterCanonicalRestartSceneShapes */
    Result<void> RegisterCanonicalRestartSceneShapes(CanonicalWorld &world, PhysicsWorldId owner,
                                                     const PhysicsStateConstructionManifest &manifest,
                                                     const CanonicalRestartShapes &prepared, CanonicalRestartBindings &bindings) {
        if (auto admitted = Admit(world, owner, manifest, prepared); admitted.HasError())
            return admitted;
        bindings.shapes.reserve(manifest.shapes.roots.size());
        for (const auto &root : manifest.shapes.roots) {
            bindings.shapes.push_back({root.shape, {}});
            if (IsQueryRoot(manifest, root.shape))
                continue;
            const auto index = root.recipe;
            const ShapeHandle handle{owner, {world.nextResourceSlot++, 1}};
            const auto *analytic = std::get_if<PhysicsShapeDescriptor>(&manifest.shapes.nodes[index].geometry);
            world.scene.shapes.push_back({.handle = handle,
                                          .shape = prepared.native[index],
                                          .nativeRequestedBytes = prepared.requestedBytes[index],
                                          .construction = prepared.recipes[index],
                                          .recipe = prepared.digests[index],
                                          .analytic = analytic ? std::optional{*analytic} : std::nullopt,
                                          .compoundChildren = ChildPoses(manifest.shapes, index)});
            bindings.shapes.back().shape = handle;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
