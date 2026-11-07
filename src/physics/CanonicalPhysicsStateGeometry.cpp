#include "CanonicalPhysicsStateGeometry.h"

#include "CanonicalPhysicsStateShapeRecipe.h"

#include <algorithm>
#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Same handle may be referenced by several bodies, but cannot change
         * physical geometry or its immutable construction witness. */
        bool Same(const CanonicalPhysicsStateGeometry &one, const CanonicalPhysicsStateGeometry &two) noexcept {
            return one.shape.GetPtr() == two.shape.GetPtr() && one.recipe == two.recipe &&
                   one.nativeRequestedBytes == two.nativeRequestedBytes;
        }

        /** @brief Cold consolidation releases only duplicate references; it never drops
         * the one actual retained shape owner. */
        Result<void> Consolidate(std::vector<CanonicalPhysicsStateGeometry> &geometry) {
            std::ranges::sort(geometry, {}, &CanonicalPhysicsStateGeometry::handle);
            std::size_t kept{};
            for (std::size_t index = 0; index < geometry.size(); ++index) {
                const auto &source = geometry[index];
                if (!source.handle.IsValid() || !source.shape || source.nativeRequestedBytes == 0)
                    return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
                if (!HasCanonicalShapeRecipe(source.recipe))
                    return Result<void>::Failure(
                        MakeError(PhysicsErrors::ProfileUnsupported,
                                  "Physics state requires actual immutable geometry construction arithmetic provenance."));
                if (kept != 0 && geometry[kept - 1].handle == source.handle) {
                    if (!Same(geometry[kept - 1], source))
                        return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                    continue;
                }
                if (kept != index)
                    geometry[kept] = std::move(geometry[index]);
                ++kept;
            }
            geometry.resize(kept);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CollectCanonicalPhysicsStateGeometry */
    Result<std::vector<CanonicalPhysicsStateGeometry>> CollectCanonicalPhysicsStateGeometry(const CanonicalWorld &world,
                                                                                            std::size_t maximumPayloadBytes) {
        using Outcome = Result<std::vector<CanonicalPhysicsStateGeometry>>;
        const auto maximumCount = maximumPayloadBytes / sizeof(CanonicalPhysicsStateGeometry);
        std::size_t count = world.scene.shapes.size();
        if (count > maximumCount || world.scene.bodies.size() > maximumCount - count)
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        count += world.scene.bodies.size();
        if (world.query.fixtures.size() > maximumCount - count)
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        count += world.query.fixtures.size();
        std::vector<CanonicalPhysicsStateGeometry> geometry;
        geometry.reserve(count);
        for (const auto &shape : world.scene.shapes)
            geometry.push_back({shape.handle, shape.shape, shape.recipe, shape.nativeRequestedBytes});
        for (const auto &body : world.scene.bodies)
            geometry.push_back({body.policy.shape, body.shapeEvidence, body.shapeRecipe, body.shapeNativeRequestedBytes});
        for (const auto &fixture : world.query.fixtures)
            geometry.push_back({fixture.fixture.shape, fixture.shape, fixture.recipe, fixture.nativeRequestedBytes});
        if (auto consolidated = Consolidate(geometry); consolidated.HasError())
            return Outcome::Failure(consolidated.ErrorValue());
        if (geometry.capacity() > maximumCount)
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        return Outcome::Success(std::move(geometry));
    }

    /** @copydoc CanonicalPhysicsStateGeometryReservation */
    Result<std::size_t> CanonicalPhysicsStateGeometryReservation(std::span<const CanonicalPhysicsStateGeometry> geometry) {
        std::size_t bytes{};
        for (const auto &shape : geometry) {
            if (shape.nativeRequestedBytes > std::numeric_limits<std::size_t>::max() - bytes)
                return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            bytes += shape.nativeRequestedBytes;
        }
        return Result<std::size_t>::Success(bytes);
    }
}  // namespace Horo::Physics::Detail
