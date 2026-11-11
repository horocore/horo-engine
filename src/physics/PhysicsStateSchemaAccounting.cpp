#include "PhysicsStateSchemaAccounting.h"

#include <cstdint>
#include <string>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Checks products and aggregate reservations before adding any actual
         * retained capacity. */
        bool Add(std::size_t count, std::size_t size, std::size_t &total) noexcept {
            if (count > (std::numeric_limits<std::size_t>::max() - total) / size)
                return false;
            total += count * size;
            return true;
        }

        /** @brief Inline character storage is already in string sizeof; external
         * capacity includes its trailing null byte. */
        bool String(const std::string &value, std::size_t &total) noexcept {
            const auto object = reinterpret_cast<std::uintptr_t>(&value);
            if (const auto data = reinterpret_cast<std::uintptr_t>(value.data()); data >= object && data - object < sizeof(value))
                return true;
            if (!Add(value.capacity(), sizeof(char), total))
                return false;
            return Add(1, sizeof(char), total);
        }
    }  // namespace

    /** @copydoc PhysicsStateSchemaAccounting::Collision */
    Result<std::size_t> PhysicsStateSchemaAccounting::Collision(const NormalizedCollisionSchema *schema) {
        if (!schema)
            return Result<std::size_t>::Success(0);
        std::size_t bytes = sizeof(*schema);
        if (!Add(schema->layers_.capacity(), sizeof(CollisionLayerDefinition), bytes) ||
            !Add(schema->pairs_.capacity(), sizeof(CollisionPairDefinition), bytes) ||
            !Add(schema->channels_.capacity(), sizeof(CollisionQueryChannelDefinition), bytes) ||
            !Add(schema->profiles_.capacity(), sizeof(CollisionProfileDefinition), bytes))
            return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        for (const auto &profile : schema->profiles_)
            if (!Add(profile.queryResponses.capacity(), sizeof(CollisionProfileQueryResponse), bytes))
                return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        return Result<std::size_t>::Success(bytes);
    }

    /** @copydoc PhysicsStateSchemaAccounting::Surfaces */
    Result<std::size_t> PhysicsStateSchemaAccounting::Surfaces(const SurfaceCatalogSnapshot *catalog) {
        if (!catalog)
            return Result<std::size_t>::Success(0);
        std::size_t bytes = sizeof(*catalog);
        if (!Add(catalog->value_.surfaces.capacity(), sizeof(SurfaceCatalogEntry), bytes) || !String(catalog->value_.projectId, bytes))
            return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        for (const auto &surface : catalog->value_.surfaces)
            if (!String(surface.displayName, bytes))
                return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        return Result<std::size_t>::Success(bytes);
    }
}  // namespace Horo::Physics::Detail
