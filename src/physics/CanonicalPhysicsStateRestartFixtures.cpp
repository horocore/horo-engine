#include "CanonicalPhysicsStateRestartBindings.h"
#include "PhysicsStateSurfaceBinding.h"

#include <type_traits>

namespace Horo::Physics::Detail {
    namespace {
        PhysicsQueryFixtureShape Analytic(const PhysicsShapeDescriptor &descriptor) {
            return std::visit([](const auto &shape) -> PhysicsQueryFixtureShape {
                return shape;
            }, descriptor);
        }

        Result<PhysicsQueryFixtureShape> Geometry(const PhysicsStateConstructionManifest &manifest,
                                                  const PhysicsStateConstructionFixture &fixture, std::uint32_t node,
                                                  const PhysicsSurfaceWorldBinding &surfaces) {
            using Outcome = Result<PhysicsQueryFixtureShape>;
            const auto &recipe = manifest.shapes.nodes[node];
            if (const auto *analytic = std::get_if<PhysicsShapeDescriptor>(&recipe.geometry))
                return Outcome::Success(Analytic(*analytic));
            const auto group = std::get<PhysicsStateCompoundRecipe>(recipe.geometry);
            PhysicsCompoundShapeDescriptor compound;
            compound.children.reserve(group.count);
            for (std::size_t i = 0; i < group.count; ++i) {
                const auto &edge = manifest.shapes.edges[group.begin + i];
                const auto &child = manifest.queryChildren[fixture.childBegin + i];
                auto surface = StateDetail::RebindStateSurface(surfaces, child.surface);
                if (surface.HasError())
                    return Outcome::Failure(surface.ErrorValue());
                compound.children.push_back({.geometry = std::get<PhysicsShapeDescriptor>(manifest.shapes.nodes[edge.child].geometry),
                                             .localPose = edge.localPose,
                                             .subshape = child.subshape,
                                             .material = child.material,
                                             .layer = child.layer,
                                             .profile = child.profile,
                                             .channel = child.channel,
                                             .response = child.response,
                                             .trigger = child.trigger,
                                             .surface = surface.Value()});
            }
            return Outcome::Success(std::move(compound));
        }

        Result<void> Create(CanonicalWorldHandle handle, PhysicsWorldId owner, const PhysicsStateConstructionManifest &manifest,
                            const PhysicsStateConstructionFixture &source, const CanonicalRestartShapes &prepared,
                            CanonicalRestartBindings &bindings) {
            const auto root = std::ranges::lower_bound(manifest.shapes.roots, source.shape, {}, &PhysicsStateShapeRecipeRoot::shape);
            auto mapped = std::ranges::lower_bound(bindings.shapes, source.shape, {}, &PhysicsStateShapeBinding::id);
            if (root == manifest.shapes.roots.end() || root->shape != source.shape || mapped == bindings.shapes.end() ||
                mapped->id != source.shape || mapped->shape.IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            const auto &world = *static_cast<const CanonicalWorld *>(handle.value);
            auto geometry = Geometry(manifest, source, root->recipe, world.simulation.binding.surfaces);
            if (geometry.HasError())
                return Result<void>::Failure(geometry.ErrorValue());
            auto surface = StateDetail::RebindStateSurface(world.simulation.binding.surfaces, source.surface);
            if (surface.HasError())
                return Result<void>::Failure(surface.ErrorValue());
            PhysicsQueryFixtureDescriptor descriptor{.shape = std::move(geometry).Value(),
                                                     .pose = source.originalPose,
                                                     .layer = source.layer,
                                                     .profile = source.profile,
                                                     .channel = source.channel,
                                                     .response = source.response,
                                                     .trigger = source.trigger,
                                                     .subshape = source.subshape,
                                                     .material = source.material,
                                                     .surface = surface.Value()};
            auto created = CreateCanonicalQueryFixture(handle, owner, descriptor, &prepared.recipes[root->recipe]);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            mapped->shape = created.Value().shape;
            bindings.bodies.emplace_back(source.body, created.Value().body);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CreateCanonicalRestartFixtures */
    Result<void> CreateCanonicalRestartFixtures(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                const PhysicsStateConstructionManifest &manifest, const CanonicalRestartShapes &prepared,
                                                CanonicalRestartBindings &bindings, const CancellationToken &cancellation) {
        if (!world.value || bindings.bodies.size() != manifest.bodies.size())
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        bindings.bodies.reserve(manifest.bodies.size() + manifest.fixtures.size());
        for (const auto &fixture : manifest.fixtures) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PhysicsErrors::StateCancelled));
            if (auto created = Create(world, owner, manifest, fixture, prepared, bindings); created.HasError())
                return created;
        }
        std::ranges::sort(bindings.bodies, {}, &PhysicsStateBodyBinding::id);
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
