#include "ShaderHotReloadInternal.h"

namespace Horo::Application {
    namespace {
        OperationState OperationOutcome(ShaderHotReloadOutcome outcome) noexcept {
            if (outcome == ShaderHotReloadOutcome::Adopted)
                return OperationState::Succeeded;
            if (outcome == ShaderHotReloadOutcome::Failed)
                return OperationState::Failed;
            return OperationState::Cancelled;
        }

        BuildOutputResult OutputOutcome(ShaderHotReloadOutcome outcome) noexcept {
            if (outcome == ShaderHotReloadOutcome::Adopted)
                return BuildOutputResult::Succeeded;
            if (outcome == ShaderHotReloadOutcome::Failed)
                return BuildOutputResult::Failed;
            return BuildOutputResult::Cancelled;
        }

        Result<void> Current(Render::IShaderHotReloadTarget &target, Render::ShaderHotReloadTargetState expected) {
            try {
                auto current = target.CaptureState();
                if (current.HasError())
                    return Result<void>::Failure(current.ErrorValue());
                if (current.Value() != expected)
                    return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Stale));
                return Result<void>::Success();
            } catch (...) {
                return Result<void>::Failure(MakeError(ShaderHotReloadErrors::TargetFailure));
            }
        }

        Result<std::unique_ptr<Render::IShaderHotReloadCandidate>> Prepare(Render::IShaderHotReloadTarget &target,
                                                                           std::span<const Render::ShaderHotReloadProgramView> programs,
                                                                           Render::ShaderHotReloadTargetState expected,
                                                                           const CancellationToken &cancellation) {
            try {
                return target.Prepare(programs, expected, cancellation);
            } catch (...) {
                return Result<std::unique_ptr<Render::IShaderHotReloadCandidate>>::Failure(MakeError(ShaderHotReloadErrors::TargetFailure));
            }
        }

        Result<Render::ShaderHotReloadTargetState> Publish(Render::IShaderHotReloadTarget &target,
                                                           std::unique_ptr<Render::IShaderHotReloadCandidate> candidate,
                                                           Render::ShaderHotReloadTargetState expected) {
            try {
                return target.Adopt(std::move(candidate), expected);
            } catch (...) {
                return Result<Render::ShaderHotReloadTargetState>::Failure(MakeError(ShaderHotReloadErrors::TargetFailure));
            }
        }
    }  // namespace

    void ShaderHotReloadService::State::Finish(const std::shared_ptr<ShaderHotReloadDetail::Work> &work, ShaderHotReloadOutcome outcome,
                                               std::optional<Error> error, std::optional<Render::ShaderHotReloadTargetState> adopted) {
        if (work->finished)
            return;
        ShaderHotReloadReceipt receipt{work->operation, work->job, work->generation, work->request.fence,
                                       outcome,         adopted,   std::move(error)};
        snapshot.lastResult = std::move(receipt);
        if (outcome == ShaderHotReloadOutcome::Adopted)
            snapshot.lastGood = snapshot.lastResult;
        work->finished = true;
        // Diagnostic retention cannot change target adoption or its authoritative terminal receipt.
        try {
            const auto &failure = snapshot.lastResult->error;
            static_cast<void>(
                operations.Update(work->operation, {.state = OperationOutcome(outcome), .phase = "shader-reload", .error = failure}));
            output.Append({.sessionId = work->outputSession,
                           .operationId = work->operation,
                           .severity = failure ? DiagnosticSeverity::Warning : DiagnosticSeverity::Note,
                           .result = OutputOutcome(outcome),
                           .stage = "shader-reload",
                           .code = DiagnosticCode{failure ? std::string(failure->code.Value()) : "application.shader_hot_reload.adopted"},
                           .message = failure ? failure->message : "Complete shader program generation adopted."});
        } catch (...) { /* Existing bounded diagnostic stores remain best effort after authoritative publication. */
        }
        Refresh();
    }

    Result<void> ShaderHotReloadService::State::Adopt(const std::shared_ptr<ShaderHotReloadDetail::Work> &work,
                                                      const ShaderHotReloadDetail::CompiledSet &compiled) {
        if (auto current = Current(target, work->target); current.HasError()) {
            Finish(work, ShaderHotReloadOutcome::Stale, current.ErrorValue());
            return Result<void>::Success();
        }
        std::array<Render::ShaderHotReloadProgramView, ShaderHotReloadDetail::MaximumPrograms> programs{};
        for (std::size_t index = 0; index < compiled.size(); ++index) {
            const auto &program = work->request.programs[index];
            programs[index] = {program.id, &program.build.compilation.manifest, &compiled[index]};
        }
        auto candidate = Prepare(target, std::span(programs).first(compiled.size()), work->target, work->cancellation.Token());
        if (candidate.HasError()) {
            Finish(work, ShaderHotReloadOutcome::Failed, candidate.ErrorValue());
            return Result<void>::Success();
        }
        if (work->cancellation.Token().IsCancellationRequested()) {
            Finish(work, ShaderHotReloadOutcome::Cancelled, ShaderHotReloadDetail::CancelledError());
            return Result<void>::Success();
        }
        auto adopted = Publish(target, std::move(candidate).Value(), work->target);
        if (adopted.HasError()) {
            Finish(work, ShaderHotReloadOutcome::Failed, adopted.ErrorValue());
            return Result<void>::Success();
        }
        Finish(work, ShaderHotReloadOutcome::Adopted, {}, adopted.Value());
        return Result<void>::Success();
    }

    void ShaderHotReloadService::State::ConsumeCompletion(const std::shared_ptr<ShaderHotReloadDetail::Work> &work,
                                                          const JobSnapshot &status) {
        std::lock_guard lock(work->mutex);
        if (!work->completed)
            Finish(work, ShaderHotReloadOutcome::Failed, status.error.value_or(MakeError(ShaderHotReloadErrors::TargetFailure)));
        else if (work->completed->HasError())
            Finish(work, ShaderHotReloadOutcome::Failed, work->completed->ErrorValue());
        else
            static_cast<void>(Adopt(work, work->completed->Value()));
    }

    Result<void> ShaderHotReloadService::State::ProcessCompletion() {
        if (!active || !job)
            return Result<void>::Success();
        const auto status = job->Snapshot();
        if (!status || !ShaderHotReloadDetail::Terminal(*status))
            return Result<void>::Success();
        const auto work = active;
        if (work->obsolete || work->request.fence != configuration.context)
            Finish(work, ShaderHotReloadOutcome::Stale, MakeError(ShaderHotReloadErrors::Stale));
        else if (work->cancellation.Token().IsCancellationRequested())
            Finish(work, ShaderHotReloadOutcome::Cancelled, ShaderHotReloadDetail::CancelledError());
        else
            ConsumeCompletion(work, *status);
        active.reset();
        job.reset();
        Refresh();
        return Result<void>::Success();
    }
}  // namespace Horo::Application
