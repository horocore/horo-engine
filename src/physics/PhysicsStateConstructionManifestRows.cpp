#include "PhysicsStateConstructionManifestCodec.h"

namespace Horo::Physics::StateDetail {
    bool ConstructionManifestRows(Wire &wire, const PhysicsStateConstructionManifest &m) noexcept {
        for (auto body : m.bodies)
            if (!ConstructionBodyFields(wire, body, false))
                return false;
        for (auto collider : m.colliders)
            ConstructionColliderFields(wire, collider);
        for (auto material : m.materials)
            ConstructionMaterialFields(wire, material);
        for (auto joint : m.joints)
            if (!ConstructionJointFields(wire, joint, false))
                return false;
        for (auto fixture : m.fixtures)
            ConstructionFixtureFields(wire, fixture);
        for (auto child : m.queryChildren)
            ConstructionQueryChildFields(wire, child);
        for (auto owner : m.owners)
            ConstructionOwnerFields(wire, owner);
        return !wire.Failed();
    }

    bool ReadConstructionManifestRows(Wire &wire, ConstructionManifestStorage s) noexcept {
        for (auto &body : s.bodies)
            if (!ConstructionBodyFields(wire, body, true))
                return false;
        for (auto &collider : s.colliders)
            ConstructionColliderFields(wire, collider);
        for (auto &material : s.materials)
            ConstructionMaterialFields(wire, material);
        for (auto &joint : s.joints)
            if (!ConstructionJointFields(wire, joint, true))
                return false;
        for (auto &fixture : s.fixtures)
            ConstructionFixtureFields(wire, fixture);
        for (auto &child : s.queryChildren)
            ConstructionQueryChildFields(wire, child);
        for (auto &owner : s.owners)
            ConstructionOwnerFields(wire, owner);
        return wire.Complete();
    }

    bool ConstructionManifestRowSize(const ConstructionManifestHeader &h, std::size_t maximum, std::size_t &out) noexcept {
        out = 0;
        for (std::size_t index = 0; index < h.counts.size(); ++index) {
            if (h.counts[index] > (maximum - out) / ConstructionRowBytes[index])
                return false;
            out += std::size_t{h.counts[index]} * ConstructionRowBytes[index];
        }
        return true;
    }

    bool ConstructionManifestStorageFits(const ConstructionManifestHeader &h, const ConstructionManifestStorage &s,
                                         const PhysicsStateConstructionManifestLimits &l) noexcept {
        const std::array capacities{s.bodies.size(),   s.colliders.size(),     s.materials.size(), s.joints.size(),
                                    s.fixtures.size(), s.queryChildren.size(), s.owners.size()};
        const std::array limits{l.maximumBodies,   l.maximumColliders,     l.maximumMaterials, l.maximumJoints,
                                l.maximumFixtures, l.maximumQueryChildren, l.maximumOwners};
        for (std::size_t index = 0; index < h.counts.size(); ++index)
            if (h.counts[index] > capacities[index] || h.counts[index] > limits[index])
                return false;
        return true;
    }

    ConstructionManifestStorage ConstructionManifestSlice(const ConstructionManifestHeader &h, ConstructionManifestStorage s) noexcept {
        s.bodies = s.bodies.first(h.counts[0]);
        s.colliders = s.colliders.first(h.counts[1]);
        s.materials = s.materials.first(h.counts[2]);
        s.joints = s.joints.first(h.counts[3]);
        s.fixtures = s.fixtures.first(h.counts[4]);
        s.queryChildren = s.queryChildren.first(h.counts[5]);
        s.owners = s.owners.first(h.counts[6]);
        return s;
    }
}  // namespace Horo::Physics::StateDetail
