#pragma once
/** @file PCGPreviewSession.h @brief Cancellable editor-only CPU preview with immutable capture and bounded overlays. */
#include "Horo/Editor/PCGGraphDocument.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/PCG/PCGCpuEvaluator.h"

namespace Horo::Editor {
    /** @brief Exact document/provider fence; memory lifetime alone does not imply currentness. */
    struct PCGPreviewFence final {
        DocumentSessionId session;
        std::uint64_t documentRevision{};
        std::uint64_t catalogRevision{};
        Sha256Digest sourceState{};
        PCG::SpatialSnapshotId snapshot;
        PCG::SpatialProviderId provider;
        PCG::SpatialSourceId providerSource;
        PCG::SpatialRevision providerRevision;
        std::uint64_t originEpoch{};
        auto operator<=>(const PCGPreviewFence &) const = default;
    };

    /** @brief Immutable worker input; no live document, provider, scene, backend or commit owner is borrowed. */
    struct PCGPreviewRequest final {
        PCGDocumentSnapshot document;
        PCG::PCGNodeCatalogSnapshot catalog;
        PCG::PCGSpatialSnapshot spatial;
        std::shared_ptr<const PCG::PCGPointSchema> schema;
        std::vector<PCG::PCGCpuInput> inputs;
        PCG::PCGCpuEvaluationLimits evaluation;
        std::size_t maximumPointsPerOutput{65'536};
        CancellationToken cancellation;
    };

    /** @brief Owner-visible preview lifecycle; failures retain typed evaluator/cooker causes. */
    enum class PCGPreviewPhase : std::uint8_t {
        Idle,
        Evaluating,
        Ready,
        Failed,
        Cancelled,
        Closed
    };

    /** @brief Disposable immutable overlay; generated outputs retain node/pin identities and point transforms/bounds. */
    struct PCGPreviewOverlay final {
        std::uint64_t request{};
        PCGPreviewFence fence;
        PCG::PCGCpuCandidate candidate;
    };

    /** @brief Bounded owner-thread projection safe for presentation without a session pointer. */
    struct PCGPreviewSnapshot final {
        PCGPreviewPhase phase{PCGPreviewPhase::Idle};
        std::uint64_t request{};
        std::shared_ptr<const PCGPreviewOverlay> overlay;
        std::optional<Error> error;
    };

    /** @brief Session owner with at most one running and one coalesced pending capture.
     * @details All methods and destruction are owner-thread only; injected jobs outlive this object.
     * Workers own immutable captures and a synchronized result box, never this object. No PCGAsyncOperations,
     * runtime time-slicing, target adapters, scene commit or save authority is used. Ordinary validation,
     * transient cooking and EvaluatePCGCpu are the sole execution path. Frame advancement never waits.
     * Explicit shutdown may wait to its deadline; incomplete shutdown retains the job for retry. Destruction
     * cancels and joins the remaining cooperative worker before releasing captures.
     */
    class PCGPreviewSession final {
    public:
        /** @brief Creates an isolated session. @param jobs Borrowed process scheduler. @param world Host-issued isolated preview world.
         * @param maximumDrawPoints Finite total point/output overlay draw ceiling, 1 through 65536. */
        PCGPreviewSession(JobSystem &jobs, PCG::PCGWorldId world, std::size_t maximumDrawPoints = 8192);
        ~PCGPreviewSession();
        PCGPreviewSession(const PCGPreviewSession &) = delete;
        PCGPreviewSession &operator=(const PCGPreviewSession &) = delete;
        /** @brief Cancels older intent and replaces the single pending capture. @param request Owned exact input.
         * @return Never-reused intent ID or zero-admission failure; identical input requests still supersede prior intent. */
        [[nodiscard]] Result<std::uint64_t> Request(PCGPreviewRequest request);
        /** @brief Revalidates live owners and advances without waiting; mismatches immediately clear the overlay.
         * @param document Current immutable document, or null after close. @param spatial Current provider snapshot, or null after loss.
         * @return Current projection or owner-thread failure. */
        [[nodiscard]] Result<PCGPreviewSnapshot> Advance(const PCGDocumentSnapshot *document, const PCG::PCGSpatialSnapshot *spatial);
        /** @brief Revokes publication and drops the overlay/pending capture. @return Success or owner-thread failure. */
        [[nodiscard]] Result<void> Cancel();
        /** @brief Closes admission, cancels and joins by a finite deadline. @param timeout Positive shutdown wait budget.
         * @return Success after capture retirement, or incomplete wait error with ownership retained for retry. */
        [[nodiscard]] Result<void> Shutdown(Duration timeout);
        /** @brief Returns the finite global draw ceiling. @return Admitted point/output count. */
        [[nodiscard]] std::size_t MaximumDrawPoints() const noexcept;

    private:
        struct State;
        std::unique_ptr<State> state_;
        [[nodiscard]] Result<void> CheckAccess() const;
        [[nodiscard]] Result<void> StartPending();
        /** @brief Checks whether any current intent or visible output has lost its exact live owner fence. */
        [[nodiscard]] bool Obsolete(const PCGPreviewFence *current) const;
        /** @brief Checks the exact active operation supersession/lifecycle gate before publication. */
        [[nodiscard]] bool CanPublish(const PCGPreviewFence *current) const;
        /** @brief Consumes only terminal worker results on the owner lane, without waiting. */
        void CompleteActive(const PCGPreviewFence *current);
    };
}  // namespace Horo::Editor
