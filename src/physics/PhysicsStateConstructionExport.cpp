#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateConstructionStorage.h"

#include <algorithm>
#include <array>

namespace Horo::Physics::StateDetail {
    namespace {
        bool Overlaps(std::span<const std::byte> a, std::span<const std::byte> b) noexcept {
            if (a.empty() || b.empty())
                return false;
            const auto first = reinterpret_cast<std::uintptr_t>(a.data());
            const auto second = reinterpret_cast<std::uintptr_t>(b.data());
            return first <= second ? second - first < a.size() : first - second < b.size();
        }

        bool Disjoint(std::span<const ConstructionRecipeRootBinding> roots, const ConstructionRecipeExportStorage &s) noexcept {
            const std::array<std::span<const std::byte>, 6> ranges{std::as_bytes(roots),     std::as_bytes(s.nodes),
                                                                   std::as_bytes(s.edges),   std::as_bytes(s.roots),
                                                                   std::as_bytes(s.nodeMap), std::as_bytes(s.frames)};
            for (std::size_t index = 0; index < ranges.size(); ++index)
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (Overlaps(ranges[index], ranges[prior]))
                        return false;
            return true;
        }
    }  // namespace

    /** @brief Synchronous owner-only traversal borrows the existing pool; no second resource inventory. */
    class ConstructionRecipeExport final {
    public:
        ConstructionRecipeExport(const ConstructionRecipePool &pool, ConstructionRecipeExportStorage storage)
            : pool_(pool), storage_(storage) {}

        Result<PhysicsStateShapeRecipes> Run(std::span<const ConstructionRecipeRootBinding> roots) {
            using Outcome = Result<PhysicsStateShapeRecipes>;
            if (auto valid = Admit(roots); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            storage_.nodeMap = storage_.nodeMap.first(pool_.limits_.maximumNodes);
            std::ranges::fill(storage_.nodeMap, ConstructionRecipePool::None);
            for (std::size_t index = 0; index < roots.size(); ++index) {
                if (auto walked = Walk(roots[index].recipe.slot_); walked.HasError())
                    return Outcome::Failure(walked.ErrorValue());
                storage_.roots[index] = {roots[index].id, storage_.nodeMap[roots[index].recipe.slot_]};
            }
            return Outcome::Success(
                {storage_.nodes.first(nodeCount_), storage_.edges.first(edgeCount_), storage_.roots.first(roots.size())});
        }

    private:
        Result<void> Admit(std::span<const ConstructionRecipeRootBinding> roots) const {
            if (!pool_.Owner())
                return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
            if (!Disjoint(roots, storage_))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            if (roots.size() > pool_.limits_.maximumRoots || roots.size() > storage_.roots.size() ||
                storage_.nodeMap.size() < pool_.limits_.maximumNodes || storage_.frames.size() < pool_.limits_.maximumDepth)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return AdmitRoots(roots);
        }

        Result<void> AdmitRoots(std::span<const ConstructionRecipeRootBinding> roots) const {
            PhysicsStateObjectId previous{};
            for (const auto &root : roots) {
                if (!root.id || root.id <= previous)
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                if (!pool_.Exact(root.recipe))
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
                previous = root.id;
            }
            return Result<void>::Success();
        }

        Result<void> Push(std::uint32_t slot) {
            if (depth_ >= storage_.frames.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            storage_.frames[depth_++] = {slot, pool_.nodes_[slot].firstEdge};
            return Result<void>::Success();
        }

        Result<void> Walk(std::uint32_t root) {
            if (storage_.nodeMap[root] != ConstructionRecipePool::None)
                return Result<void>::Success();
            if (auto pushed = Push(root); pushed.HasError())
                return pushed;
            while (depth_) {
                auto &frame = storage_.frames[depth_ - 1];
                if (frame.nextEdge != ConstructionRecipePool::None) {
                    const auto &edge = pool_.edges_[frame.nextEdge];
                    frame.nextEdge = edge.next;
                    if (storage_.nodeMap[edge.child] == ConstructionRecipePool::None) {
                        if (auto pushed = Push(edge.child); pushed.HasError())
                            return pushed;
                    }
                } else {
                    if (auto emitted = Emit(frame.slot); emitted.HasError())
                        return emitted;
                    --depth_;
                }
            }
            return Result<void>::Success();
        }

        Result<void> Emit(std::uint32_t slot) {
            if (nodeCount_ >= storage_.nodes.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            const auto &source = pool_.nodes_[slot];
            auto recipe = source.recipe;
            if (auto *compound = std::get_if<PhysicsStateCompoundRecipe>(&recipe.geometry)) {
                if (compound->count > storage_.edges.size() - edgeCount_)
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
                compound->begin = static_cast<std::uint32_t>(edgeCount_);
                for (auto index = source.firstEdge; index != ConstructionRecipePool::None; index = pool_.edges_[index].next) {
                    const auto &edge = pool_.edges_[index];
                    storage_.edges[edgeCount_++] = {storage_.nodeMap[edge.child], edge.localPose, edge.userData};
                }
            }
            storage_.nodeMap[slot] = static_cast<std::uint32_t>(nodeCount_);
            storage_.nodes[nodeCount_++] = recipe;
            return Result<void>::Success();
        }

        const ConstructionRecipePool &pool_;
        ConstructionRecipeExportStorage storage_;
        std::size_t nodeCount_{}, edgeCount_{}, depth_{};
    };

    /** @copydoc ConstructionRecipePool::Export */
    Result<PhysicsStateShapeRecipes> ConstructionRecipePool::Export(std::span<const ConstructionRecipeRootBinding> roots,
                                                                    ConstructionRecipeExportStorage storage) const {
        return ConstructionRecipeExport{*this, storage}.Run(roots);
    }
}  // namespace Horo::Physics::StateDetail
