#include "Horo/Editor/WorldStreamingPreview.h"

#include <cmath>
#include <set>
#include <tuple>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;
        using Cells = std::vector<WorldStreamingPreviewCell>;

        /** @brief Preserves a typed failure without altering any authority. */
        template <typename T> Result<T> Fail(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Tests that each publication belongs to the exact host-captured world. */
        bool SamePublications(const PartitionedWorldSnapshot &source, const WS::WorldPartitionRegistrySnapshot &registry,
                              const WS::StreamingSourcePlanView &plan, const WS::WorldStreamingDiagnosticSnapshot &diagnostics,
                              const WorldStreamingPreviewFence &expected) {
            return source.fence == expected.document && registry.Binding() == expected.registry && plan.partition == expected.registry &&
                   plan.revision == expected.planRevision && diagnostics.Revision() == expected.diagnostics &&
                   diagnostics.Owner() == expected.registry.owner && source.document.settings.partition == diagnostics.Owner().partition;
        }

        /** @brief Validates captures before allocating any rows. */
        Result<void> ValidateCapture(const PartitionedWorldSnapshotLease &source, const WS::WorldPartitionRegistrySnapshot &registry,
                                     const WS::StreamingSourcePlanView &plan, const WS::WorldStreamingDiagnosticSnapshot &diagnostics,
                                     const WorldStreamingPreviewFence &expected) {
            if (!source || !registry.IsValid() || expected.viewRevision == 0 || plan.revision == 0)
                return Fail<void>(WorldDocumentErrors::Invalid);
            if (!SamePublications(*source, registry, plan, diagnostics, expected))
                return Fail<void>(WorldDocumentErrors::Stale);
            if (diagnostics.Lifecycle() == WS::WorldStreamingRuntimeCompositionState::Closed)
                return Fail<void>(WorldDocumentErrors::Lifecycle);
            return Result<void>::Success();
        }

        /** @brief Rejects zero or excessive consumer ceilings independently of input size. */
        bool ValidLimits(const WorldStreamingPreviewLimits limits) {
            return limits.cells != 0 && limits.cells <= 4096 && limits.attempts != 0 && limits.attempts <= 1024 && limits.decisions != 0 &&
                   limits.decisions <= 4096 && limits.contributions != 0 && limits.contributions <= 16384;
        }

        /** @brief Applies all row ceilings before copying any borrowed data. */
        bool Fits(const WS::WorldPartitionRegistrySnapshot &registry, const WS::StreamingSourcePlanView &plan,
                  const WS::WorldStreamingDiagnosticSnapshot &diagnostics, const WorldStreamingPreviewLimits limits) {
            return registry.Cells().size() <= limits.cells && plan.cells.size() <= limits.cells &&
                   plan.contributions.size() <= limits.contributions && diagnostics.Cells().size() <= limits.attempts &&
                   diagnostics.Failures().size() <= limits.attempts && diagnostics.Events().size() <= limits.decisions;
        }

        /** @brief Prepares canonical registry rows using the existing checked spatial index. */
        Result<Cells> BuildRows(const WS::WorldPartitionRegistrySnapshot &registry, const std::stop_token cancellation) {
            Cells cells;
            cells.reserve(registry.Cells().size());
            for (const auto &cell : registry.Cells()) {
                if (cancellation.stop_requested())
                    return Fail<Cells>(WorldDocumentErrors::Cancelled);
                const auto handle = registry.Find(cell.id);
                if (handle.HasError())
                    return Result<Cells>::Failure(handle.ErrorValue());
                const auto bounds = registry.CellBounds(handle.Value());
                if (bounds.HasError())
                    return Result<Cells>::Failure(bounds.ErrorValue());
                cells.push_back({.cell = handle.Value(), .bounds = bounds.Value()});
            }
            return Result<Cells>::Success(std::move(cells));
        }

        /** @brief Checks closed desired-state vocabularies without inferring runtime residency. */
        Result<void> ValidateDesired(const WS::StreamingPlannedCell &desired) {
            if (desired.residency > WS::StreamingDesiredResidency::Activated ||
                (desired.pinnedFloor && *desired.pinnedFloor > WS::StreamingDesiredResidency::Activated) ||
                desired.stability.phase >= WS::StreamingCellStabilityPhase::Count)
                return Fail<void>(WorldDocumentErrors::Unsupported);
            if (!std::isfinite(desired.priority))
                return Fail<void>(WorldDocumentErrors::Invalid);
            return Result<void>::Success();
        }

        /** @brief Copies one exact desired row per registry cell, rejecting duplicate and foreign handles. */
        Result<void> AttachDesired(Cells &cells, const WS::WorldPartitionRegistrySnapshot &registry,
                                   const WS::StreamingSourcePlanView &plan, const std::stop_token cancellation) {
            for (const auto &desired : plan.cells) {
                if (cancellation.stop_requested())
                    return Fail<void>(WorldDocumentErrors::Cancelled);
                const auto resolved = registry.Resolve(desired.cell);
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                const auto valid = ValidateDesired(desired);
                if (valid.HasError())
                    return valid;
                auto &row = cells[desired.cell.slot];
                if (row.desired)
                    return Fail<void>(WorldDocumentErrors::Conflict);
                row.desired = desired;
            }
            return Result<void>::Success();
        }

        /** @brief Validates source provenance and prediction restrictions using the canonical desired-state validator. */
        Result<void> ValidateContribution(const WS::StreamingPlannedSourceCell &contribution, const WS::StreamingRuntimeOwnerToken &owner) {
            const auto desired =
                WS::StreamingSourceDesiredState::Create(contribution.source, contribution.residency, contribution.retention);
            if (desired.HasError())
                return Result<void>::Failure(desired.ErrorValue());
            if (contribution.source.owner.partition != owner.partition || contribution.source.owner.epoch != owner.epoch)
                return Fail<void>(WorldDocumentErrors::Stale);
            if (contribution.predicted && (contribution.residency > WS::StreamingDesiredResidency::Loaded ||
                                           contribution.retention != WS::StreamingRetention::Releasable))
                return Fail<void>(WorldDocumentErrors::Invalid);
            return Result<void>::Success();
        }

        /** @brief Copies source reasons while distinguishing current demand from the same source's predicted demand. */
        Result<void> AttachContributions(Cells &cells, const WS::WorldPartitionRegistrySnapshot &registry,
                                         const WS::StreamingSourcePlanView &plan, const std::stop_token cancellation) {
            std::set<std::tuple<std::uint32_t, WS::StreamingSourceId, bool>> seen;
            for (const auto &contribution : plan.contributions) {
                if (cancellation.stop_requested())
                    return Fail<void>(WorldDocumentErrors::Cancelled);
                const auto resolved = registry.Resolve(contribution.cell);
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                const auto valid = ValidateContribution(contribution, registry.Binding().owner);
                if (valid.HasError())
                    return valid;
                if (!seen.emplace(contribution.cell.slot, contribution.source.id, contribution.predicted).second)
                    return Fail<void>(WorldDocumentErrors::Conflict);
                cells[contribution.cell.slot].contributions.push_back(contribution);
            }
            return Result<void>::Success();
        }

        /** @brief Joins an already validated diagnostic row set without collapsing cell-attempt generations. */
        template <typename T>
        Result<void> AttachEvidence(Cells &cells, const WS::WorldPartitionRegistrySnapshot &registry, const std::span<const T> evidence,
                                    std::vector<T> WorldStreamingPreviewCell::*member, const std::stop_token cancellation) {
            for (const auto &entry : evidence) {
                if (cancellation.stop_requested())
                    return Fail<void>(WorldDocumentErrors::Cancelled);
                const auto cell = registry.Find(entry.operation.fence.cell);
                if (cell.HasError())
                    return Result<void>::Failure(cell.ErrorValue());
                (cells[cell.Value().slot].*member).push_back(entry);
            }
            return Result<void>::Success();
        }

        /** @brief Copies all canonical diagnostic channels, including failures when events are disabled. */
        Result<void> AttachDiagnostics(Cells &cells, const WS::WorldPartitionRegistrySnapshot &registry,
                                       const WS::WorldStreamingDiagnosticSnapshot &diagnostics, const std::stop_token cancellation) {
            const auto attempts = AttachEvidence(cells, registry, diagnostics.Cells(), &WorldStreamingPreviewCell::attempts, cancellation);
            if (attempts.HasError())
                return attempts;
            const auto failures =
                AttachEvidence(cells, registry, diagnostics.Failures(), &WorldStreamingPreviewCell::failures, cancellation);
            if (failures.HasError())
                return failures;
            return AttachEvidence(cells, registry, diagnostics.Events(), &WorldStreamingPreviewCell::decisions, cancellation);
        }
    }  // namespace

    /** @copydoc WorldStreamingPreview::Create */
    Result<WorldStreamingPreview> WorldStreamingPreview::Create(
        PartitionedWorldSnapshotLease source, const WS::WorldPartitionRegistrySnapshot &registry, const WS::StreamingSourcePlanView &plan,
        const WS::WorldStreamingDiagnosticSnapshot &diagnostics, const WorldStreamingPreviewFence expected,
        const WorldStreamingPreviewLimits limits, const std::stop_token cancellation) {
        if (cancellation.stop_requested())
            return Fail<WorldStreamingPreview>(WorldDocumentErrors::Cancelled);
        const auto capture = ValidateCapture(source, registry, plan, diagnostics, expected);
        if (capture.HasError())
            return Result<WorldStreamingPreview>::Failure(capture.ErrorValue());
        if (!ValidLimits(limits) || !Fits(registry, plan, diagnostics, limits))
            return Fail<WorldStreamingPreview>(WorldDocumentErrors::Capacity);
        auto rows = BuildRows(registry, cancellation);
        if (rows.HasError())
            return Result<WorldStreamingPreview>::Failure(rows.ErrorValue());
        auto &cells = rows.Value();
        const auto desired = AttachDesired(cells, registry, plan, cancellation);
        if (desired.HasError())
            return Result<WorldStreamingPreview>::Failure(desired.ErrorValue());
        const auto contributions = AttachContributions(cells, registry, plan, cancellation);
        if (contributions.HasError())
            return Result<WorldStreamingPreview>::Failure(contributions.ErrorValue());
        const auto runtime = AttachDiagnostics(cells, registry, diagnostics, cancellation);
        if (runtime.HasError())
            return Result<WorldStreamingPreview>::Failure(runtime.ErrorValue());
        if (cancellation.stop_requested())
            return Fail<WorldStreamingPreview>(WorldDocumentErrors::Cancelled);
        return Result<WorldStreamingPreview>::Success(
            WorldStreamingPreview{std::move(source), expected, diagnostics.Lifecycle(), std::move(cells)});
    }
}  // namespace Horo::Editor
