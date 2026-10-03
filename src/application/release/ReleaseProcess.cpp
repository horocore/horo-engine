#include "Horo/Release/ReleaseProcess.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace Horo::Release {
    namespace {
        constexpr std::size_t MaximumReleaseLineBytes = 16U * 1024U;
        constexpr std::size_t MaximumReleaseOutputBytes = 1024U * 1024U;
        constexpr auto MaximumGracefulTermination = std::chrono::seconds{2};
        constexpr auto MaximumDrainDuration = std::chrono::seconds{1};
    }  // namespace

    /** @copydoc ReleaseProcessRunner::ReleaseProcessRunner */
    ReleaseProcessRunner::ReleaseProcessRunner(IExternalProcessRunner &processes) noexcept : processes_(processes) {}

    /** @copydoc ReleaseProcessRunner::Run */
    Result<ExternalProcessResult> ReleaseProcessRunner::Run(const ReleaseStageContext &context, ReleaseProcessRequest request,
                                                            std::function<void(ReleaseProcessOutput)> onOutput) const {
        const auto now = std::chrono::steady_clock::now();
        if (now >= context.deadline)
            return Result<ExternalProcessResult>::Failure(MakeError(ReleaseErrors::PipelineStageTimeout));
        if (context.cancellation.IsCancellationRequested())
            return Result<ExternalProcessResult>::Failure(MakeError(ReleaseErrors::PipelineProcessCancelled));

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(context.deadline - now);
        if (remaining <= std::chrono::milliseconds::zero())
            return Result<ExternalProcessResult>::Failure(MakeError(ReleaseErrors::PipelineStageTimeout));
        ExternalProcessRequest native;
        native.executable = std::move(request.executable);
        native.arguments = std::move(request.arguments);
        native.workingDirectory = std::move(request.workingDirectory);
        native.environment = std::move(request.environment);
        native.timeout = std::min(request.timeout, remaining);
        native.gracefulTermination = std::chrono::duration_cast<std::chrono::milliseconds>(MaximumGracefulTermination);
        native.maximumDrainDuration = std::chrono::duration_cast<std::chrono::milliseconds>(MaximumDrainDuration);
        native.maximumLineBytes = MaximumReleaseLineBytes;
        native.maximumOutputBytes = MaximumReleaseOutputBytes;
        bool observerFailed = false;
        native.onOutput = [context, observer = std::move(onOutput), &observerFailed](ProcessOutputLine line) {
            if (!observer || observerFailed)
                return;
            try {
                observer(
                    ReleaseProcessOutput{context.job, context.target, context.operation, context.stage, context.attempt, std::move(line)});
            } catch (...) {  // NOSONAR: External output observers may throw non-standard exceptions; keep draining safely.
                observerFailed = true;
            }
        };

        auto result = processes_.Run(native, context.cancellation);
        if (result.HasError())
            return result;
        if (result.Value().stopCause == ProcessStopCause::Timeout || result.Value().reason == ProcessTerminationReason::TimedOut)
            return Result<ExternalProcessResult>::Failure(MakeError(ReleaseErrors::PipelineStageTimeout));
        if (result.Value().stopCause == ProcessStopCause::Cancellation || result.Value().reason == ProcessTerminationReason::Cancelled)
            return Result<ExternalProcessResult>::Failure(MakeError(ReleaseErrors::PipelineProcessCancelled));
        if (observerFailed)
            return Result<ExternalProcessResult>::Failure(MakeError(ReleaseErrors::PipelineStageException));
        if (result.Value().reason != ProcessTerminationReason::Exited || result.Value().exitCode != 0)
            return Result<ExternalProcessResult>::Failure(MakeError(ReleaseErrors::PipelineProcessFailed));
        return result;
    }
}  // namespace Horo::Release
