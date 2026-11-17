#include "CanonicalPhysicsStateAccounting.h"

#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief All physical Horo allocation products and conservative native
         * reservations use checked accumulation. */
        bool Add(std::size_t count, std::size_t size, std::size_t &total) noexcept {
            if (count > (std::numeric_limits<std::size_t>::max() - total) / size)
                return false;
            total += count * size;
            return true;
        }

        template <class T> bool Vector(const std::vector<T> &values, std::size_t &total) noexcept {
            return Add(values.capacity(), sizeof(T), total);
        }

        /** @brief Child/collider payload belongs to the actual copied metadata, not a
         * guessed maximum or native GetStats. */
        bool SceneMetadata(const CanonicalWorld &world, std::size_t &bytes) noexcept {
            for (const auto &shape : world.scene.shapes)
                if (!Vector(shape.compoundChildren, bytes))
                    return false;
            for (const auto &body : world.scene.bodies) {
                if (!body.collision)
                    continue;
                if (!Vector(body.collision->colliders, bytes))
                    return false;
                for (const auto &collider : body.collision->colliders)
                    if (!Vector(collider.materials, bytes))
                        return false;
            }
            return true;
        }

        /** @brief Query compound descriptors contain one bounded owned child vector;
         * primitive descriptors own no heap payload. */
        bool QueryMetadata(const CanonicalWorld &world, std::size_t &bytes) noexcept {
            for (const auto &fixture : world.query.fixtures)
                if (const auto *compound = std::get_if<PhysicsCompoundShapeDescriptor>(&fixture.descriptor.shape))
                    if (!Vector(compound->children, bytes))
                        return false;
            return true;
        }

        /** @brief All seven top-level owning vectors are counted even when their
         * current element prefixes are empty. */
        bool OwnerVectors(const CanonicalWorld &world, std::size_t &bytes) noexcept {
            return Vector(world.scene.shapes, bytes) && Vector(world.scene.bodies, bytes) && Vector(world.scene.nativeBodyIndices, bytes) &&
                   Vector(world.scene.constraints, bytes) && Vector(world.scene.disabledJointCollisionPairs, bytes) &&
                   Vector(world.query.fixtures, bytes) && Vector(world.query.nativeFixtureIndices, bytes);
        }
    }  // namespace

    /** @copydoc CanonicalPhysicsStateOwnerPayloadBytes */
    Result<std::size_t> CanonicalPhysicsStateOwnerPayloadBytes(const CanonicalWorld &world) {
        std::size_t bytes = sizeof(world);
        if (!OwnerVectors(world, bytes) || !SceneMetadata(world, bytes) || !QueryMetadata(world, bytes))
            return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        return Result<std::size_t>::Success(bytes);
    }

}  // namespace Horo::Physics::Detail
