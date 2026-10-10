#pragma once

/** @file RenderGraphInspection.h
 * @brief Explicit bounded, owned graph inspection and deterministic debug export.
 */

#include "Horo/Runtime/Render/RenderGraphExecution.h"
#include "Horo/Runtime/Render/RenderGraphLifetime.h"

#include <memory>
#include <stop_token>
#include <string>
#include <thread>

namespace Horo::Render {
    /** @brief Exact frontend generation, real backend frame and inspection publication revision. */
    struct RenderGraphInspectionContext {
        RenderResourceOwnerId renderer;
        FrameToken frame;
        std::uint64_t revision{};
    };

    /** @brief Finite graph record and encoded-byte allowances; exhaustion rejects the entire capture. */
    struct RenderGraphInspectionLimits {
        static constexpr std::size_t HardMaxRecords = 262'144;
        static constexpr std::size_t HardMaxBytes = 16 * 1024 * 1024;
        std::size_t maxRecords{65'536};
        std::size_t maxBytes{HardMaxBytes};
    };

    /** @brief Timing availability preserves missing instrumentation independently of measured zero. */
    enum class RenderGraphInspectionTiming : std::uint8_t {
        Unavailable
    };

    /** @brief Retained pass and effective queue, without borrowed execution-array offsets. */
    struct RenderGraphInspectionExecutionPass {
        RenderGraphPassRef pass;
        RenderPassKind kind{RenderPassKind::Graphics};
        RenderQueueId queue;
    };

    /**
     * @brief Detached immutable graph facts, with no resource pins, native objects or borrowed storage.
     *
     * Capture is explicit tooling work over immutable compiler outputs, never an automatic draw-path
     * registry walk. Logical whole-resource transitions and allocation opportunities retain the
     * compiler's semantics; they do not claim native realization or GPU timing qualification.
     */
    class RenderGraphInspectionSnapshot final {
    public:
        RenderGraphInspectionSnapshot(const RenderGraphInspectionSnapshot &) = delete;
        RenderGraphInspectionSnapshot &operator=(const RenderGraphInspectionSnapshot &) = delete;

        /** @brief Returns exact source/publication identity. @return Owned immutable context. */
        [[nodiscard]] const RenderGraphInspectionContext &Context() const noexcept {
            return context_;
        }

        /** @brief Returns the captured graph identity. @return Non-zero source graph owner. */
        [[nodiscard]] RenderGraphOwnerId Owner() const noexcept {
            return owner_;
        }

        /** @brief Returns authored passes, including culled passes. @return Snapshot-lifetime view. */
        [[nodiscard]] std::span<const RenderGraphPass> Passes() const noexcept {
            return passes_;
        }

        /** @brief Returns exact compile/cull provenance. @return Snapshot-lifetime view. */
        [[nodiscard]] std::span<const RenderGraphPassDisposition> Dispositions() const noexcept {
            return dispositions_;
        }

        /** @brief Returns retained passes and effective queues in execution order. @return Snapshot-lifetime view. */
        [[nodiscard]] std::span<const RenderGraphInspectionExecutionPass> Execution() const noexcept {
            return execution_;
        }

        /** @brief Returns graph-local and resident Horo identities. @return Snapshot-lifetime view; handles confer no ownership. */
        [[nodiscard]] std::span<const RenderGraphResource> Resources() const noexcept {
            return resources_;
        }

        /** @brief Returns explicitly observable resource outputs. @return Snapshot-lifetime view. */
        [[nodiscard]] std::span<const RenderGraphResourceExport> Exports() const noexcept {
            return exports_;
        }

        /** @brief Returns all authored resource uses. @return Snapshot-lifetime view. */
        [[nodiscard]] std::span<const RenderGraphResourceUsage> Usages() const noexcept {
            return usages_;
        }

        /** @brief Returns all authored dependency edges. @return Snapshot-lifetime view. */
        [[nodiscard]] std::span<const RenderGraphDependency> Dependencies() const noexcept {
            return dependencies_;
        }

        /** @brief Returns compiled retained lifetimes. @return Snapshot-lifetime view. */
        [[nodiscard]] std::span<const RenderGraphResourceLifetime> Lifetimes() const noexcept {
            return lifetimes_;
        }

        /** @brief Returns logical allocation reuse opportunities, without granting native aliasing. @return Snapshot-lifetime view. */
        [[nodiscard]] std::span<const RenderGraphAliasOpportunity> Aliases() const noexcept {
            return aliases_;
        }

        /** @brief Returns logical transient allocation slot proof. @return Snapshot-lifetime view; no native allocation ownership. */
        [[nodiscard]] std::span<const RenderGraphTransientAllocationRequirement> Allocations() const noexcept {
            return allocations_;
        }

        /** @brief Returns logical whole-resource transitions. @return Snapshot-lifetime view. */
        [[nodiscard]] std::span<const RenderGraphTransition> Transitions() const noexcept {
            return transitions_;
        }

        /** @brief Returns matched logical queue ownership edges. @return Snapshot-lifetime view. */
        [[nodiscard]] std::span<const RenderGraphOwnershipTransfer> Transfers() const noexcept {
            return transfers_;
        }

        /** @brief Returns explicit unavailable timing until exact measurement joins are implemented. @return Unavailable. */
        [[nodiscard]] RenderGraphInspectionTiming Timing() const noexcept {
            return RenderGraphInspectionTiming::Unavailable;
        }

