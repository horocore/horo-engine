#include "CanonicalPhysicsStateBackendInternal.h"
#include "CanonicalPhysicsStateGeometry.h"

#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Cold stable-ID sort has a complete tie rejection; physical handles
         * never become durable identity. */
        bool ValidStableOrder(std::span<const PhysicsStateShapeBinding> values) noexcept {
            PhysicsStateObjectId previous{};
            for (const auto &value : values) {
                if (value.id == 0 || value.id <= previous || !value.shape.IsValid())
                    return false;
                previous = value.id;
            }
            return true;
        }

        /** @brief Both inventories are handle-sorted for exact complete coverage and
         * duplicate rejection. */
        bool SameGeometry(std::span<const CanonicalPhysicsStateGeometry> geometry,
                          std::span<const PhysicsStateShapeBinding> bindings) noexcept {
            if (geometry.size() != bindings.size())
                return false;
            for (std::size_t index = 0; index < geometry.size(); ++index)
                if (geometry[index].handle != bindings[index].shape)
                    return false;
            return true;
        }
    }  // namespace

    /** @copydoc CanonicalPhysicsStateShapeBindings */
    Result<std::vector<PhysicsStateShapeBinding>> CanonicalPhysicsStateShapeBindings(const CanonicalWorld &world,
                                                                                     std::span<const PhysicsStateShapeBinding> bindings) {
        using Outcome = Result<std::vector<PhysicsStateShapeBinding>>;
        // World capacities already bound the actual inventory. No caller count may
        // trigger an unbounded extra allocation.
        if (const auto maximum = world.scene.shapes.size() + world.scene.bodies.size() + world.query.fixtures.size();
            bindings.size() > maximum)
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        auto geometry = CollectCanonicalPhysicsStateGeometry(world, std::numeric_limits<std::size_t>::max());
        if (geometry.HasError())
            return Outcome::Failure(geometry.ErrorValue());
        std::vector<PhysicsStateShapeBinding> copied{bindings.begin(), bindings.end()};
        std::ranges::sort(copied, {}, &PhysicsStateShapeBinding::shape);
        if (!SameGeometry(geometry.Value(), copied))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        std::ranges::sort(copied, {}, &PhysicsStateShapeBinding::id);
        if (!ValidStableOrder(copied))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        return Outcome::Success(std::move(copied));
    }
}  // namespace Horo::Physics::Detail
