#include "ShaderHotReloadInternal.h"

namespace Horo::Application::ShaderHotReloadDetail {
    Error CancelledError() {
        return JobCancelled().ErrorValue();
    }

    bool Terminal(const JobSnapshot &snapshot) noexcept {
        return snapshot.terminalResult.has_value();
    }

    Result<void> CompileWork(const std::shared_ptr<Work> &work, ShaderBuildService &compiler, const CancellationToken &cancellation) {
        CompiledSet compiled;
        compiled.reserve(work->request.programs.size());
        for (const auto &program : work->request.programs) {
            auto result = compiler.Compile(program.build, cancellation);
            if (result.HasError()) {
                const auto error = result.ErrorValue();
                std::lock_guard lock(work->mutex);
                work->completed.emplace(Result<CompiledSet>::Failure(error));
                return Result<void>::Failure(error);
            }
            compiled.push_back(std::move(result).Value());
        }
        std::lock_guard lock(work->mutex);
        work->completed.emplace(Result<CompiledSet>::Success(std::move(compiled)));
        return Result<void>::Success();
    }
}  // namespace Horo::Application::ShaderHotReloadDetail

namespace Horo::Application {
    Result<void> ShaderHotReloadService::State::StartPending() {
        if (active || !pending)
            return Result<void>::Success();
        active = std::move(pending);
        const auto work = active;
        auto submitted = jobs.SubmitResult({.parentCancellation = work->cancellation.Token(),
                                            .operationId = work->operation,
                                            .priority = JobPriority::Interactive},
                                           [work, builds = &compiler](const CancellationToken &token) {
            return ShaderHotReloadDetail::CompileWork(work, *builds, token);
        });
        if (submitted.HasError()) {
            Finish(work, ShaderHotReloadOutcome::Failed, submitted.ErrorValue());
            active.reset();
            Refresh();
            return Result<void>::Success();  // Admission succeeded; its terminal receipt owns scheduling failure.
        }
        job.emplace(std::move(submitted).Value());
        work->job = job->Id();
        static_cast<void>(operations.Update(work->operation, {.state = OperationState::Running, .phase = "compile"}));
        Refresh();
        return Result<void>::Success();
    }
}  // namespace Horo::Application
