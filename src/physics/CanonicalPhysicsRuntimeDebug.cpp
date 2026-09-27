#include "CanonicalPhysicsRuntimeInternal.h"

#include <algorithm>
#include <cstddef>

namespace Horo::Physics::Detail {
    /** @copydoc ProjectCanonicalDebug */
    CanonicalDebugProjection ProjectCanonicalDebug(const CanonicalWorldHandle world, const PhysicsDebugBudget &budget) {
        CanonicalDebugProjection projected;
        if (world.value == nullptr)
            return projected;
        const auto &canonical = *static_cast<const CanonicalWorld *>(world.value);
        const auto capacity = [&budget](const PhysicsDebugCategory category) {
            const auto &limit = budget.categories[static_cast<std::size_t>(category)];
            return std::min<std::size_t>({limit.maximumRecords, limit.maximumPayloadBytes / sizeof(PhysicsDebugRecord),
                                          budget.maximumPayloadBytes / sizeof(PhysicsDebugRecord), MaximumPhysicsDebugRecords});
        };
        const std::size_t bodyLimit = capacity(PhysicsDebugCategory::Body);
        projected.bodies.reserve(std::min(bodyLimit, canonical.scene.bodies.size() + canonical.query.fixtures.size()));
        for (std::size_t index = 0; index < canonical.scene.bodies.size() && projected.bodies.size() < bodyLimit; ++index)
            projected.bodies.emplace_back(PhysicsDebugBody{canonical.scene.bodies[index].handle});
        for (std::size_t index = 0; index < canonical.query.fixtures.size() && projected.bodies.size() < bodyLimit; ++index)
            projected.bodies.emplace_back(PhysicsDebugBody{canonical.query.fixtures[index].fixture.body});
        projected.truncatedBodies = canonical.scene.bodies.size() + canonical.query.fixtures.size() - projected.bodies.size();

        const std::size_t shapeLimit = capacity(PhysicsDebugCategory::Shape);
        projected.shapes.reserve(std::min(shapeLimit, canonical.scene.shapes.size() + canonical.query.fixtures.size()));
        for (std::size_t index = 0; index < canonical.scene.shapes.size() && projected.shapes.size() < shapeLimit; ++index)
            projected.shapes.emplace_back(PhysicsDebugShape{canonical.scene.shapes[index].handle, {}});
        for (std::size_t index = 0; index < canonical.query.fixtures.size() && projected.shapes.size() < shapeLimit; ++index)
            projected.shapes.emplace_back(
                PhysicsDebugShape{canonical.query.fixtures[index].fixture.shape, canonical.query.fixtures[index].fixture.body});
        projected.truncatedShapes = canonical.scene.shapes.size() + canonical.query.fixtures.size() - projected.shapes.size();

        const std::size_t constraintLimit = capacity(PhysicsDebugCategory::Constraint);
        projected.constraints.reserve(std::min(constraintLimit, canonical.scene.constraints.size()));
        for (std::size_t index = 0; index < canonical.scene.constraints.size() && projected.constraints.size() < constraintLimit; ++index) {
            const auto &record = canonical.scene.constraints[index];
            projected.constraints.emplace_back(PhysicsDebugConstraint{record.handle, record.first, record.second});
        }
        projected.truncatedConstraints = canonical.scene.constraints.size() - projected.constraints.size();
        return projected;
    }
}  // namespace Horo::Physics::Detail