    private:
        friend Result<std::shared_ptr<const RenderGraphInspectionSnapshot>> CaptureRenderGraphInspection(
            const RenderGraph &, const RenderGraphSchedule &, const RenderGraphLifetimePlan &, const CompiledRenderGraphExecution &,
            RenderGraphInspectionContext, RenderGraphInspectionLimits, std::stop_token);
        RenderGraphInspectionSnapshot() = default;
        RenderGraphInspectionContext context_;
        RenderGraphOwnerId owner_;
        std::vector<RenderGraphPass> passes_;
        std::vector<RenderGraphPassDisposition> dispositions_;
        std::vector<RenderGraphInspectionExecutionPass> execution_;
        std::vector<RenderGraphResource> resources_;
        std::vector<RenderGraphResourceExport> exports_;
        std::vector<RenderGraphResourceUsage> usages_;
        std::vector<RenderGraphDependency> dependencies_;
        std::vector<RenderGraphResourceLifetime> lifetimes_;
        std::vector<RenderGraphAliasOpportunity> aliases_;
        std::vector<RenderGraphTransientAllocationRequirement> allocations_;
        std::vector<RenderGraphTransition> transitions_;
        std::vector<RenderGraphOwnershipTransfer> transfers_;
    };

    /**
     * @brief Copies complete compiler facts transactionally into detached immutable storage.
     * @param graph Intact finalized graph; all source objects must remain immutable/alive during this call.
     * @param schedule Schedule compiled from this exact graph.
     * @param lifetime Lifetime plan compiled from this exact graph/schedule.
     * @param execution Execution plan compiled from this exact graph/schedule.
     * @param context Host-supplied exact renderer owner, real backend frame and non-zero revision.
     * @param limits Finite record/owned-byte capacities; no partial or silently truncated capture.
     * @param cancellation Cooperative cancellation observed between bounded copies and before publication.
     * @return Owned immutable snapshot or typed identity, capacity, cancellation or allocation failure.
     */
    [[nodiscard]] Result<std::shared_ptr<const RenderGraphInspectionSnapshot>> CaptureRenderGraphInspection(
        const RenderGraph &graph, const RenderGraphSchedule &schedule, const RenderGraphLifetimePlan &lifetime,
        const CompiledRenderGraphExecution &execution, RenderGraphInspectionContext context, RenderGraphInspectionLimits limits = {},
        std::stop_token cancellation = {});

    /**
     * @brief Encodes the already captured complete graph as deterministic versioned JSON without native content or paths.
     * @param snapshot Detached captured facts; export never recaptures or calls a backend.
     * @param maxBytes Finite output allowance no greater than the inspection hard byte bound.
     * @param cancellation Cooperative cancellation; failure returns no partial output.
     * @return Owned JSON bytes or typed capacity, cancellation or allocation failure. Host Platform services own safe file publication.
     */
    [[nodiscard]] Result<std::string> ExportRenderGraphInspection(const RenderGraphInspectionSnapshot &snapshot,
                                                                  std::size_t maxBytes = RenderGraphInspectionLimits::HardMaxBytes,
                                                                  std::stop_token cancellation = {});

    /**
     * @brief Owner-thread publication slot for explicit captures; readers retain detached immutable values.
     *
     * Every method except destruction runs on the constructing thread; there are no callbacks,
     * workers, mutexes or automatic capture. Replacement releases the slot, never invalidates
     * already returned snapshots, and rejects old-generation publication. The host owns reader
     * retention budgets and safe-point dispatch. Shutdown closes admission before renderer teardown.
     */
    class RenderGraphInspectionFeed final {
    public:
        /** @brief Constructs an inert publication slot. @param renderer Exact current renderer owner. */
        explicit RenderGraphInspectionFeed(RenderResourceOwnerId renderer) noexcept;
        RenderGraphInspectionFeed(const RenderGraphInspectionFeed &) = delete;
        RenderGraphInspectionFeed &operator=(const RenderGraphInspectionFeed &) = delete;
        /** @brief Publishes a complete current-generation, increasing-revision capture. @param snapshot Owned candidate.
         * @return Success or typed affinity, closed, identity or stale-revision failure; previous value remains intact on failure. */
        [[nodiscard]] Result<void> Publish(std::shared_ptr<const RenderGraphInspectionSnapshot> snapshot);
        /** @brief Reads the latest publication without source access. @return Owned snapshot, empty before capture, or affinity failure. */
        [[nodiscard]] Result<std::shared_ptr<const RenderGraphInspectionSnapshot>> Read() const;
        /** @brief Replaces the renderer generation and releases retained publication. @param renderer New distinct valid owner.
         * @return Success or typed affinity, closed or identity failure. */
        [[nodiscard]] Result<void> ReplaceRenderer(RenderResourceOwnerId renderer);
        /** @brief Idempotently closes admission and releases the publication. @return Success or typed affinity failure. */
        [[nodiscard]] Result<void> Shutdown();

    private:
        const std::thread::id thread_{std::this_thread::get_id()};
        RenderResourceOwnerId renderer_;
        std::uint64_t revision_{};
        std::shared_ptr<const RenderGraphInspectionSnapshot> snapshot_;
        bool closed_{};
    };
}  // namespace Horo::Render
