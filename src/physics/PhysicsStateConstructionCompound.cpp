#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateConstructionStorage.h"
#include "PhysicsStateShapeRecipeHash.h"

#include <algorithm>

namespace Horo::Physics::StateDetail {
    Result<std::uint32_t> ConstructionRecipePool::AdmitChildren(std::span<const ConstructionRecipeChild> children) const {
        if (children.empty())
            return Result<std::uint32_t>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (children.size() > freeEdges_)
            return Result<std::uint32_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        std::uint32_t depth{};
        std::uint64_t ordinal{};
        for (const auto &child : children) {
            if (child.userData != ++ordinal)
                return Result<std::uint32_t>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            if (!Exact(child.recipe))
                return Result<std::uint32_t>::Failure(MakeError(PhysicsErrors::CapabilityStale));
            if (auto pose = ValidatePhysicsPose(child.localPose); pose.HasError())
                return Result<std::uint32_t>::Failure(pose.ErrorValue());
            depth = std::max(depth, nodes_[child.recipe.slot_].depth);
        }
        if (depth >= limits_.maximumDepth)
            return Result<std::uint32_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        return Result<std::uint32_t>::Success(depth + 1);
    }

    Result<ConstructionRecipeLease> ConstructionRecipePool::AddCompound(std::span<const ConstructionRecipeChild> children,
                                                                        PhysicsStateFpControl actual) {
        using Outcome = Result<ConstructionRecipeLease>;
        auto slot = ReserveNode(actual);
        if (slot.HasError())
            return Outcome::Failure(slot.ErrorValue());
        auto depth = AdmitChildren(children);
        if (depth.HasError())
            return Outcome::Failure(depth.ErrorValue());
        auto &node = nodes_[slot.Value()];
        node.recipe = {actual, PhysicsStateCompoundRecipe{0, static_cast<std::uint32_t>(children.size())}};
        node.depth = depth.Value();
        node.firstEdge = freeEdge_;
        auto hash = Detail::BeginCanonicalCompoundRecipe(children.size());
        for (const auto &child : children) {
            hash.Digest(nodes_[child.recipe.slot_].digest);
            hash.Pose(child.localPose);
            auto &edge = edges_[freeEdge_];
            freeEdge_ = edge.next;
            edge.child = child.recipe.slot_;
            edge.serial = child.recipe.serial_;
            edge.localPose = child.localPose;
            edge.userData = child.userData;
            AddReader(edge.child, edge.serial);
        }
        node.digest = Detail::BindCanonicalShapeCreationRecipe(hash.Finish(), actual);
        // The last allocated edge must not expose the remaining free-list tail.
        auto tail = node.firstEdge;
        for (std::size_t index = 1; index < children.size(); ++index)
            tail = edges_[tail].next;
        edges_[tail].next = None;
        freeEdges_ -= static_cast<std::uint32_t>(children.size());
        return Outcome::Success(Seal(slot.Value()));
    }

    Result<PhysicsStateShapeRecipe> ConstructionRecipePool::Read(const ConstructionRecipeLease &lease) const {
        if (!Owner())
            return Result<PhysicsStateShapeRecipe>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (!Exact(lease))
            return Result<PhysicsStateShapeRecipe>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        return Result<PhysicsStateShapeRecipe>::Success(nodes_[lease.slot_].recipe);
    }

    Result<std::size_t> ConstructionRecipePool::CopyChildren(const ConstructionRecipeLease &lease,
                                                             std::span<ConstructionRecipeChild> output) {
        if (!Owner())
            return Result<std::size_t>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (!Exact(lease))
            return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        const auto &node = nodes_[lease.slot_];
        const auto *group = std::get_if<PhysicsStateCompoundRecipe>(&node.recipe.geometry);
        const std::size_t count = group ? group->count : 0;
        if (output.size() < count)
            return Result<std::size_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        std::size_t index{};
        for (auto edgeIndex = node.firstEdge; edgeIndex != None; edgeIndex = edges_[edgeIndex].next) {
            const auto &edge = edges_[edgeIndex];
            AddReader(edge.child, edge.serial);
            output[index++] = {ConstructionRecipeLease{shared_from_this(), edge.child, edge.serial}, edge.localPose, edge.userData};
        }
        return Result<std::size_t>::Success(count);
    }

    Result<Sha256Digest> ConstructionRecipePool::Digest(const ConstructionRecipeLease &lease) const {
        if (!Owner())
            return Result<Sha256Digest>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (!Exact(lease))
            return Result<Sha256Digest>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        return Result<Sha256Digest>::Success(nodes_[lease.slot_].digest);
    }
}  // namespace Horo::Physics::StateDetail
