#pragma once

/** @file WorldStreamingPreviewRuntime.h
 * @brief Explicit mounted index-inspection lifecycle using canonical runtime registry, planner and residency authorities.
 */
#include "Horo/Editor/WorldStreamingPreview.h"
#include "Horo/WorldStreaming/WorldIndexArtifact.h"

#include <chrono>

namespace Horo::Editor {
    /** @brief Host-selected inspection work/parse ceilings; this profile never mutates packaged world policy or admits cell I/O. */
    struct WorldStreamingPreviewRuntimePolicy final {
        WorldStreaming::StreamingSourcePlannerConfig planner;
        WorldStreaming::WorldIndexArtifactLimits artifact;
    };

    /** @brief Creates the explicit bounded editor inspection profile. @return Validated policy or original typed policy failure. */
    [[nodiscard]] Result<WorldStreamingPreviewRuntimePolicy> MakeWorldStreamingPreviewRuntimePolicy();

    /** @brief Sole mounted inspection authority for one validated cooked index and canonical document incarnation.
     * @details Owner lane only. Owns the actual registry, planner, source registrations, scheduler and residency ledger. It does not
     * load/activate packages, synthesize residency operations, report guessed budget usage, or own an editor world/camera/document.
     * Empty residency records mean unobserved, never fabricated Unloaded/Resident/Active. Hosts unbind the bridge before shutdown.
     */
    class WorldStreamingPreviewRuntime final {
    public:
        /** @brief Prepares a private complete native mount without publishing a bridge binding.
         * @param owner Fresh host-issued mounted identity. @param document Exact canonical document incarnation.
         * @param descriptor Topology decoded from actual world.index bytes, consumed while preparing the private candidate.
         * @param policy Explicit inspection-only limits and planner configuration.
         * @return Native owner or typed failure without replacing an existing mount. */
        [[nodiscard]] static Result<std::unique_ptr<WorldStreamingPreviewRuntime>> Create(
            WorldStreaming::StreamingRuntimeOwnerToken owner, WorldDocumentFence document,
            WorldStreaming::WorldPartitionDescriptor &&descriptor, const WorldStreamingPreviewRuntimePolicy &policy);
        /** @brief Shuts down native inspection owners; the host must revoke bridge borrows first. */
        ~WorldStreamingPreviewRuntime();
        WorldStreamingPreviewRuntime(const WorldStreamingPreviewRuntime &) = delete;
        WorldStreamingPreviewRuntime &operator=(const WorldStreamingPreviewRuntime &) = delete;
        /** @brief Advances one bounded real planner quantum from canonical authored sources and the native steady clock.
         * @param source Current immutable canonical document; replacement/replayed revisions are rejected.
         * @return True once a current plan/diagnostic publication exists, false while preparing, or a typed failure.
         * Source withdrawal invalidates the actual plan. Publications are throttled to 100 ms except changed source input. */
        [[nodiscard]] Result<bool> Service(const PartitionedWorldSnapshotLease &source);
        /** @brief Stops admission and revokes actual source registrations before native owner destruction. @return Typed shutdown result.
         */
        [[nodiscard]] Result<void> Shutdown();

        /** @brief Returns the exact mounted identity used for bridge unbind. @return Immutable owner token. */
        [[nodiscard]] WorldStreaming::StreamingRuntimeOwnerToken Owner() const noexcept {
            return owner_;
        }

        /** @brief Borrows the actual mounted registry. @return Native owner, outlived by this runtime. */
        [[nodiscard]] const WorldStreaming::WorldPartitionRegistry &Registry() const noexcept {
            return *registry_;
        }

        /** @brief Borrows the actual native source planner. @return Native owner, outlived by this runtime. */
        [[nodiscard]] const WorldStreaming::StreamingSourcePlanner &Planner() const noexcept {
            return *planner_;
        }

        /** @brief Borrows the current actual diagnostic publication. @return Null until the first successful planner publication. */
        [[nodiscard]] const WorldStreaming::WorldStreamingDiagnosticSnapshot *Diagnostics() const noexcept {
            return diagnostics_ ? &*diagnostics_ : nullptr;
        }

    private:
        struct Source final {
            PartitionedWorldSource authored;
            WorldStreaming::StreamingSourceRegistration registration;
        };

        /** @brief Takes unique native owners after complete creation succeeds. */
        WorldStreamingPreviewRuntime(WorldStreaming::StreamingRuntimeOwnerToken owner, WorldDocumentFence document,
                                     std::unique_ptr<WorldStreaming::WorldPartitionRegistry> registry,
                                     std::unique_ptr<WorldStreaming::StreamingSourcePlanner> planner,
                                     std::unique_ptr<WorldStreaming::StreamingSchedulerAdmissionLedger> scheduler,
                                     std::unique_ptr<WorldStreaming::StreamingCellStateLedger> residency, std::uint32_t maximumSources);
        /** @brief Reconciles actual source registrations against the canonical document, never an independent authoring model. */
        [[nodiscard]] Result<bool> Synchronize(const PartitionedWorldSnapshotLease &source);
        /** @brief Tests cadence reuse only while the real published plan remains valid for unchanged source input. */
        [[nodiscard]] bool CanReusePublication(bool sourceChanged, std::chrono::steady_clock::time_point now) const;
        /** @brief Captures actual scheduler/residency/source facts with explicit absent budget observations. */
        [[nodiscard]] Result<void> PublishDiagnostics();
        WorldStreaming::StreamingRuntimeOwnerToken owner_;
        WorldDocumentFence document_;
        std::unique_ptr<WorldStreaming::WorldPartitionRegistry> registry_;
        std::unique_ptr<WorldStreaming::StreamingSourcePlanner> planner_;
        std::unique_ptr<WorldStreaming::StreamingSchedulerAdmissionLedger> scheduler_;
        std::unique_ptr<WorldStreaming::StreamingCellStateLedger> residency_;
        std::vector<Source> sources_;
        std::optional<WorldStreaming::WorldStreamingDiagnosticSnapshot> diagnostics_;
        std::uint32_t maximumSources_{};
        std::uint64_t diagnosticRevision_{};
        std::chrono::steady_clock::time_point created_{std::chrono::steady_clock::now()};
        std::chrono::steady_clock::time_point serviced_{};
        std::thread::id lane_{std::this_thread::get_id()};
        bool closed_{};
    };
}  // namespace Horo::Editor
