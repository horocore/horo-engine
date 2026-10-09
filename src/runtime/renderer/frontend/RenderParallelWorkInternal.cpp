#include "RenderParallelWorkInternal.h"

#include "RenderParallelWorkErrors.h"

#include <new>
#include <utility>

namespace Horo::Render::Detail {
    RenderParallelGraphWorkState::RenderParallelGraphWorkState(std::shared_ptr<IRenderParallelGraphRecording> recording,
                                                               const CancellationToken &cancellation)
        : recording_(std::move(recording)), cancellation_(cancellation) {
        jobs_.reserve(recording_->PassCount());
    }

    RenderParallelGraphWorkState::~RenderParallelGraphWorkState() {
        cancellation_.RequestCancellation();
        recording_->Cancel();
    }

    Result<std::unique_ptr<RenderParallelGraphWorkState>> RenderParallelGraphWorkState::Start(
        JobSystem &jobs, std::shared_ptr<IRenderParallelGraphRecording> recording, const CancellationToken &cancellation) {
        using StartResult = Result<std::unique_ptr<RenderParallelGraphWorkState>>;
        try {
            auto state =
                std::unique_ptr<RenderParallelGraphWorkState>(new RenderParallelGraphWorkState(std::move(recording), cancellation));
            const JobDescriptor descriptor{.parentCancellation = state->cancellation_.Token()};
            for (std::size_t index = 0; index < state->recording_->PassCount(); ++index) {
                if (descriptor.parentCancellation.IsCancellationRequested())
                    return StartResult::Failure(MakeError(ParallelWorkErrors::Cancelled));
                auto job = jobs.SubmitResult(descriptor, [capsule = state->recording_, index](const CancellationToken &token) {
                    return capsule->Record(index, token);
                });
                if (job.HasError())
                    return StartResult::Failure(job.ErrorValue());
                state->jobs_.push_back(std::move(job).Value());
            }
            return StartResult::Success(std::move(state));
        } catch (const std::bad_alloc &) {
            return StartResult::Failure(MakeError(ParallelWorkErrors::CaptureFailed));
        }
    }

    Result<RenderWorkProgress> RenderParallelGraphWorkState::Poll() {
        using PollResult = Result<RenderWorkProgress>;
        if (cancellation_.Token().IsCancellationRequested())
            return PollResult::Failure(MakeError(ParallelWorkErrors::Cancelled));
        for (const auto &job : jobs_) {
            const auto snapshot = job.Snapshot();
            if (!snapshot || !snapshot->terminalResult)
                return PollResult::Success(RenderWorkProgress::Pending);
            if (snapshot->terminalResult->error)
                return PollResult::Failure(*snapshot->terminalResult->error);
        }
        if (cancellation_.Token().IsCancellationRequested())
            return PollResult::Failure(MakeError(ParallelWorkErrors::Cancelled));
        return PollResult::Success(RenderWorkProgress::Ready);
    }

    const std::shared_ptr<IRenderParallelGraphRecording> &RenderParallelGraphWorkState::Recording() const noexcept {
        return recording_;
    }

    RenderParallelWorkState::RenderParallelWorkState(std::shared_ptr<const CapturedRenderFrame> inputs,
                                                     const CancellationToken &parentCancellation)
        : inputs_(std::move(inputs)), commands_(std::make_shared<std::vector<RenderPassDescriptor>>(inputs_->passes.size())),
          cancellation_(parentCancellation) {
        jobs_.reserve(inputs_->passes.size());
    }

    /** @copydoc RenderParallelWorkState::~RenderParallelWorkState */
    RenderParallelWorkState::~RenderParallelWorkState() {
        Cancel();
    }

