#include "PhysicsStateConstructionManifestOwner.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <array>
#include <limits>
#include <new>
#include <stdexcept>

namespace Horo::Physics::StateDetail {
    namespace {
        bool Charge(std::size_t count, std::size_t size, std::size_t &remaining) noexcept {
            if (count > remaining / size)
                return false;
            remaining -= count * size;
            return true;
        }

        bool ShapeBudget(const PhysicsStateConstructionManifestLimits &l, std::size_t &remaining) noexcept {
            return Charge(l.shapes.maximumNodes, sizeof(PhysicsStateShapeRecipe), remaining) &&
                   Charge(l.shapes.maximumEdges, sizeof(PhysicsStateShapeRecipeEdge), remaining) &&
                   Charge(l.shapes.maximumRoots, sizeof(PhysicsStateShapeRecipeRoot), remaining) &&
                   Charge(l.shapes.maximumRoots, sizeof(ConstructionRecipeRootBinding), remaining) &&
                   Charge(l.shapes.maximumDepth, sizeof(ConstructionRecipeExportFrame), remaining) &&
                   Charge(l.shapes.maximumNodes, 2 * sizeof(std::uint32_t), remaining) &&
                   Charge(l.shapes.maximumRoots, sizeof(std::uint32_t), remaining);
        }

        bool RowBudget(const PhysicsStateConstructionManifestLimits &l, std::size_t &remaining) noexcept {
            return Charge(l.maximumBodies, sizeof(PhysicsStateConstructionBody), remaining) &&
                   Charge(l.maximumColliders, sizeof(PhysicsStateConstructionCollider), remaining) &&
                   Charge(l.maximumMaterials, sizeof(PhysicsQueryMaterial), remaining) &&
                   Charge(l.maximumJoints, sizeof(PhysicsStateConstructionJoint), remaining) &&
                   Charge(l.maximumFixtures, sizeof(PhysicsStateConstructionFixture), remaining) &&
                   Charge(l.maximumQueryChildren, sizeof(PhysicsStateConstructionQueryChild), remaining) &&
                   Charge(l.maximumOwners, sizeof(PhysicsStatePersistentOwner), remaining) && Charge(l.maximumEncodedBytes, 1, remaining);
        }

        bool Budget(const PhysicsStateConstructionManifestLimits &l) noexcept {
            auto remaining = l.maximumRetainedBytes;
            return Charge(1, sizeof(ConstructionManifestOwner), remaining) && ShapeBudget(l, remaining) && RowBudget(l, remaining);
        }

        template <class T> std::size_t PayloadFor(const std::vector<T> &v) noexcept {
            return v.capacity() * sizeof(T);
        }
    }  // namespace

    /** @copydoc ConstructionManifestOwner::Prepare */
    Result<std::unique_ptr<ConstructionManifestOwner>> ConstructionManifestOwner::Prepare(PhysicsStateConstructionManifestLimits limits) {
        using Outcome = Result<std::unique_ptr<ConstructionManifestOwner>>;
        if (!Budget(limits))
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (limits.shapes.maximumNodes > std::numeric_limits<std::size_t>::max() - limits.shapes.maximumRoots)
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        try {
            auto owner = std::unique_ptr<ConstructionManifestOwner>(new ConstructionManifestOwner(limits));
            if (auto allocated = owner->Allocate(); allocated.HasError())
                return Outcome::Failure(allocated.ErrorValue());
            return Outcome::Success(std::move(owner));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        } catch (const std::length_error &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }

    Result<void> ConstructionManifestOwner::Allocate() {
        nodes_.resize(limits_.shapes.maximumNodes);
        edges_.resize(limits_.shapes.maximumEdges);
        roots_.resize(limits_.shapes.maximumRoots);
        bodies_.resize(limits_.maximumBodies);
        colliders_.resize(limits_.maximumColliders);
        materials_.resize(limits_.maximumMaterials);
        joints_.resize(limits_.maximumJoints);
        fixtures_.resize(limits_.maximumFixtures);
        children_.resize(limits_.maximumQueryChildren);
        owners_.resize(limits_.maximumOwners);
        rootBindings_.resize(limits_.shapes.maximumRoots);
        frames_.resize(limits_.shapes.maximumDepth);
        nodeMap_.resize(limits_.shapes.maximumNodes);
        validation_.resize(std::size_t{limits_.shapes.maximumNodes} + limits_.shapes.maximumRoots);
        encoded_.resize(limits_.maximumEncodedBytes);
        return RetainedPayloadBytes() <= limits_.maximumRetainedBytes ? Result<void>::Success()
                                                                      : Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
    }

    /** @copydoc ConstructionManifestOwner::Staging */
    ConstructionManifestStorage ConstructionManifestOwner::Staging() noexcept {
        if (sealed_)
            return {};
        return {{nodes_, edges_, roots_}, bodies_, colliders_, materials_, joints_, fixtures_, children_, owners_};
    }

    /** @copydoc ConstructionManifestOwner::ExportStorage */
    ConstructionRecipeExportStorage ConstructionManifestOwner::ExportStorage() noexcept {
        if (sealed_)
            return {};
        return {nodes_, edges_, roots_, nodeMap_, frames_};
    }

    /** @copydoc ConstructionManifestOwner::RetainedPayloadBytes */
    std::size_t ConstructionManifestOwner::RetainedPayloadBytes() const noexcept {
        const std::array<std::size_t, 15> arrays{PayloadFor(nodes_),        PayloadFor(edges_),     PayloadFor(roots_),
                                                 PayloadFor(bodies_),       PayloadFor(colliders_), PayloadFor(materials_),
                                                 PayloadFor(joints_),       PayloadFor(fixtures_),  PayloadFor(children_),
                                                 PayloadFor(rootBindings_), PayloadFor(frames_),    PayloadFor(nodeMap_),
                                                 PayloadFor(validation_),   PayloadFor(encoded_),   PayloadFor(owners_)};
        std::size_t total = sizeof(*this);
        for (const auto bytes : arrays) {
            if (bytes > std::numeric_limits<std::size_t>::max() - total)
                return std::numeric_limits<std::size_t>::max();
            total += bytes;
        }
        return total;
    }
}  // namespace Horo::Physics::StateDetail
