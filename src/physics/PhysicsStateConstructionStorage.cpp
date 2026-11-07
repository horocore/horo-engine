#include "PhysicsStateConstructionStorage.h"

#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateShapeRecipeHash.h"

#include <limits>
#include <new>

namespace Horo::Physics::StateDetail {
    namespace {
        bool KnownProfile(PhysicsStateFpControl profile) noexcept {
            return profile.architecture == PhysicsStateFpArchitecture::ArmFpcr ||
                   profile.architecture == PhysicsStateFpArchitecture::X86MxcsrX87;
        }

        bool Charge(std::size_t count, std::size_t size, std::size_t &budget) noexcept {
            if (count > budget / size)
                return false;
            budget -= count * size;
            return true;
        }
    }  // namespace

    ConstructionRecipePool::ConstructionRecipePool(PhysicsStateConstructionLimits limits, PhysicsStateFpControl profile,
                                                   PhysicsStateConstructionTracking tracking)
        : limits_(limits), profile_(profile), tracking_(tracking), owner_(std::this_thread::get_id()),
          nodes_(std::make_unique<Node[]>(limits.maximumNodes)), edges_(std::make_unique<Edge[]>(limits.maximumEdges)),
          freeEdge_(limits.maximumEdges ? 0 : None), freeEdges_(limits.maximumEdges) {
        for (std::uint32_t index = 0; index < limits.maximumEdges; ++index)
            edges_[index].next = index + 1 == limits.maximumEdges ? None : index + 1;
    }

    bool ConstructionRecipePool::ConfigurationValid(const PhysicsStateConstructionLimits &limits, PhysicsStateFpControl profile,
                                                    PhysicsStateConstructionTracking tracking) noexcept {
        if (tracking != PhysicsStateConstructionTracking::Required && tracking != PhysicsStateConstructionTracking::Optional)
            return false;
        return limits.maximumNodes != 0 && limits.maximumNodes != None && limits.maximumDepth != 0 && limits.maximumDepth < (1U << 31U) &&
               KnownProfile(profile);
    }

    Result<std::shared_ptr<ConstructionRecipePool>> ConstructionRecipePool::Prepare(PhysicsStateConstructionLimits limits,
                                                                                    PhysicsStateFpControl profile,
                                                                                    PhysicsStateConstructionTracking tracking) {
        using Outcome = Result<std::shared_ptr<ConstructionRecipePool>>;
        if (!ConfigurationValid(limits, profile, tracking))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        auto remaining = limits.maximumBytes;
        if (!Charge(1, sizeof(ConstructionRecipePool), remaining) || !Charge(limits.maximumNodes, sizeof(Node), remaining) ||
            !Charge(limits.maximumEdges, sizeof(Edge), remaining))
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        try {
            return Outcome::Success(std::shared_ptr<ConstructionRecipePool>(new ConstructionRecipePool(limits, profile, tracking)));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }

    bool ConstructionRecipePool::Owner() const noexcept {
        return owner_ == std::this_thread::get_id();
    }

    bool ConstructionRecipePool::Exact(const ConstructionRecipeLease &lease) const noexcept {
        if (lease.owner_.get() != this || lease.slot_ >= limits_.maximumNodes)
            return false;
        const auto &node = nodes_[lease.slot_];
        return node.occupied && node.serial == lease.serial_ && node.readers.load(std::memory_order_acquire) != 0;
    }

    std::size_t ConstructionRecipePool::RetainedPayloadBytes() const noexcept {
        return sizeof(*this) + std::size_t{limits_.maximumNodes} * sizeof(Node) + std::size_t{limits_.maximumEdges} * sizeof(Edge);
    }

    Result<std::uint32_t> ConstructionRecipePool::ReserveNode(PhysicsStateFpControl actual) {
        if (!Owner())
            return Result<std::uint32_t>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (!KnownProfile(actual) || (tracking_ == PhysicsStateConstructionTracking::Required && actual != profile_))
            return Result<std::uint32_t>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        if (!nextSerial_)
            return Result<std::uint32_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        for (std::uint32_t index = 0; index < limits_.maximumNodes; ++index)
            if (!nodes_[index].occupied)
                return Result<std::uint32_t>::Success(index);
        return Result<std::uint32_t>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
    }

    ConstructionRecipeLease ConstructionRecipePool::Seal(std::uint32_t slot) noexcept {
        auto &node = nodes_[slot];
        node.serial = nextSerial_++;
        node.occupied = true;
        node.readers.store(1, std::memory_order_release);
        return ConstructionRecipeLease{shared_from_this(), slot, node.serial};
    }

    void ConstructionRecipePool::AddReader(std::uint32_t slot, std::uint64_t) noexcept {
        // A valid source lease itself holds a reader. At least sizeof(lease) bytes per live alias
        // make uint64 overflow unreachable in the addressable storage model. No weak promotion exists.
        nodes_[slot].readers.fetch_add(1, std::memory_order_relaxed);
    }

    void ConstructionRecipePool::DropReader(std::uint32_t slot, std::uint64_t) noexcept {
        nodes_[slot].readers.fetch_sub(1, std::memory_order_acq_rel);
    }

    Result<ConstructionRecipeLease> ConstructionRecipePool::AddAnalytic(const PhysicsShapeDescriptor &shape, PhysicsStateFpControl actual) {
        using Outcome = Result<ConstructionRecipeLease>;
        if (!Owner())
            return Outcome::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (auto valid = ValidatePhysicsShapeDescriptor(shape); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        auto slot = ReserveNode(actual);
        if (slot.HasError())
            return Outcome::Failure(slot.ErrorValue());
        auto &node = nodes_[slot.Value()];
        node.recipe = {actual, shape};
        node.digest = Detail::BindCanonicalShapeCreationRecipe(Detail::CanonicalPrimitiveRecipe(shape), actual);
        node.firstEdge = None;
        node.depth = 1;
        return Outcome::Success(Seal(slot.Value()));
    }
}  // namespace Horo::Physics::StateDetail
