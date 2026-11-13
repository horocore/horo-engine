#pragma once

#include "Horo/Physics/PhysicsQuery.h"
#include "Horo/Physics/PhysicsShapeDescriptor.h"
#include "Horo/Physics/PhysicsStateConstructionTypes.h"
#include "PhysicsStateHashInternal.h"

#include <algorithm>
#include <optional>
#include <type_traits>

namespace Horo::Physics::Detail {
    /** @brief Zero is the explicit unknown provenance marker, never a supported native recipe. */
    inline bool HasCanonicalShapeRecipe(const Sha256Digest &value) noexcept {
        return std::ranges::any_of(value.bytes, [](auto byte) {
            return byte != 0;
        });
    }

    /** @brief Binds actual constructor arithmetic controls without serializing native object memory or pointer identity. */
    inline Sha256Digest BindCanonicalShapeCreationRecipe(const Sha256Digest &inputs,
                                                         const std::optional<PhysicsStateFpControl> &control) noexcept {
        if (!control || !HasCanonicalShapeRecipe(inputs))
            return {};
        StateDetail::StateHash hash{"Horo.Physics.ShapeCreation.v2"};
        hash.Digest(inputs);
        hash.Word(static_cast<std::uint8_t>(control->architecture));
        hash.Word(control->value);
        return hash.Finish();
    }

    /** @brief Captured from the actual admitted immutable analytic construction
     * input, independent of native allocation addresses. */
    inline Sha256Digest CanonicalPrimitiveRecipe(const PhysicsShapeDescriptor &descriptor) noexcept {
        StateDetail::StateHash hash{"Horo.Physics.ShapeRecipe.v2"};
        hash.Word(descriptor.index());
        std::visit([&hash](const auto &shape) {
            using Shape = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<Shape, PhysicsBoxShape>)
                hash.Vector(shape.halfExtentsMeters);
            else if constexpr (std::is_same_v<Shape, PhysicsSphereShape>)
                hash.Float(shape.radiusMeters);
            else if constexpr (std::is_same_v<Shape, PhysicsCapsuleShape>) {
                hash.Float(shape.radiusMeters);
                hash.Float(shape.cylindricalHalfHeightMeters);
            } else {
                hash.Vector(shape.normal);
                hash.Float(shape.signedDistanceMeters);
            }
        }, descriptor);
        return hash.Finish();
    }

    /** @brief Compound witness includes child construction witnesses and exact
     * ordinal/pose; child record retirement cannot erase it. */
    inline StateDetail::StateHash BeginCanonicalCompoundRecipe(std::size_t children) noexcept {
        StateDetail::StateHash hash{"Horo.Physics.ShapeRecipe.v2"};
        hash.Word(std::variant_size_v<PhysicsShapeDescriptor>);
        hash.Word(children);
        return hash;
    }

    /** @brief Query fixtures own complete analytic child geometry/poses, so no
     * retired Horo shape record is needed. */
    inline Sha256Digest CanonicalQueryGeometryRecipe(const PhysicsQueryFixtureShape &descriptor,
                                                     const std::optional<PhysicsStateFpControl> &control) noexcept {
        return std::visit([&control](const auto &shape) -> Sha256Digest {
            using Shape = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<Shape, PhysicsCompoundShapeDescriptor>) {
                auto hash = BeginCanonicalCompoundRecipe(shape.children.size());
                for (const auto &child : shape.children) {
                    hash.Digest(BindCanonicalShapeCreationRecipe(CanonicalPrimitiveRecipe(child.geometry), control));
                    hash.Pose(child.localPose);
                }
                return hash.Finish();
            } else
                return CanonicalPrimitiveRecipe(PhysicsShapeDescriptor{shape});
        }, descriptor);
    }
}  // namespace Horo::Physics::Detail
