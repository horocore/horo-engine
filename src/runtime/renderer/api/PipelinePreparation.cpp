#include "Horo/Runtime/Render/PipelinePreparation.h"

#include "Horo/Runtime/Render/PipelinePreparationErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Render {
    namespace {
        /** @brief Adds metrics without wrapping after a long-lived host operation. */
        void SaturatingAdd(std::uint64_t &value, const std::uint64_t increment) noexcept {
            value += std::min(increment, std::numeric_limits<std::uint64_t>::max() - value);
        }

        /** @brief Validates finite storage, concurrency and dispatch-time envelopes before allocation. */
        [[nodiscard]] bool ValidBudget(const PipelinePreparationBudget &budget) noexcept {
            return budget.maximumPipelines > 0 && budget.maximumPipelines <= PipelinePreparationBudget::HardMaximumPipelines &&
                   budget.maximumInFlight > 0 && budget.maximumInFlight <= budget.maximumPipelines &&
                   budget.maximumDispatchesPerBatch > 0 && budget.maximumDispatchesPerBatch <= budget.maximumPipelines &&
                   budget.estimatedWorkMicroseconds > 0 && budget.maximumBatchMicroseconds >= budget.estimatedWorkMicroseconds &&
                   budget.hitchThresholdMicroseconds > 0;
        }

        /** @brief Rejects absent, duplicate or cyclic/implicit fallback identities in bounded load-time input. */
        [[nodiscard]] bool ValidUsage(const PipelineUsageManifest &manifest, const std::size_t index) noexcept {
            const auto &entry = manifest.pipelines[index];
            if (entry.key.digest == Sha256Digest{})
                return false;
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (manifest.pipelines[previous].key == entry.key)
                    return false;
            }
            if (!entry.fallback)
                return true;
            if (entry.required || *entry.fallback >= manifest.pipelines.size() || *entry.fallback == index)
                return false;
            const auto &fallback = manifest.pipelines[*entry.fallback];
            return fallback.cookedArtifactAvailable && !fallback.fallback;
        }
    }  // namespace

    /** @copydoc PipelinePreparation::PipelinePreparation */
    PipelinePreparation::PipelinePreparation(PipelineUsageManifest manifest, PipelinePreparationBudget budget,
                                             CancellationToken cancellation)
        : manifest_(std::move(manifest)), budget_(budget), entries_(manifest_.pipelines.size()), cancellation_(std::move(cancellation)) {
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            if (!manifest_.pipelines[index].cookedArtifactAvailable)
                entries_[index].state = State::SourcePending;
        }
    }

    /** @copydoc PipelinePreparation::Prepare */
    Result<PipelinePreparation> PipelinePreparation::Prepare(PipelineUsageManifest manifest, const PipelineCompilationMode mode,
                                                             PipelinePreparationBudget budget, CancellationToken cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<PipelinePreparation>::Failure(MakeError(PipelinePreparationErrors::Cancelled));
        if (!ValidBudget(budget))
            return Result<PipelinePreparation>::Failure(MakeError(PipelinePreparationErrors::InvalidBudget));
        if (manifest.generation == 0 || manifest.pipelines.size() > budget.maximumPipelines ||
            (mode != PipelineCompilationMode::Packaged && mode != PipelineCompilationMode::Development))
            return Result<PipelinePreparation>::Failure(MakeError(PipelinePreparationErrors::InvalidManifest));
        for (std::size_t index = 0; index < manifest.pipelines.size(); ++index) {
            if (!ValidUsage(manifest, index))
                return Result<PipelinePreparation>::Failure(MakeError(PipelinePreparationErrors::InvalidManifest));
            if (mode == PipelineCompilationMode::Packaged && !manifest.pipelines[index].cookedArtifactAvailable)
                return Result<PipelinePreparation>::Failure(MakeError(PipelinePreparationErrors::MissingCookedArtifact));
        }
        try {
            return Result<PipelinePreparation>::Success(PipelinePreparation(std::move(manifest), budget, std::move(cancellation)));
        } catch (const std::bad_alloc &) {
            return Result<PipelinePreparation>::Failure(MakeError(PipelinePreparationErrors::AllocationFailed));
        }
    }

    /** @copydoc PipelinePreparation::Dispatch */
    Result<std::vector<PipelinePreparationWork>> PipelinePreparation::Dispatch() {
        if (cancellation_.IsCancellationRequested())
            Close();
        if (closed_ || cancellation_.IsCancellationRequested())
            return Result<std::vector<PipelinePreparationWork>>::Failure(MakeError(
                cancellation_.IsCancellationRequested() ? PipelinePreparationErrors::Cancelled : PipelinePreparationErrors::Closed));
        const auto count =
            std::min({budget_.maximumDispatchesPerBatch, budget_.maximumInFlight - inFlight_,
                      static_cast<std::size_t>(std::min<std::uint64_t>(budget_.maximumPipelines, budget_.maximumBatchMicroseconds /
                                                                                                     budget_.estimatedWorkMicroseconds))});
        std::vector<PipelinePreparationWork> work;
        try {
            work.reserve(count);
            for (std::size_t index = 0; index < entries_.size() && work.size() < count; ++index) {
                const auto state = entries_[index].state;
                if (state == State::SourcePending || state == State::NativePending)
                    work.push_back({manifest_.generation, index, manifest_.pipelines[index].key,
                                    state == State::SourcePending ? PipelinePreparationAction::CompileSource
                                                                  : PipelinePreparationAction::RealizeCooked});
            }
        } catch (const std::bad_alloc &) {
            return Result<std::vector<PipelinePreparationWork>>::Failure(MakeError(PipelinePreparationErrors::AllocationFailed));
        }
        // Publish admission only after the whole batch allocation succeeds.
        for (const auto &item : work)
            entries_[item.index].state =
                item.action == PipelinePreparationAction::CompileSource ? State::SourceInFlight : State::NativeInFlight;
        inFlight_ += work.size();
        return Result<std::vector<PipelinePreparationWork>>::Success(std::move(work));
    }

    /** @copydoc PipelinePreparation::Complete */
    Result<void> PipelinePreparation::Complete(const PipelinePreparationWork &work, Result<void> outcome,
                                               const std::uint64_t durationMicroseconds) {
        if (closed_ || cancellation_.IsCancellationRequested())
            return Result<void>::Failure(MakeError(cancellation_.IsCancellationRequested() ? PipelinePreparationErrors::Cancelled
                                                                                           : PipelinePreparationErrors::Closed));
        if (work.generation != manifest_.generation || work.index >= entries_.size() || work.key != manifest_.pipelines[work.index].key ||
            (work.action != PipelinePreparationAction::CompileSource && work.action != PipelinePreparationAction::RealizeCooked))
            return Result<void>::Failure(MakeError(PipelinePreparationErrors::StaleCompletion));
        auto &entry = entries_[work.index];
        const auto expected = work.action == PipelinePreparationAction::CompileSource ? State::SourceInFlight : State::NativeInFlight;
        if (entry.state != expected)
            return Result<void>::Failure(MakeError(PipelinePreparationErrors::StaleCompletion));
        --inFlight_;
        SaturatingAdd(metrics_.completedWork, 1);
        SaturatingAdd(metrics_.totalWorkMicroseconds, durationMicroseconds);
        metrics_.maximumWorkMicroseconds = std::max(metrics_.maximumWorkMicroseconds, durationMicroseconds);
        if (durationMicroseconds > budget_.hitchThresholdMicroseconds)
            SaturatingAdd(metrics_.hitches, 1);
        if (outcome.HasError()) {
            entry.error = std::move(outcome).ErrorValue();
            entry.state = State::Failed;
            SaturatingAdd(metrics_.failedWork, 1);
        } else {
            entry.state = work.action == PipelinePreparationAction::CompileSource ? State::NativePending : State::Ready;
        }
        return Result<void>::Success();
    }

    /** @copydoc PipelinePreparation::Bind */
    Result<PipelineBindingSelection> PipelinePreparation::Bind(const std::size_t index) {
        if (closed_ || cancellation_.IsCancellationRequested())
            return Result<PipelineBindingSelection>::Failure(MakeError(
                cancellation_.IsCancellationRequested() ? PipelinePreparationErrors::Cancelled : PipelinePreparationErrors::Closed));
        if (index >= entries_.size())
            return Result<PipelineBindingSelection>::Failure(MakeError(PipelinePreparationErrors::InvalidManifest));
        if (entries_[index].state == State::Ready)
            return Result<PipelineBindingSelection>::Success({manifest_.pipelines[index].key, false});
        const auto fallback = manifest_.pipelines[index].fallback;
        if (fallback && entries_[*fallback].state == State::Ready) {
            SaturatingAdd(metrics_.fallbackBindings, 1);
            return Result<PipelineBindingSelection>::Success({manifest_.pipelines[*fallback].key, true});
        }
        if (entries_[index].error)
            return Result<PipelineBindingSelection>::Failure(*entries_[index].error);
        return Result<PipelineBindingSelection>::Failure(MakeError(PipelinePreparationErrors::Pending));
    }

    /** @copydoc PipelinePreparation::CheckReady */
    Result<void> PipelinePreparation::CheckReady() const {
        if (closed_ || cancellation_.IsCancellationRequested())
            return Result<void>::Failure(MakeError(cancellation_.IsCancellationRequested() ? PipelinePreparationErrors::Cancelled
                                                                                           : PipelinePreparationErrors::Closed));
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            if (!manifest_.pipelines[index].required)
                continue;
            if (entries_[index].error)
                return Result<void>::Failure(*entries_[index].error);
            if (entries_[index].state != State::Ready)
                return Result<void>::Failure(MakeError(PipelinePreparationErrors::Pending));
        }
        return Result<void>::Success();
    }

    /** @copydoc PipelinePreparation::Close */
    void PipelinePreparation::Close() noexcept {
        closed_ = true;
    }

    /** @copydoc PipelinePreparation::Metrics */
    const PipelinePreparationMetrics &PipelinePreparation::Metrics() const noexcept {
        return metrics_;
    }
}  // namespace Horo::Render
