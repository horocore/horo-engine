#include "PhysicsStateConstructionManifestInternal.h"

#include <tuple>

namespace Horo::Physics::StateDetail {
    namespace {
        auto Key(const PhysicsStatePersistentOwner &v) noexcept {
            return std::tie(v.entity, v.generation, v.role, v.slot);
        }

        bool ScalarValid(const PhysicsStatePersistentOwner &v) noexcept {
            const auto role = static_cast<std::uint8_t>(v.role);
            return v.generation && v.slot && role >= 1 && role <= 4 && std::ranges::any_of(v.entity, [](auto byte) {
                return byte != 0;
            });
        }

        bool CompleteCount(const PhysicsStateConstructionManifest &m) noexcept {
            auto remaining = m.owners.size();
            for (const auto count : {m.bodies.size(), m.shapes.roots.size(), m.joints.size(), m.fixtures.size()}) {
                if (count > remaining)
                    return false;
                remaining -= count;
            }
            return remaining == 0;
        }

        bool BodyExists(const PhysicsStateConstructionManifest &m, PhysicsStateObjectId id) noexcept {
            if (HasSceneBody(m, id))
                return true;
            const auto found = std::ranges::lower_bound(m.fixtures, id, {}, &PhysicsStateConstructionFixture::body);
            return found != m.fixtures.end() && found->body == id;
        }

        bool JointExists(const PhysicsStateConstructionManifest &m, PhysicsStateObjectId id) noexcept {
            const auto found = std::ranges::lower_bound(m.joints, id, {}, &PhysicsStateConstructionJoint::id);
            return found != m.joints.end() && found->id == id;
        }

        bool CompoundOwner(const PhysicsStateConstructionManifest &m, const PhysicsStatePersistentOwner &v) noexcept {
            auto bodyKey = v;
            bodyKey.role = PhysicsStateOwnerRole::Body;
            const auto owner = std::ranges::lower_bound(m.owners, Key(bodyKey), {}, [](const auto &row) {
                return Key(row);
            });
            if (owner == m.owners.end() || Key(*owner) != Key(bodyKey))
                return false;
            const auto body = std::ranges::lower_bound(m.bodies, owner->object, {}, &PhysicsStateConstructionBody::id);
            return body != m.bodies.end() && body->id == owner->object && body->shape == v.object;
        }

        bool ReferencesOwnedObject(const PhysicsStateConstructionManifest &m, const PhysicsStatePersistentOwner &v) noexcept {
            switch (v.role) {
                using enum Horo::Physics::PhysicsStateOwnerRole;
                case Body:
                    return BodyExists(m, v.object);
                case Collider:
                    return HasShape(m, v.object);
                case Constraint:
                    return JointExists(m, v.object);
                case CompoundBodyRoot:
                    return HasShape(m, v.object) && CompoundOwner(m, v);
            }
            return false;
        }
    }  // namespace

    /** @copydoc ValidateConstructionOwners */
    Result<void> ValidateConstructionOwners(const PhysicsStateConstructionManifest &m) {
        if (m.owners.empty())
            return Result<void>::Success();  // Generic caller-bound non-Scene profiles remain explicit.
        if (!CompleteCount(m))
            return MalformedConstruction();
        for (std::size_t i = 0; i < m.owners.size(); ++i) {
            const auto &owner = m.owners[i];
            if (!ScalarValid(owner) || owner.object != static_cast<PhysicsStateObjectId>(i) + 1)
                return MalformedConstruction();
            if (i && !(Key(m.owners[i - 1]) < Key(owner)))
                return MalformedConstruction();
        }
        for (const auto &owner : m.owners)
            if (!ReferencesOwnedObject(m, owner))
                return MalformedConstruction();
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::StateDetail
