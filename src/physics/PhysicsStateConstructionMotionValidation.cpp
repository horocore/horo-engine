#include "PhysicsStateConstructionManifestInternal.h"

namespace Horo::Physics::StateDetail {
    namespace {
        bool Movable(const PhysicsStateShapeRecipes &graph, const PhysicsStateShapeRecipe &recipe,
                     std::span<const std::uint32_t> previous) noexcept {
            if (const auto *analytic = std::get_if<PhysicsShapeDescriptor>(&recipe.geometry))
                return !std::holds_alternative<PhysicsStaticPlaneShape>(*analytic);
            const auto group = std::get<PhysicsStateCompoundRecipe>(recipe.geometry);
            for (const auto &edge : graph.edges.subspan(group.begin, group.count))
                if (!previous[edge.child])
                    return false;
            return true;
        }

        Result<void> ColliderPoses(const PhysicsStateConstructionManifest &manifest, const PhysicsStateConstructionBody &body,
                                   const PhysicsStateShapeRecipe &shape) {
            if (!body.colliderCount)
                return Result<void>::Success();
            if (body.colliderBegin > manifest.colliders.size() || body.colliderCount > manifest.colliders.size() - body.colliderBegin)
                return MalformedConstruction();
            const auto *group = std::get_if<PhysicsStateCompoundRecipe>(&shape.geometry);
            if (!group)
                return body.colliderCount == 1 ? Result<void>::Success() : MalformedConstruction();
            if (body.colliderCount != group->count)
                return MalformedConstruction();
            for (std::size_t i = 0; i < group->count; ++i)
                if (manifest.colliders[body.colliderBegin + i].localPose != manifest.shapes.edges[group->begin + i].localPose)
                    return MalformedConstruction();
            return Result<void>::Success();
        }

        Result<void> Body(const PhysicsStateConstructionManifest &manifest, const PhysicsStateConstructionBody &body,
                          std::span<const std::uint32_t> movable) {
            const auto root = std::ranges::lower_bound(manifest.shapes.roots, body.shape, {}, &PhysicsStateShapeRecipeRoot::shape);
            if (root == manifest.shapes.roots.end() || root->shape != body.shape)
                return MalformedConstruction();
            const bool mayMove = movable[root->recipe] != 0;
            if (!mayMove && body.policy.motion != PhysicsMotionType::Static)
                return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            // Canonical creation always reserves motion storage for movable shapes, including initially static bodies.
            if (body.motionStorageReserved != mayMove)
                return MalformedConstruction();
            return ColliderPoses(manifest, body, manifest.shapes.nodes[root->recipe]);
        }
    }  // namespace

    /** @brief Same pinned Shape::MustBeStatic/CompoundShape recursive rule, evaluated on already checked topological rows.
     * Reuses caller-owned node scratch after depth validation; never invokes a native asserting getter/constructor.
     */
    Result<void> ValidateConstructionMotion(const PhysicsStateConstructionManifest &manifest, std::span<std::uint32_t> scratch) {
        for (std::size_t index = 0; index < manifest.shapes.nodes.size(); ++index)
            scratch[index] = Movable(manifest.shapes, manifest.shapes.nodes[index], scratch.first(index)) ? 1 : 0;
        for (const auto &body : manifest.bodies)
            if (auto valid = Body(manifest, body, scratch); valid.HasError())
                return valid;
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::StateDetail
