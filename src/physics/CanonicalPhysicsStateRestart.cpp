#include "CanonicalPhysicsStateRestart.h"

#include "CanonicalPhysicsStateFpControl.h"
#include "CanonicalPhysicsStateRestartBindings.h"

#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        Result<void> Routes(const PhysicsStateConstructionManifest &manifest, std::span<const CanonicalRestartBodyRoute> routes) {
            if (routes.size() != manifest.bodies.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            for (std::size_t i = 0; i < routes.size(); ++i)
                if (routes[i].body != manifest.bodies[i].id)
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            return Result<void>::Success();
        }

        Result<void> Capacity(const CanonicalWorld &world, const PhysicsStateConstructionManifest &manifest) {
            const auto maximumBodies = static_cast<std::size_t>(world.native.system->GetMaxBodies());
            if (manifest.bodies.size() > maximumBodies || manifest.fixtures.size() > maximumBodies - manifest.bodies.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            const auto sceneShapes = manifest.shapes.roots.size() - manifest.fixtures.size();  // Unique roles already fully validated.
            if (sceneShapes > world.scene.maximumShapes || manifest.fixtures.size() > world.query.maximumFixtures ||
                manifest.joints.size() > world.scene.maximumConstraints)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            auto slots = std::size_t{std::numeric_limits<std::uint32_t>::max() - world.nextResourceSlot};
            for (const auto count : {sceneShapes, manifest.bodies.size(), manifest.fixtures.size()}) {
                if (count > slots)
                    return Result<void>::Failure(MakeError(PhysicsErrors::GenerationExhausted));
                slots -= count;
            }
            return Result<void>::Success();
        }

        Result<void> Generations(const CanonicalWorld &world, const PhysicsStateConstructionManifest &manifest) {
            if (manifest.joints.size() > std::numeric_limits<std::uint32_t>::max() - world.scene.nextConstraintSlot ||
                manifest.fixtures.size() > std::numeric_limits<std::uint32_t>::max() - world.query.nextFixtureGeneration ||
                manifest.fixtures.size() > std::numeric_limits<std::uint64_t>::max() - world.query.querySchemaGeneration)
                return Result<void>::Failure(MakeError(PhysicsErrors::GenerationExhausted));
            return Result<void>::Success();
        }

        Result<std::size_t> ScratchWords(const CanonicalWorld &world, const PhysicsStateConstructionManifest &manifest,
                                         PhysicsStateFpControl current) {
            using Outcome = Result<std::size_t>;
            const auto &pool = world.owner.constructionRecipes;
            if (!pool || pool->Tracking() != PhysicsStateConstructionTracking::Required || pool->Profile() != current)
                return Outcome::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
            const auto &limits = pool->Limits();
            const auto &shapes = manifest.shapes;
            if (shapes.nodes.size() > limits.maximumNodes || shapes.edges.size() > limits.maximumEdges ||
                shapes.roots.size() > limits.maximumRoots)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            if (shapes.nodes.size() > std::numeric_limits<std::size_t>::max() - shapes.roots.size())
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Outcome::Success(shapes.nodes.size() + shapes.roots.size());
        }

        Result<void> Validate(const CanonicalWorld &world, const PhysicsStateConstructionManifest &manifest,
                              const PhysicsStateConstructionManifestLimits &limits, std::span<const CanonicalRestartBodyRoute> routes,
                              const PhysicsContinuousCollisionPolicy &ccd) {
            auto current = ReadCanonicalPhysicsStateFpControl();
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            auto words = ScratchWords(world, manifest, current.Value());
            if (words.HasError())
                return Result<void>::Failure(words.ErrorValue());
            std::vector<std::uint32_t> scratch(words.Value());
            if (auto complete = ValidatePhysicsStateConstructionManifest(manifest, limits, current.Value(), scratch); complete.HasError())
                return complete;
            if (auto policy = ValidatePhysicsContinuousCollisionPolicy(ccd); policy.HasError())
                return policy;
            if (auto routesValid = Routes(manifest, routes); routesValid.HasError())
                return routesValid;
            if (auto capacity = Capacity(world, manifest); capacity.HasError())
                return capacity;
            return Generations(world, manifest);
        }

        void ReceivingCounters(const CanonicalWorld &previous, CanonicalWorld &next) noexcept {
            next.nextResourceSlot = previous.nextResourceSlot;
            next.scene.nextConstraintSlot = previous.scene.nextConstraintSlot;
            next.query.nextFixtureGeneration = previous.query.nextFixtureGeneration;
            next.query.querySchemaGeneration = previous.query.querySchemaGeneration;
            next.continuousCollision = previous.continuousCollision;
            next.native.system->SetPhysicsSettings(previous.native.system->GetPhysicsSettings());
        }

        Result<void> Populate(CanonicalWorldHandle handle, PhysicsWorldId owner, const PhysicsStateConstructionManifest &manifest,
                              std::span<const CanonicalRestartBodyRoute> routes, CanonicalRestartBindings &bindings,
                              const CancellationToken &cancellation) {
            auto &world = *static_cast<CanonicalWorld *>(handle.value);
            if (!bindings.shapes.empty() || !bindings.bodies.empty() || !bindings.constraints.empty())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            auto shapes = PrepareCanonicalRestartShapes(world.owner, manifest.shapes, cancellation);
            if (shapes.HasError())
                return Result<void>::Failure(shapes.ErrorValue());
            if (auto roots = RegisterCanonicalRestartSceneShapes(world, owner, manifest, shapes.Value(), bindings); roots.HasError())
                return roots;
            if (auto bodies = CreateCanonicalRestartBodies(handle, owner, manifest, routes, bindings, cancellation); bodies.HasError())
                return bodies;
            if (auto fixtures = CreateCanonicalRestartFixtures(handle, owner, manifest, shapes.Value(), bindings, cancellation);
                fixtures.HasError())
                return fixtures;
            return CreateCanonicalRestartJoints(handle, owner, manifest, bindings, cancellation);
        }
    }  // namespace

    /** @copydoc ReconstructCanonicalPhysicsStateTopology */
    Result<CanonicalWorldHandle> ReconstructCanonicalPhysicsStateTopology(
        CanonicalWorldHandle receiving, PhysicsWorldId owner, const PhysicsWorldSettings &settings,
        const PhysicsStateConstructionManifest &manifest, const PhysicsStateConstructionManifestLimits &limits,
        std::span<const CanonicalRestartBodyRoute> routes, const PhysicsContinuousCollisionPolicy &ccd,
        const CancellationToken &cancellation, CanonicalRestartBindings &bindings) {
        using Outcome = Result<CanonicalWorldHandle>;
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(PhysicsErrors::StateCancelled));
        auto *previous = static_cast<CanonicalWorld *>(receiving.value);
        if (!previous || !owner.IsValid() || !previous->native.system || previous->settingsIdentity != settings.Identity())
            return Outcome::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (auto valid = Validate(*previous, manifest, limits, routes, ccd); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        auto created = CreateCanonicalWorld({&previous->owner}, settings, CanonicalFailurePoint::None, previous->simulation.binding);
        if (created.HasError())
            return created;
        std::unique_ptr<CanonicalWorld> candidate{static_cast<CanonicalWorld *>(created.Value().value)};
        ReceivingCounters(*previous, *candidate);
        if (auto applied = ApplyCanonicalContinuousCollision({candidate.get()}, ccd); applied.HasError())
            return Outcome::Failure(applied.ErrorValue());
        if (auto populated = Populate({candidate.get()}, owner, manifest, routes, bindings, cancellation); populated.HasError())
            return Outcome::Failure(populated.ErrorValue());
        return Outcome::Success({candidate.release()});
    }
}  // namespace Horo::Physics::Detail
