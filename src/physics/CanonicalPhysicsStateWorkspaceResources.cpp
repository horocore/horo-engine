#include "CanonicalPhysicsStateBackendInternal.h"
#include "CanonicalPhysicsStateGeometry.h"
#include "PhysicsStateSchemaAccounting.h"

namespace Horo::Physics::Detail {
    namespace {
        bool Charge(std::size_t bytes, std::size_t maximum, std::size_t &total) noexcept {
            if (bytes > maximum - total)
                return false;
            total += bytes;
            return true;
        }

        Result<std::size_t> SharedReservation(const CanonicalWorld &world, std::span<const CanonicalPhysicsStateGeometry> geometry,
                                              std::size_t maximum, std::size_t referenceCapacity) {
            using Outcome = Result<std::size_t>;
            auto native = CanonicalPhysicsStateGeometryReservation(geometry);
            if (native.HasError())
                return native;
            auto schema = PhysicsStateSchemaAccounting::Collision(world.simulation.binding.schema.get());
            if (schema.HasError())
                return schema;
            auto catalog = PhysicsStateSchemaAccounting::Surfaces(world.simulation.binding.surfaces.catalog.get());
            if (catalog.HasError())
                return catalog;
            std::size_t total{};
            if (!Charge(sizeof(CanonicalPhysicsStateResources), maximum, total) ||
                referenceCapacity > (maximum - total) / sizeof(JPH::RefConst<JPH::Shape>))
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            total += referenceCapacity * sizeof(JPH::RefConst<JPH::Shape>);
            if (!Charge(native.Value(), maximum, total) || !Charge(schema.Value(), maximum, total) ||
                !Charge(catalog.Value(), maximum, total))
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Outcome::Success(total);
        }
    }  // namespace

    /** @copydoc CanonicalPhysicsStateWorkspace::Impl::PrepareInventories */
    Result<void> CanonicalPhysicsStateWorkspace::Impl::PrepareInventories(const CanonicalWorld &world,
                                                                          const PhysicsStateCaptureConfiguration &configuration) {
        auto resolved = PrepareCanonicalPhysicsStateBindings(world, configuration.bodies, configuration.constraints);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        auto resolvedShapes = CanonicalPhysicsStateShapeBindings(world, configuration.shapes);
        if (resolvedShapes.HasError())
            return Result<void>::Failure(resolvedShapes.ErrorValue());
        auto geometry = CollectCanonicalPhysicsStateGeometry(world, configuration.maximumRetainedBytes);
        if (geometry.HasError())
            return Result<void>::Failure(geometry.ErrorValue());
        auto retained = std::make_shared<CanonicalPhysicsStateResources>();
        retained->shapes.reserve(geometry.Value().size());
        for (const auto &entry : geometry.Value())
            retained->shapes.push_back(entry.shape);
        auto bytes = SharedReservation(world, geometry.Value(), configuration.maximumRetainedBytes, retained->shapes.capacity());
        if (bytes.HasError())
            return Result<void>::Failure(bytes.ErrorValue());
        retained->retainedBytes = bytes.Value();
        retained->simulation = world.simulation.binding;
        resources = std::move(retained);
        bindings = std::move(resolved).Value();
        shapes = std::move(resolvedShapes).Value();
        bodies.assign(configuration.bodies.begin(), configuration.bodies.end());
        std::ranges::sort(bodies, {}, &PhysicsStateBodyBinding::id);
        bodiesByHandle = bodies;
        std::ranges::sort(bodiesByHandle, {}, &PhysicsStateBodyBinding::body);
        filters = CanonicalPhysicsStateFilterFingerprint(world);
        surfaces = CanonicalPhysicsStateSurfaceFingerprint(world.simulation.binding.surfaces);
        filterEpoch = world.simulation.binding.generation;
        surfaceEpoch = world.simulation.binding.surfaces.mappingGeneration;
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
