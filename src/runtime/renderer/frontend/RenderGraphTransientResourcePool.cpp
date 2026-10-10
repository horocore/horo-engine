#include "RenderGraphTransientResourcePool.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "RenderFrontendErrors.h"
#include "RenderResourceOperations.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
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
            PendingClaims(RenderResourceRegistry &registry, RenderMemoryBudget &budget, bool &rollbackIncomplete)
                : registry_(registry), budget_(budget), rollbackIncomplete_(rollbackIncomplete) {}

            PendingClaims(const PendingClaims &) = delete;
            PendingClaims &operator=(const PendingClaims &) = delete;
            PendingClaims(PendingClaims &&) = delete;
            PendingClaims &operator=(PendingClaims &&) = delete;

            ~PendingClaims() noexcept {
                for (const auto &claim : slots_)
                    RollbackClaim(claim);
                static_cast<void>(registry_.DrainRetirements());
            }

            [[nodiscard]] std::vector<SlotClaim> &Slots() noexcept {
                return slots_;
            }

            void Commit() noexcept {
                slots_.clear();
            }

        private:
            /** @brief Keeps rollback allocation failures inside the registry's remaining shutdown ownership. */
            void RollbackClaim(const SlotClaim &claim) noexcept {
                try {
                    const auto state = registry_.State(claim.resourceClass, claim.resource.identity);
                    if (state.HasValue() && state.Value() == RenderResourceState::Pending) {
                        if (claim.memory.IsValid())
                            static_cast<void>(budget_.Cancel(claim.memory));
                        static_cast<void>(registry_.CancelPending(claim.resourceClass, claim.resource.identity));
                    } else if (state.HasValue() && state.Value() == RenderResourceState::Ready)
                        static_cast<void>(registry_.Release(claim.resourceClass, claim.resource.identity));
                } catch (const std::bad_alloc &) {
                    rollbackIncomplete_ = true;
                } catch (const std::length_error &) {
                    rollbackIncomplete_ = true;
                }
            }

            std::vector<SlotClaim> slots_;
            RenderResourceRegistry &registry_;
            RenderMemoryBudget &budget_;
            bool &rollbackIncomplete_;
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
        [[nodiscard]] Result<void> ReserveSlot(IRenderBackend &backend, RenderResourceRegistry &registry, RenderMemoryBudget &budget,
                                               std::vector<SlotClaim> &slots, const RenderMemoryScopeId scope,
                                               const RenderGraphTransientAllocationRequirement &requirement) {
            const auto cost = QueryCost(backend, requirement.descriptor);
            if (cost.HasError())
                return Result<void>::Failure(cost.ErrorValue());
            const auto resourceClass = std::holds_alternative<RenderBufferDescriptor>(requirement.descriptor)
                                           ? RenderResourceClass::Buffer
                                           : RenderResourceClass::Texture;
            const auto reserved = registry.Reserve(resourceClass);
            if (reserved.HasError())
                return Result<void>::Failure(reserved.ErrorValue());
            slots.emplace_back(requirement.slot, requirement.descriptor, reserved.Value(), RenderMemoryReservationId{},
                               RenderMemoryPlacement{}, resourceClass);
            SlotClaim &claim = slots.back();
            const auto memory = budget.Reserve(scope, reserved.Value().operation, cost.Value());
            if (memory.HasError())
                return Result<void>::Failure(memory.ErrorValue());
            claim.memory = memory.Value();
            const auto placement = budget.Placement(memory.Value());
            if (placement.HasError())
                return Result<void>::Failure(placement.ErrorValue());
            claim.placement = placement.Value();
            return Result<void>::Success();
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

        /** @brief Binds a proven logical resource to its admitted physical slot before native realization. */
        [[nodiscard]] Result<void> BindLogicalResource(RenderGraphTransientResourceSet &set, const RenderGraphResourceId resource,
                                                       const SlotClaim &claim) {
            const auto lifetime = std::ranges::find(set.lifetimes, resource, &RenderGraphResourceLifetime::resource);
            if (lifetime == set.lifetimes.end())
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            set.bindings[static_cast<std::size_t>(lifetime - set.lifetimes.begin())] = Binding(claim);
            return Result<void>::Success();
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
        if (rollbackIncomplete_)
            return PrepareResult::Failure(
                MakeError(FrontendErrors::ResourceBackendException,
                          "Transient rollback remains registry-owned; shut down the frontend before preparing more backing."));
        if (!plan.Owner().IsValid() || plan.Lifetimes().size() > RenderGraphLimits::HardMaxResources)
            return PrepareResult::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        if (!backend_->Capabilities().supportsExactTransientResourceReuse)
            return PrepareResult::Failure(
                MakeError(FrontendErrors::ResourceUnsupported, "The selected backend does not admit exact transient resource reuse."));
        const auto available = std::ranges::find_if(sets_, [](const auto &set) {
            return !set || (set->released && !set->inFlight);
        });
        if (available == sets_.end() || nextIdentity_ == std::numeric_limits<std::uint64_t>::max())
            return PrepareResult::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        auto set = std::make_unique<RenderGraphTransientResourceSet>();
        set->handle = {registry_->Owner(), plan.Owner(), nextIdentity_++};
        set->lifetimes.assign(plan.Lifetimes().begin(), plan.Lifetimes().end());
        set->bindings.resize(plan.Lifetimes().size());
        set->backing.reserve(plan.AllocationRequirements().size());
        PendingClaims claims{*registry_, *budget_, rollbackIncomplete_};
        auto &slots = claims.Slots();
        slots.reserve(plan.AllocationRequirements().size());
        for (const auto &requirement : plan.AllocationRequirements()) {
            auto existing = std::ranges::find_if(slots, [&requirement](const SlotClaim &claim) {
                return claim.slot == requirement.slot;
            });
            if (existing == slots.end()) {
                auto reserved = ReserveSlot(*backend_, *registry_, *budget_, slots, scope, requirement);
                if (reserved.HasError())
                    return PrepareResult::Failure(reserved.ErrorValue());
                existing = std::prev(slots.end());
            }
            if (const auto bound = BindLogicalResource(*set, requirement.resource, *existing); bound.HasError())
                return PrepareResult::Failure(bound.ErrorValue());
        }
        for (const auto &claim : slots) {
            if (const auto realized = RealizeSlot(*backend_, *registry_, *budget_, claim); realized.HasError())
                return PrepareResult::Failure(realized.ErrorValue());
            set->backing.emplace_back(claim.resourceClass, claim.resource.identity);
        }
        const auto handle = set->handle;
        *available = std::move(set);
        claims.Commit();
        return PrepareResult::Success(handle);
    }

    /** @copydoc RenderGraphTransientResourcePool::Find */
    Result<RenderGraphTransientResourceSet *> RenderGraphTransientResourcePool::Find(
        const RenderGraphTransientResourcesHandle handle) const {
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
