#pragma once
#include "Horo/Extensions/ScriptModuleRuntime.h"

#include <atomic>

namespace Horo::Extensions {
    struct ScriptModulePreparedRebind::Impl {
        virtual ~Impl() = default;
        virtual Result<void> Validate() = 0;
        virtual bool Commit() noexcept = 0;
    };

    struct ScriptModuleRebindAccess final {
        using Implementation = ScriptModulePreparedRebind::Impl;
        static std::unique_ptr<ScriptModulePreparedRebind> Create(std::unique_ptr<Implementation>);

        static bool CommitContext(ScriptModuleContext &current, const ScriptModuleContext &next) noexcept {
            return current.CommitPreparedReplacement(next);
        }
    };

    /** @brief Immutable publication protected by the VM State's atomic shared pointer for concurrent Close.
     * The Close latch-store/publication-load and commit publication-store/latch-load use sequential consistency.
     * Their single order forbids both sides observing the other's old value; release/acquire on two atomics is insufficient.
     * If Close snapshots the prior publication, commit observes its latch and closes the new actual context before returning.
     * Owner fields still drive VM access; Close snapshots both actual contexts and never reads mutable owner containers.
     */
    struct ScriptModulePublishedContexts final {
        std::shared_ptr<ScriptModuleContext> active, predecessor;

        void Close() const noexcept {
            if (active)
                active->Close();
            if (predecessor)
                predecessor->Close();
        }
    };

    /** Two snapshots belong to one predecessor: the removed active-only record and the removed active+predecessor record.
     * Retain both on the owner while a concurrent Close borrows either. Core context/lease destruction therefore stays on the owner.
     * A caller must keep the runtime object alive until concurrent Close returns, as for any concurrent member invocation. */
    bool ReapScriptModulePublications(auto &state) {
        for (auto &publication : state.retiredPublications)
            if (publication && publication.use_count() == 1)
                publication.reset();
        return !state.retiredPublications[0] && !state.retiredPublications[1];
    }

    /** Bounded owner-only predecessor pump shared by both adapters; it never invokes a VM or invents completion. */
    bool PollScriptModulePredecessor(auto &state) {
        static_cast<void>(ReapScriptModulePublications(state));
        if (!state.predecessor)
            return true;
        auto settled = state.predecessor->PollRetirement();
        if (settled.HasError()) {
            state.predecessorDiagnostic = settled.ErrorValue();
            state.predecessor->Close();
            return false;
        }
        if (!settled.Value() || state.PendingFrom(state.predecessor))
            return false;
        state.retiredPublications[1] = std::atomic_exchange_explicit(&state.published, state.activeOnly, std::memory_order_seq_cst);
        auto retired = std::move(state.predecessor);
        retired->Close();
        return true;
    }

    [[nodiscard]] Result<std::vector<std::shared_ptr<const ScriptModuleBindingPlan>>> PrepareScriptModulePlans(const ScriptModuleContext &);
    /** The next VM uses the same allocator, stack, bridge and execution budget; binding refresh cannot silently change that profile. */
    [[nodiscard]] bool SameScriptModuleLimits(const ScriptModuleRuntimeLimits &, const ScriptModuleRuntimeLimits &) noexcept;
}  // namespace Horo::Extensions
