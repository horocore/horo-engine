#include "ExposureGpuKernel.h"

namespace Horo::Render::Detail {
    namespace {
        /** @brief Tests actual native terminal state without waiting for the GPU. */
        bool Terminal(id<MTLCommandBuffer> commands) noexcept {
            return commands == nil || commands.status == MTLCommandBufferStatusCompleted || commands.status == MTLCommandBufferStatusError;
        }

        /** @brief Requires accepted native same-queue submission, never merely recorded commands. */
        bool Accepted(const ExposureGpuJournalEntry &entry, id<MTLCommandQueue> queue) noexcept {
            if (!entry.accepted || entry.commands == nil || entry.commands.commandQueue != queue)
                return false;
            const auto status = entry.commands.status;
            return status == MTLCommandBufferStatusCommitted || status == MTLCommandBufferStatusScheduled ||
                   status == MTLCommandBufferStatusCompleted;
        }

        /** @brief Matches caller scope metadata to the actual native recorded buffer generation. */
        bool SameState(const ExposureState &a, const ExposureState &b) noexcept {
            return a.view == b.view && a.generation == b.generation && a.settingsGeneration == b.settingsGeneration &&
                   a.deviceGeneration == b.deviceGeneration && a.sourceSubmission == b.sourceSubmission && a.buffer == b.buffer;
        }
    }  // namespace

    /** @copydoc ExposureGpuKernel::Latest */
    const ExposureGpuJournalEntry *ExposureGpuKernel::Latest(RenderViewId view) const noexcept {
        const ExposureGpuJournalEntry *latest = nullptr;
        for (const auto &entry : journal)
            if (entry.buffer != nil && entry.state.view == view &&
                (!latest || entry.state.sourceSubmission > latest->state.sourceSubmission))
                latest = &entry;
        return latest;
    }

    /** @copydoc ExposureGpuKernel::LatestAccepted */
    const ExposureGpuJournalEntry *ExposureGpuKernel::LatestAccepted(RenderViewId view) const noexcept {
        const ExposureGpuJournalEntry *latest = nullptr;
        for (const auto &entry : journal)
            if (entry.buffer != nil && entry.state.view == view && Accepted(entry, entry.commands.commandQueue) &&
                (!latest || entry.state.sourceSubmission > latest->state.sourceSubmission))
                latest = &entry;
        return latest;
    }

    /** @copydoc ExposureGpuKernel::InvalidateFailedChains */
    void ExposureGpuKernel::InvalidateFailedChains() noexcept {
        for (const auto &failed : journal) {
            if (failed.commands == nil || failed.commands.status != MTLCommandBufferStatusError)
                continue;
            for (auto &entry : journal)
                if (entry.state.view == failed.state.view && entry.chainStart == failed.chainStart)
                    entry.invalid = true;
        }
    }

    /** @copydoc ExposureGpuKernel::Prune */
    void ExposureGpuKernel::Prune() noexcept {
        InvalidateFailedChains();
        for (auto &entry : journal)
            if (entry.buffer != nil && Terminal(entry.commands) && Latest(entry.state.view) != &entry &&
                LatestAccepted(entry.state.view) != &entry)
                entry = {};
    }

    /** @copydoc ExposureGpuKernel::Predecessor */
    bool ExposureGpuKernel::Predecessor(const RenderGraphExposureGpu &work, id<MTLBuffer> previous,
                                        id<MTLCommandQueue> queue) const noexcept {
        const auto *entry = Latest(work.produced.view);
        return entry && !entry->invalid && entry->buffer == previous && Accepted(*entry, queue) && SameState(entry->state, work.previous) &&
               work.produced.generation > entry->state.generation;
    }

    /** @copydoc ExposureGpuKernel::ResourcesReusable */
    bool ExposureGpuKernel::ResourcesReusable(const RenderGraphExposureGpu &work, id<MTLBuffer> previous,
                                              id<MTLBuffer> output) const noexcept {
        if (previous == output || previous == nil || output == nil)
            return false;
        for (const auto &entry : journal) {
            if (entry.buffer == nil)
                continue;
            if (entry.buffer == output)
                return false;
            if (work.resetPrevious && entry.buffer == previous)
                return false;
        }
        return true;
    }

    /** @copydoc ExposureGpuKernel::Increasing */
    bool ExposureGpuKernel::Increasing(const RenderGraphExposureGpu &work) const noexcept {
        const auto *latest = Latest(work.produced.view);
        return !latest || work.produced.sourceSubmission > latest->state.sourceSubmission;
    }

    /** @copydoc ExposureGpuKernel::Prepare */
    Result<std::size_t> ExposureGpuKernel::Prepare(const RenderGraphExposureGpu &work, id<MTLBuffer> previous, id<MTLBuffer> output,
                                                   id<MTLCommandQueue> queue) {
        using Return = Result<std::size_t>;
        if (std::this_thread::get_id() != lane)
            return Return::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        Prune();
        if (!Increasing(work) || !ResourcesReusable(work, previous, output))
            return Return::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        if (!work.resetPrevious && !Predecessor(work, previous, queue))
            return Return::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        for (std::size_t i = 0; i < journal.size(); ++i)
            if (journal[i].buffer == nil)
                return Return::Success(i);
        return Return::Failure(MakeError(ExposureGpuErrors::Capacity));
    }

    /** @copydoc ExposureGpuKernel::Record */
    void ExposureGpuKernel::Record(std::size_t slot, const RenderGraphExposureGpu &work, id<MTLBuffer> output,
                                   id<MTLCommandBuffer> commands) noexcept {
        const auto *previous = Latest(work.produced.view);
        const auto chain = work.resetPrevious ? work.produced.sourceSubmission : previous->chainStart;
        journal[slot] = {output, commands, work.produced, false, chain, false};
    }

    /** @copydoc ExposureGpuKernel::Finish */
    void ExposureGpuKernel::Finish(id<MTLCommandBuffer> commands, bool committed) noexcept {
        for (auto &entry : journal) {
            if (entry.commands != commands)
                continue;
            if (committed)
                entry.accepted = true;
            else
                entry = {};
        }
    }
}  // namespace Horo::Render::Detail