    /** @copydoc RenderParallelWorkState::Start */
    Result<std::unique_ptr<RenderParallelWorkState>> RenderParallelWorkState::Start(JobSystem &jobs, const FrameToken frame,
                                                                                    const std::span<const RenderPassDescriptor> passes,
                                                                                    const RenderFrameInputLimits &limits,
                                                                                    const CancellationToken &parentCancellation) {
        using StartResult = Result<std::unique_ptr<RenderParallelWorkState>>;
        auto captured = CaptureRenderFrameInputs(frame, passes, limits, parentCancellation);
        if (captured.HasError())
            return StartResult::Failure(captured.ErrorValue());
        try {
            auto work =
                std::unique_ptr<RenderParallelWorkState>(new RenderParallelWorkState(std::move(captured).Value(), parentCancellation));
            const Result<void> admitted = work->Admit(jobs);
            if (admitted.HasError())
                return StartResult::Failure(admitted.ErrorValue());
            return StartResult::Success(std::move(work));
        } catch (const std::bad_alloc &) {
            return StartResult::Failure(MakeError(ParallelWorkErrors::CaptureFailed));
        }
    }

    Result<void> RenderParallelWorkState::Admit(JobSystem &jobs) {
        const JobDescriptor descriptor{.parentCancellation = cancellation_.Token()};
        for (std::size_t index = 0; index < inputs_->passes.size(); ++index) {
            if (descriptor.parentCancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ParallelWorkErrors::Cancelled));
            auto admitted = jobs.SubmitResult(descriptor, [inputs = inputs_, commands = commands_, index](const CancellationToken &token) {
                const CapturedRenderPass &pass = inputs->passes[index];
                const Result<void> prepared = ValidateCapturedRenderPass(pass, token);
                if (prepared.HasError()) {
                    if (token.IsCancellationRequested())
                        return JobCancelled(prepared.ErrorValue());
                    return prepared;
                }
                if (token.IsCancellationRequested())
                    return JobCancelled();
                // Distinct jobs write distinct preallocated elements. Publication occurs only
                // after every durable job record reports Succeeded, never on callback completion order.
                (*commands)[index] = pass.descriptor;
                return Result<void>::Success();
            });
            if (admitted.HasError())
                return Result<void>::Failure(admitted.ErrorValue());
            jobs_.push_back(std::move(admitted).Value());
        }
        return Result<void>::Success();
    }

    /** @copydoc RenderParallelWorkState::Poll */
    Result<RenderWorkProgress> RenderParallelWorkState::Poll() {
        using PollResult = Result<RenderWorkProgress>;
        if (closed_ || cancellation_.Token().IsCancellationRequested()) {
            Cancel();
            return PollResult::Failure(MakeError(ParallelWorkErrors::Cancelled));
        }
        ready_ = false;
        for (const JobHandle &job : jobs_) {
            const std::optional<JobSnapshot> snapshot = job.Snapshot();
            // Do not publish a later failure while an earlier command's outcome is
            // unknown. The observed failure identity must not depend on worker timing.
            if (!snapshot || !snapshot->terminalResult)
                return PollResult::Success(RenderWorkProgress::Pending);
            if (snapshot->terminalResult->error) {
                Cancel();
                return PollResult::Failure(*snapshot->terminalResult->error);
            }
        }
        if (cancellation_.Token().IsCancellationRequested()) {
            Cancel();
            return PollResult::Failure(MakeError(ParallelWorkErrors::Cancelled));
        }
        ready_ = true;
        return PollResult::Success(RenderWorkProgress::Ready);
    }

    /** @copydoc RenderParallelWorkState::Cancel */
    void RenderParallelWorkState::Cancel() noexcept {
        closed_ = true;
        ready_ = false;
        cancellation_.RequestCancellation();
    }

    /** @copydoc RenderParallelWorkState::Commands */
    std::span<const RenderPassDescriptor> RenderParallelWorkState::Commands() const noexcept {
        if (!ready_ || closed_)
            return {};
        return *commands_;
    }

    /** @copydoc RenderParallelWorkState::Frame */
    FrameToken RenderParallelWorkState::Frame() const noexcept {
        return inputs_->frame;
    }
}  // namespace Horo::Render::Detail
