#include "RenderGraphTransientResourcePool.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "RenderFrontendErrors.h"
#include "RenderResourceOperations.h"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace Horo::Render::Detail {
    namespace {
        /** @brief One reserved physical slot retained until complete set publication or rollback. */
        struct SlotClaim {
            RenderGraphTransientAllocationSlot slot;
            RenderGraphTransientDescriptor descriptor;
            ResourceReservation resource;
            RenderMemoryReservationId memory;
            RenderMemoryPlacement placement;
            RenderResourceClass resourceClass;
        };

        /** @brief Cancels unconsumed claims even when a native callback unwinds preparation. */
        class PendingClaims final {
        public:
            PendingClaims(RenderResourceRegistry &registry, RenderMemoryBudget &budget) : registry_(registry), budget_(budget) {}

            ~PendingClaims() {
                for (const auto &claim : slots) {
                    static_cast<void>(budget_.Cancel(claim.memory));
                    const auto state = registry_.State(claim.resourceClass, claim.resource.identity);
                    if (state.HasValue() && state.Value() == RenderResourceState::Pending)
                        static_cast<void>(registry_.CancelPending(claim.resourceClass, claim.resource.identity));
                    else if (state.HasValue() && state.Value() == RenderResourceState::Ready)
                        static_cast<void>(registry_.Release(claim.resourceClass, claim.resource.identity));
                }
                static_cast<void>(registry_.DrainRetirements());
            }

            void Commit() noexcept {
                slots.clear();
            }

            std::vector<SlotClaim> slots;

        private:
            RenderResourceRegistry &registry_;
            RenderMemoryBudget &budget_;
        };

        /** @brief Queries the selected backend without reclassifying its native placement requirements. */
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryCost(IRenderBackend &backend, const RenderGraphTransientDescriptor &descriptor) {
            return std::visit([&backend]<typename Descriptor>(const Descriptor &value) {
                if constexpr (std::is_same_v<Descriptor, RenderBufferDescriptor>)
                    return backend.QueryBufferMemoryCost(value);
                else
                    return backend.QueryTextureMemoryCost(value);
            }, descriptor);
        }

        /** @brief Reserves a registry generation and memory claim before any native slot allocation. */
        [[nodiscard]] Result<SlotClaim> ReserveSlot(IRenderBackend &backend, RenderResourceRegistry &registry, RenderMemoryBudget &budget,
                                                    const RenderMemoryScopeId scope,
                                                    const RenderGraphTransientAllocationRequirement &requirement) {
            const auto cost = QueryCost(backend, requirement.descriptor);
            if (cost.HasError())
                return Result<SlotClaim>::Failure(cost.ErrorValue());
            const auto resourceClass = std::holds_alternative<RenderBufferDescriptor>(requirement.descriptor)
                                           ? RenderResourceClass::Buffer
                                           : RenderResourceClass::Texture;
            const auto reserved = registry.Reserve(resourceClass);
            if (reserved.HasError())
                return Result<SlotClaim>::Failure(reserved.ErrorValue());
            const auto memory = budget.Reserve(scope, reserved.Value().operation, cost.Value());
            if (memory.HasError()) {
                static_cast<void>(registry.CancelPending(resourceClass, reserved.Value().identity));
                return Result<SlotClaim>::Failure(memory.ErrorValue());
            }
            const auto placement = budget.Placement(memory.Value());
            if (placement.HasError()) {
                static_cast<void>(budget.Cancel(memory.Value()));
                static_cast<void>(registry.CancelPending(resourceClass, reserved.Value().identity));
                return Result<SlotClaim>::Failure(placement.ErrorValue());
            }
            return Result<SlotClaim>::Success(
                {requirement.slot, requirement.descriptor, reserved.Value(), memory.Value(), placement.Value(), resourceClass});
        }

        /** @brief Uses the existing native realization and publication transaction for one admitted slot. */
        [[nodiscard]] Result<void> RealizeSlot(IRenderBackend &backend, RenderResourceRegistry &registry, RenderMemoryBudget &budget,
                                               const SlotClaim &claim) {
            RenderResourceUploadQueue::Request request{};
            request.identity = claim.resource.identity;
            request.memoryReservation = claim.memory;
            request.memoryPlacement = claim.placement;
            if (const auto *buffer = std::get_if<RenderBufferDescriptor>(&claim.descriptor)) {
                request.kind = RenderResourceUploadQueue::RequestKind::Buffer;
                request.buffer = *buffer;
            } else {
                request.kind = RenderResourceUploadQueue::RequestKind::Texture;
                request.texture = std::get<RenderTextureDescriptor>(claim.descriptor);
            }
            const auto created = RealizeResourceRequest(backend, registry, request, {});
            CompleteResourceRequest(backend, budget, registry, request, created);
            return registry.OperationResult(claim.resource.operation);
        }

        /** @brief Maps one admitted physical generation into a native-free graph binding. */
        [[nodiscard]] RenderGraphResourceBinding Binding(const SlotClaim &claim) {
            if (claim.resourceClass == RenderResourceClass::Buffer)
                return BufferHandle(claim.resource.identity);
            return TextureHandle(claim.resource.identity);
        }
    }  // namespace

    /** @copydoc RenderGraphTransientResourcePool::RenderGraphTransientResourcePool */
    RenderGraphTransientResourcePool::RenderGraphTransientResourcePool(IRenderBackend &backend, RenderResourceRegistry &registry,
                                                                       RenderMemoryBudget &budget)
        : backend_(&backend), registry_(&registry), budget_(&budget) {}

    /** @copydoc RenderGraphTransientResourcePool::Prepare */
    Result<RenderGraphTransientResourcesHandle> RenderGraphTransientResourcePool::Prepare(const RenderGraphLifetimePlan &plan,
                                                                                          const RenderMemoryScopeId scope) {
        using PrepareResult = Result<RenderGraphTransientResourcesHandle>;
        if (!plan.Owner().IsValid() || plan.Lifetimes().size() > RenderGraphLimits::HardMaxResources)
            return PrepareResult::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        if (!backend_->Capabilities().supportsExactTransientResourceReuse)
            return PrepareResult::Failure(
                MakeError(FrontendErrors::ResourceUnsupported, "The selected backend does not admit exact transient resource reuse."));
        const auto available = std::find_if(sets_.begin(), sets_.end(), [](const auto &set) {
            return !set || (set->released && !set->inFlight);
        });
        if (available == sets_.end() || nextIdentity_ == std::numeric_limits<std::uint64_t>::max())
            return PrepareResult::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        auto set = std::make_unique<RenderGraphTransientResourceSet>();
        set->handle = {registry_->Owner(), plan.Owner(), nextIdentity_++};
        set->lifetimes.assign(plan.Lifetimes().begin(), plan.Lifetimes().end());
        set->bindings.resize(plan.Lifetimes().size());
        set->backing.reserve(plan.AllocationRequirements().size());
        PendingClaims claims{*registry_, *budget_};
        claims.slots.reserve(plan.AllocationRequirements().size());
        for (const auto &requirement : plan.AllocationRequirements()) {
            auto existing = std::find_if(claims.slots.begin(), claims.slots.end(), [&requirement](const SlotClaim &claim) {
                return claim.slot == requirement.slot;
            });
            if (existing == claims.slots.end()) {
                auto reserved = ReserveSlot(*backend_, *registry_, *budget_, scope, requirement);
                if (reserved.HasError())
                    return PrepareResult::Failure(reserved.ErrorValue());
                claims.slots.push_back(std::move(reserved).Value());
                existing = std::prev(claims.slots.end());
            }
            const auto resource = std::find_if(set->lifetimes.begin(), set->lifetimes.end(), [&requirement](const auto &lifetime) {
                return lifetime.resource == requirement.resource;
            });
            if (resource == set->lifetimes.end())
                return PrepareResult::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            set->bindings[static_cast<std::size_t>(resource - set->lifetimes.begin())] = Binding(*existing);
        }
        for (const auto &claim : claims.slots) {
            if (const auto realized = RealizeSlot(*backend_, *registry_, *budget_, claim); realized.HasError())
                return PrepareResult::Failure(realized.ErrorValue());
            set->backing.push_back({claim.resourceClass, claim.resource.identity});
        }
        const auto handle = set->handle;
        *available = std::move(set);
        claims.Commit();
        return PrepareResult::Success(handle);
    }

    /** @copydoc RenderGraphTransientResourcePool::Find */
    Result<RenderGraphTransientResourceSet *> RenderGraphTransientResourcePool::Find(const RenderGraphTransientResourcesHandle handle) {
        if (!handle.IsValid())
            return Result<RenderGraphTransientResourceSet *>::Failure(MakeError(FrontendErrors::ResourceHandleMalformed));
        if (handle.renderer != registry_->Owner())
            return Result<RenderGraphTransientResourceSet *>::Failure(MakeError(FrontendErrors::ResourceWrongOwner));
        for (const auto &set : sets_) {
            if (set && set->handle == handle && !set->released)
                return Result<RenderGraphTransientResourceSet *>::Success(set.get());
        }
        return Result<RenderGraphTransientResourceSet *>::Failure(MakeError(FrontendErrors::ResourceStale));
    }

    /** @copydoc RenderGraphTransientResourcePool::Resolve */
    Result<RenderGraphTransientResourceSet *> RenderGraphTransientResourcePool::Resolve(const RenderGraphTransientResourcesHandle handle,
                                                                                        const CompiledRenderGraphExecution &graph) {
        using ResolveResult = Result<RenderGraphTransientResourceSet *>;
        const auto found = Find(handle);
        if (found.HasError())
            return found;
        auto &set = *found.Value();
        if (handle.graph != graph.Owner() || graph.Resources().size() != set.lifetimes.size())
            return ResolveResult::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        if (set.inFlight)
            return ResolveResult::Failure(
                MakeError(FrontendErrors::ResourceNotReady, "Transient backing remains in use by an accepted submission."));
        constexpr auto unused = std::numeric_limits<std::size_t>::max();
        std::array<std::size_t, RenderGraphLimits::HardMaxResources> first;
        std::array<std::size_t, RenderGraphLimits::HardMaxResources> last;
        first.fill(unused);
        last.fill(unused);
        for (std::size_t passIndex = 0; passIndex < graph.Passes().size(); ++passIndex) {
            const auto &pass = graph.Passes()[passIndex];
            if (!pass.usages.IsValidFor(graph.Usages().size()))
                return ResolveResult::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            for (const auto &use : graph.Usages().subspan(pass.usages.offset, pass.usages.count)) {
                if (use.resource.owner != graph.Owner() || use.resource.value == 0 || use.resource.value > set.lifetimes.size())
                    return ResolveResult::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
                const auto index = use.resource.value - 1U;
                first[index] = std::min(first[index], passIndex);
                last[index] = passIndex;
            }
        }
        for (std::size_t index = 0; index < set.lifetimes.size(); ++index) {
            const auto &lifetime = set.lifetimes[index];
            if (graph.Resources()[index].id != lifetime.resource)
                return ResolveResult::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            if (lifetime.disposition == RenderGraphLifetimeDisposition::Unused) {
                if (first[index] != unused)
                    return ResolveResult::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            } else if (first[index] != lifetime.firstUseIndex || last[index] != lifetime.lastUseIndex ||
                       graph.Passes()[first[index]].pass != lifetime.firstPass || graph.Passes()[last[index]].pass != lifetime.lastPass) {
                return ResolveResult::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            }
        }
        return found;
    }

    /** @copydoc RenderGraphTransientResourcePool::Rollback */
    void RenderGraphTransientResourcePool::Rollback(RenderGraphTransientResourceSet &set) noexcept {
        for (const auto &backing : set.backing)
            static_cast<void>(registry_->Release(backing.resourceClass, backing.identity));
        set.released = true;
        static_cast<void>(registry_->DrainRetirements());
    }

    /** @copydoc RenderGraphTransientResourcePool::Release */
    Result<void> RenderGraphTransientResourcePool::Release(const RenderGraphTransientResourcesHandle handle) {
        const auto found = Find(handle);
        if (found.HasError())
            return Result<void>::Failure(found.ErrorValue());
        Rollback(*found.Value());
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
