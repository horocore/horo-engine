#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsStateConstruction.h"
#include "PhysicsStateValidationInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Physics {
    namespace {
        bool Fits(const PhysicsStateShapeRecipes &r, const PhysicsStateConstructionLimits &l) noexcept {
            return l.maximumNodes && l.maximumRoots && l.maximumDepth && l.maximumBytes && r.nodes.size() <= l.maximumNodes &&
                   r.edges.size() <= l.maximumEdges && r.roots.size() <= l.maximumRoots;
        }

        bool Charge(std::size_t count, std::size_t stride, std::size_t &remaining) noexcept {
            if (count > remaining / stride)
                return false;
            remaining -= count * stride;
            return true;
        }

        bool StorageFits(const PhysicsStateShapeRecipes &r, std::size_t remaining) noexcept {
            return Charge(r.nodes.size(), sizeof(PhysicsStateShapeRecipe), remaining) &&
                   Charge(r.edges.size(), sizeof(PhysicsStateShapeRecipeEdge), remaining) &&
                   Charge(r.roots.size(), sizeof(PhysicsStateShapeRecipeRoot), remaining);
        }

        bool StorageAdmitted(const PhysicsStateShapeRecipes &r, const PhysicsStateConstructionLimits &limits,
                             std::size_t scratchCount) noexcept {
            return Fits(r, limits) && limits.maximumDepth < (1U << 31U) && StorageFits(r, limits.maximumBytes) &&
                   scratchCount >= r.nodes.size();
        }

        bool ScratchAliases(const PhysicsStateShapeRecipes &r, std::span<std::uint32_t> scratch) noexcept {
            auto bytes = std::span{reinterpret_cast<std::uint8_t *>(scratch.data()), scratch.size_bytes()};
            return StateDetail::Overlaps(bytes, r.nodes) || StateDetail::Overlaps(bytes, r.edges) || StateDetail::Overlaps(bytes, r.roots);
        }

        Result<void> Compound(const PhysicsStateShapeRecipes &r, PhysicsStateCompoundRecipe group, std::size_t parent,
                              std::uint32_t maximumDepth, std::span<std::uint32_t> depths, std::size_t &nextEdge) {
            if (group.count == 0 || group.begin != nextEdge || group.begin > r.edges.size() || group.count > r.edges.size() - group.begin)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            std::uint32_t deepest{};
            std::uint64_t ordinal{};
            for (const auto &edge : r.edges.subspan(group.begin, group.count)) {
                if (edge.userData != ++ordinal)
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                if (edge.child >= parent)
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                if (auto pose = ValidatePhysicsPose(edge.localPose); pose.HasError())
                    return pose;
                deepest = std::max(deepest, depths[edge.child]);
            }
            if (deepest >= maximumDepth)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            depths[parent] = deepest + 1;
            nextEdge += group.count;
            return Result<void>::Success();
        }

        Result<void> Nodes(const PhysicsStateShapeRecipes &r, const PhysicsStateConstructionLimits &limits, PhysicsStateFpControl current,
                           std::span<std::uint32_t> depths) {
            std::size_t nextEdge{};
            for (std::size_t index = 0; index < r.nodes.size(); ++index) {
                const auto &node = r.nodes[index];
                if (node.floatingPoint != current)
                    return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
                if (const auto *analytic = std::get_if<PhysicsShapeDescriptor>(&node.geometry)) {
                    if (auto valid = ValidatePhysicsShapeDescriptor(*analytic); valid.HasError())
                        return valid;
                    depths[index] = 1;
                } else if (auto valid = Compound(r, std::get<PhysicsStateCompoundRecipe>(node.geometry), index, limits.maximumDepth, depths,
                                                 nextEdge);
                           valid.HasError())
                    return valid;
            }
            return nextEdge == r.edges.size() ? Result<void>::Success()
                                              : Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        }

        bool AllReachable(const PhysicsStateShapeRecipes &r, std::span<std::uint32_t> depths) noexcept {
            constexpr std::uint32_t marked = 1U << 31U;
            for (const auto &root : r.roots)
                depths[root.recipe] |= marked;
            for (std::size_t end = r.nodes.size(); end != 0; --end) {
                const auto index = end - 1;
                if ((depths[index] & marked) == 0)
                    return false;
                if (const auto *group = std::get_if<PhysicsStateCompoundRecipe>(&r.nodes[index].geometry))
                    for (const auto &edge : r.edges.subspan(group->begin, group->count))
                        depths[edge.child] |= marked;
            }
            return true;
        }

        bool Roots(const PhysicsStateShapeRecipes &r) noexcept {
            PhysicsStateObjectId previous{};
            for (const auto &root : r.roots) {
                if (root.shape <= previous || root.recipe >= r.nodes.size())
                    return false;
                previous = root.shape;
            }
            return r.nodes.empty() == r.roots.empty();
        }
    }  // namespace

    /** @copydoc ValidatePhysicsStateShapeRecipes */
    Result<void> ValidatePhysicsStateShapeRecipes(const PhysicsStateShapeRecipes &r, const PhysicsStateConstructionLimits &limits,
                                                  PhysicsStateFpControl current, std::span<std::uint32_t> depths) {
        if (current.architecture == PhysicsStateFpArchitecture::Unspecified ||
            static_cast<unsigned>(current.architecture) > static_cast<unsigned>(PhysicsStateFpArchitecture::ArmFpcr))
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        if (!StorageAdmitted(r, limits, depths.size()))
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        depths = depths.first(r.nodes.size());
        if (ScratchAliases(r, depths) || !Roots(r))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (auto nodes = Nodes(r, limits, current, depths); nodes.HasError())
            return nodes;
        return AllReachable(r, depths) ? Result<void>::Success() : Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
    }
}  // namespace Horo::Physics
