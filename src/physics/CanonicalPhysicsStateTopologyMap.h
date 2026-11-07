#pragma once

#include "CanonicalPhysicsStateBindingInternal.h"

namespace Horo::Physics::Detail {
    /** @brief Precompiled exact record index; no native address or physical index is emitted as durable identity. */
    struct CanonicalPhysicsStateBodyRecordIndex final {
        BodyHandle handle;
        std::size_t index{};
        bool fixture{};
    };

    struct CanonicalPhysicsStateShapeRecordIndex final {
        PhysicsStateObjectId id{};
        ShapeHandle handle;
        std::size_t index{};
    };

    /** @brief Each owner of the retained native pair has its own immutable index map because joint record order may differ. */
    struct CanonicalPhysicsStateTopologyMap final {
        std::vector<CanonicalPhysicsStateBodyRecordIndex> bodies; /**< Same strict stable-ID order as binding.stableBodies. */
        std::vector<std::size_t> joints;                          /**< Same strict stable-ID order as binding.stableJoints. */
        std::vector<PhysicsStateShapeBinding> shapesByHandle;
        std::vector<CanonicalPhysicsStateShapeRecordIndex> shapeRecords;
        std::size_t sceneBodies{}, queryBodies{}, sceneShapes{}, sceneJoints{};

        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;
    };

    /** @brief Cold complete lookup compilation; duplicate/missing records fail before any publication. */
    [[nodiscard]] Result<CanonicalPhysicsStateTopologyMap> PrepareCanonicalPhysicsStateTopologyMap(
        const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings, std::span<const PhysicsStateShapeBinding> shapes);
    /** @brief Allocation-free exact shape key resolution from the cold compiled handle-sorted map. */
    [[nodiscard]] PhysicsStateObjectId FindStateShapeId(const CanonicalPhysicsStateTopologyMap &map, ShapeHandle shape) noexcept;
}  // namespace Horo::Physics::Detail
