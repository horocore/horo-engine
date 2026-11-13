#include "PhysicsStateConstructionManifestInternal.h"

#include <array>

namespace Horo::Physics::StateDetail {
    namespace {
        template <class T> bool Selectors(const T &v) noexcept {
            return v.layer.IsValid() && v.profile.IsValid() && v.channel.IsValid() &&
                   static_cast<std::uint8_t>(v.response) <= static_cast<std::uint8_t>(PhysicsQueryFixtureResponse::Block);
        }

        template <class T> bool Evidence(const T &v) noexcept {
            return (!v.material || ConstructionMaterialValid(*v.material)) &&
                   (!v.surface || (v.material && v.surface->collider == 0 && ConstructionSurfaceValid(*v.surface)));
        }

        const PhysicsStateShapeRecipe *Recipe(const PhysicsStateConstructionManifest &m, PhysicsStateObjectId shape) noexcept {
            auto found = std::ranges::lower_bound(m.shapes.roots, shape, {}, &PhysicsStateShapeRecipeRoot::shape);
            return found != m.shapes.roots.end() && found->shape == shape ? &m.shapes.nodes[found->recipe] : nullptr;
        }

        bool DirectAnalyticChildren(const PhysicsStateConstructionManifest &m, PhysicsStateCompoundRecipe group) noexcept {
            for (const auto &edge : m.shapes.edges.subspan(group.begin, group.count)) {
                const auto *shape = std::get_if<PhysicsShapeDescriptor>(&m.shapes.nodes[edge.child].geometry);
                if (!shape || std::holds_alternative<PhysicsStaticPlaneShape>(*shape))
                    return false;
            }
            return true;
        }

        Result<void> Children(const PhysicsStateConstructionManifest &m, const PhysicsStateConstructionFixture &f) {
            // Same existing closed query-compound limit (PhysicsQuery.cpp ValidateCompoundShape).
            if (f.childCount > 256)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            std::array<std::uint64_t, 256> ids{};
            const auto rows = m.queryChildren.subspan(f.childBegin, f.childCount);
            for (std::size_t index = 0; index < rows.size(); ++index) {
                const auto &child = rows[index];
                if (!child.subshape.IsValid() || !Selectors(child) || !Evidence(child))
                    return MalformedConstruction();
                if (child.trigger != f.trigger)
                    return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
                ids[index] = child.subshape.Value();
            }
            auto used = std::span{ids}.first(f.childCount);
            std::ranges::sort(used);
            return std::ranges::adjacent_find(used) == used.end() ? Result<void>::Success() : MalformedConstruction();
        }

        Result<void> Geometry(const PhysicsStateConstructionManifest &m, const PhysicsStateConstructionFixture &f) {
            const auto *recipe = Recipe(m, f.shape);
            if (!recipe)
                return MalformedConstruction();
            const auto *group = std::get_if<PhysicsStateCompoundRecipe>(&recipe->geometry);
            if (!group)
                return f.childCount == 0 ? Result<void>::Success() : MalformedConstruction();
            if (group->count != f.childCount || f.subshape || f.material || f.surface)
                return MalformedConstruction();
            if (!DirectAnalyticChildren(m, *group))
                return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            return Children(m, f);
        }

        bool ChildSlice(const PhysicsStateConstructionManifest &m, const PhysicsStateConstructionFixture &f,
                        std::size_t nextChild) noexcept {
            return f.childBegin == nextChild && f.childBegin <= m.queryChildren.size() &&
                   f.childCount <= m.queryChildren.size() - f.childBegin;
        }

        Result<void> Fixture(const PhysicsStateConstructionManifest &m, const PhysicsStateConstructionFixture &f, std::size_t &nextChild) {
            if (HasSceneBody(m, f.body) || !Selectors(f) || !Evidence(f))
                return MalformedConstruction();
            if (f.subshape && !f.subshape->IsValid())
                return MalformedConstruction();
            if (auto pose = ValidatePhysicsPose(f.originalPose); pose.HasError())
                return pose;
            if (!ChildSlice(m, f, nextChild))
                return MalformedConstruction();
            auto geometry = Geometry(m, f);
            if (geometry.HasValue())
                nextChild += f.childCount;
            return geometry;
        }
    }  // namespace

    Result<void> ValidateConstructionFixtures(const PhysicsStateConstructionManifest &m, PhysicsStateFpControl current) {
        if (!Ordered(m.fixtures, &PhysicsStateConstructionFixture::body))
            return MalformedConstruction();
        std::size_t nextChild{};
        for (const auto &fixture : m.fixtures) {
            if (fixture.floatingPoint != current)
                return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            if (auto valid = Fixture(m, fixture, nextChild); valid.HasError())
                return valid;
        }
        return nextChild == m.queryChildren.size() ? Result<void>::Success() : MalformedConstruction();
    }
}  // namespace Horo::Physics::StateDetail
