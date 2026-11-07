#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateConstructionStorage.h"

#include <algorithm>
#include <limits>

namespace Horo::Physics::StateDetail {
    void ConstructionRecipePool::ReleaseNode(Node &node) noexcept {
        auto current = node.firstEdge;
        while (current != None) {
            auto &edge = edges_[current];
            const auto next = edge.next;
            DropReader(edge.child, edge.serial);
            edge.next = freeEdge_;
            freeEdge_ = current;
            ++freeEdges_;
            current = next;
        }
        node.firstEdge = None;
        node.occupied = false;
        node.recipe = {};
    }

    Result<std::size_t> ConstructionRecipePool::Collect(std::size_t maximumRecords) {
        if (!Owner())
            return Result<std::size_t>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        const auto scan = std::min<std::size_t>(maximumRecords, limits_.maximumNodes);
        std::size_t reclaimed{};
        for (std::size_t index = 0; index < scan; ++index) {
            auto &node = nodes_[cursor_];
            cursor_ = (cursor_ + 1) % limits_.maximumNodes;
            if (node.occupied && node.readers.load(std::memory_order_acquire) == 0) {
                ReleaseNode(node);
                ++reclaimed;
            }
        }
        return Result<std::size_t>::Success(reclaimed);
    }

    Result<void> ConstructionRecipePool::CanReserve(std::size_t nodes, std::size_t edges) const {
        if (!Owner())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (edges > freeEdges_)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (nodes != 0 && (!nextSerial_ || nodes - 1 > std::numeric_limits<std::uint64_t>::max() - nextSerial_))
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        std::size_t available{};
        for (std::uint32_t index = 0; index < limits_.maximumNodes; ++index)
            available += !nodes_[index].occupied;
        return nodes <= available ? Result<void>::Success() : Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
    }
}  // namespace Horo::Physics::StateDetail
