#pragma once
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsStateConstructionManifest.h"
#include "PhysicsStateValidationInternal.h"

#include <algorithm>

namespace Horo::Physics::StateDetail {
    template <class T, class Member> bool Ordered(std::span<const T> rows, Member member) noexcept {
        PhysicsStateObjectId previous{};
        for (const auto &row : rows) {
            const auto value = row.*member;
            if (value <= previous)
                return false;
            previous = value;
        }
        return true;
    }

    inline bool HasShape(const PhysicsStateConstructionManifest &m, PhysicsStateObjectId id) noexcept {
        auto found = std::ranges::lower_bound(m.shapes.roots, id, {}, &PhysicsStateShapeRecipeRoot::shape);
        return found != m.shapes.roots.end() && found->shape == id;
    }

    inline bool HasSceneBody(const PhysicsStateConstructionManifest &m, PhysicsStateObjectId id) noexcept {
        auto found = std::ranges::lower_bound(m.bodies, id, {}, &PhysicsStateConstructionBody::id);
        return found != m.bodies.end() && found->id == id;
    }

    inline Result<void> MalformedConstruction() {
        return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
    }

    [[nodiscard]] Result<void> ValidateConstructionBodies(const PhysicsStateConstructionManifest &, PhysicsStateFpControl);
    [[nodiscard]] Result<void> ValidateConstructionJoints(const PhysicsStateConstructionManifest &, PhysicsStateFpControl);
    [[nodiscard]] Result<void> ValidateConstructionFixtures(const PhysicsStateConstructionManifest &, PhysicsStateFpControl);
    [[nodiscard]] bool ConstructionMaterialValid(const PhysicsQueryMaterial &) noexcept;
    [[nodiscard]] bool ConstructionSurfaceValid(const PhysicsStateSurfaceEvidence &) noexcept;
    [[nodiscard]] Result<void> ValidateConstructionOwners(const PhysicsStateConstructionManifest &);
    [[nodiscard]] Result<void> ValidateConstructionMotion(const PhysicsStateConstructionManifest &, std::span<std::uint32_t>);
}  // namespace Horo::Physics::StateDetail
