#include "PhysicsStateConstructionManifestInternal.h"

namespace Horo::Physics {
    namespace {
        bool Counts(const PhysicsStateConstructionManifest &m, const PhysicsStateConstructionManifestLimits &l) noexcept {
            return m.bodies.size() <= l.maximumBodies && m.colliders.size() <= l.maximumColliders &&
                   m.materials.size() <= l.maximumMaterials && m.joints.size() <= l.maximumJoints &&
                   m.fixtures.size() <= l.maximumFixtures && m.queryChildren.size() <= l.maximumQueryChildren &&
                   m.owners.size() <= l.maximumOwners;
        }

        template <class T> bool Charge(std::span<const T> rows, std::size_t &remaining) noexcept {
            if (rows.size() > remaining / sizeof(T))
                return false;
            remaining -= rows.size_bytes();
            return true;
        }

        bool ScratchStorage(std::size_t count, std::size_t &remaining) noexcept {
            if (count > remaining / sizeof(std::uint32_t))
                return false;
            remaining -= count * sizeof(std::uint32_t);
            return true;
        }

        bool RowStorage(const PhysicsStateConstructionManifest &m, std::size_t &remaining) noexcept {
            return Charge(m.bodies, remaining) && Charge(m.colliders, remaining) && Charge(m.materials, remaining) &&
                   Charge(m.joints, remaining) && Charge(m.fixtures, remaining) && Charge(m.queryChildren, remaining) &&
                   Charge(m.owners, remaining);
        }

        bool Storage(const PhysicsStateConstructionManifest &m, std::size_t scratchCount, std::size_t remaining) noexcept {
            const bool geometry =
                Charge(m.shapes.nodes, remaining) && Charge(m.shapes.edges, remaining) && Charge(m.shapes.roots, remaining);
            return geometry && RowStorage(m, remaining) && ScratchStorage(scratchCount, remaining);
        }

        bool Disjoint(const PhysicsStateConstructionManifest &m, std::span<std::uint32_t> scratch) noexcept {
            const auto bytes = std::span{reinterpret_cast<std::uint8_t *>(scratch.data()), scratch.size_bytes()};
            const std::array inputs{std::as_bytes(m.shapes.nodes), std::as_bytes(m.shapes.edges), std::as_bytes(m.shapes.roots),
                                    std::as_bytes(m.bodies),       std::as_bytes(m.colliders),    std::as_bytes(m.materials),
                                    std::as_bytes(m.joints),       std::as_bytes(m.fixtures),     std::as_bytes(m.queryChildren),
                                    std::as_bytes(m.owners)};
            return std::ranges::none_of(inputs, [bytes](auto input) {
                return StateDetail::Overlaps(bytes, input);
            });
        }

        std::size_t RootIndex(const PhysicsStateConstructionManifest &m, PhysicsStateObjectId id) noexcept {
            return std::ranges::lower_bound(m.shapes.roots, id, {}, &PhysicsStateShapeRecipeRoot::shape) - m.shapes.roots.begin();
        }

        bool QueryShapeOwnership(const PhysicsStateConstructionManifest &m, std::span<std::uint32_t> marks) noexcept {
            std::ranges::fill(marks, 0);
            for (const auto &body : m.bodies)
                marks[RootIndex(m, body.shape)] = 1;
            for (const auto &fixture : m.fixtures) {
                auto &mark = marks[RootIndex(m, fixture.shape)];
                if (mark != 0)
                    return false;
                mark = 2;
            }
            return true;
        }

        Result<void> PolicyRows(const PhysicsStateConstructionManifest &m, PhysicsStateFpControl current) {
            if (auto bodies = StateDetail::ValidateConstructionBodies(m, current); bodies.HasError())
                return bodies;
            if (auto joints = StateDetail::ValidateConstructionJoints(m, current); joints.HasError())
                return joints;
            if (auto fixtures = StateDetail::ValidateConstructionFixtures(m, current); fixtures.HasError())
                return fixtures;
            return StateDetail::ValidateConstructionOwners(m);
        }
    }  // namespace

    /** @copydoc ValidatePhysicsStateConstructionManifest */
    Result<void> ValidatePhysicsStateConstructionManifest(const PhysicsStateConstructionManifest &m,
                                                          const PhysicsStateConstructionManifestLimits &limits,
                                                          PhysicsStateFpControl current, std::span<std::uint32_t> scratch) {
        if (!Counts(m, limits))
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (m.shapes.nodes.size() > scratch.size() || m.shapes.roots.size() > scratch.size() - m.shapes.nodes.size())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        const auto used = m.shapes.nodes.size() + m.shapes.roots.size();
        if (!Storage(m, used, limits.maximumRetainedBytes))
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        scratch = scratch.first(used);
        if (!Disjoint(m, scratch))
            return StateDetail::MalformedConstruction();
        if (auto graph = ValidatePhysicsStateShapeRecipes(m.shapes, limits.shapes, current, scratch.first(m.shapes.nodes.size()));
            graph.HasError())
            return graph;
        if (auto motion = StateDetail::ValidateConstructionMotion(m, scratch.first(m.shapes.nodes.size())); motion.HasError())
            return motion;
        if (auto policies = PolicyRows(m, current); policies.HasError())
            return policies;
        return QueryShapeOwnership(m, scratch.subspan(m.shapes.nodes.size())) ? Result<void>::Success()
                                                                              : StateDetail::MalformedConstruction();
    }
}  // namespace Horo::Physics
