#pragma once

#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderGraphLifetime.h"
#include "Horo/Runtime/Render/RenderGraphTransientResources.h"
#include "Horo/Runtime/Render/RenderMemoryBudget.h"
#include "RenderGraphResourceLeasePool.h"

#include <array>
#include <memory>
#include <vector>

namespace Horo::Render::Detail {
    /** @brief Stable frontend-owned metadata; backing and completion pins remain registry-owned. */
    struct RenderGraphTransientResourceSet final {
        RenderGraphTransientResourcesHandle handle;
        std::vector<RenderGraphResourceLifetime> lifetimes;
        std::vector<RenderGraphResourceBinding> bindings;
        std::vector<RenderGraphResidentIdentity> backing;
        bool inFlight{false};
        bool released{false};
    };

    /**
     * @brief Owns a finite set of owner-thread graph realization records.
     *
     * Preparation happens before frame admission. Each logical alias slot owns one registry
     * generation and one budget claim. Released records remain stable while completion leases
     * borrow them; registry pins preserve native storage even after logical release.
     */
    class RenderGraphTransientResourcePool final {
    public:
        static constexpr std::size_t MaximumSets = 8;

        /** @brief Borrows the frontend's sole allocation and retirement authorities. */
        RenderGraphTransientResourcePool(IRenderBackend &backend, RenderResourceRegistry &registry, RenderMemoryBudget &budget);

        /**
         * @brief Realizes an entire lifetime plan transactionally before publication.
         * @param plan Intact owning lifetime proof.
         * @param scope Explicit admitted memory owner incarnation.
         * @return Set identity or the original typed failure after rollback.
         */
        [[nodiscard]] Result<RenderGraphTransientResourcesHandle> Prepare(const RenderGraphLifetimePlan &plan, RenderMemoryScopeId scope);

        /**
         * @brief Resolves an intact set for one exact compiled graph without acquiring a lease.
         * @param handle Frontend-issued, unreleased set identity.
         * @param graph Exact execution whose retained lifetimes must match preparation.
         * @return Stable record or a typed identity, lifetime, or in-flight rejection.
         */
        [[nodiscard]] Result<RenderGraphTransientResourceSet *> Resolve(RenderGraphTransientResourcesHandle handle,
                                                                        const CompiledRenderGraphExecution &graph);

        /**
         * @brief Retires a set's backing without revoking accepted GPU readers.
         * @param handle Exact unreleased set identity.
         * @return Success or a typed invalid, foreign, or stale identity result.
         */
        [[nodiscard]] Result<void> Release(RenderGraphTransientResourcesHandle handle);

    private:
        /** @brief Looks up one unreleased identity under the exact frontend incarnation. */
        [[nodiscard]] Result<RenderGraphTransientResourceSet *> Find(RenderGraphTransientResourcesHandle handle) const;
        /** @brief Releases each physical registry generation once; submission pins gate native destruction. */
        void Rollback(RenderGraphTransientResourceSet &set) noexcept;

        IRenderBackend *backend_;
        RenderResourceRegistry *registry_;
        RenderMemoryBudget *budget_;
        std::uint64_t nextIdentity_{1};
        bool rollbackIncomplete_{
            false}; /**< Closes preparation after cleanup allocation failure; registry/budget retain shutdown ownership. */
        std::array<std::unique_ptr<RenderGraphTransientResourceSet>, MaximumSets> sets_;
    };
}  // namespace Horo::Render::Detail
