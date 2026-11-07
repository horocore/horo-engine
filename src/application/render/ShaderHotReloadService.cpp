#include "ShaderHotReloadInternal.h"

#include <limits>

namespace Horo::Application {
    namespace ShaderHotReloadErrors {
        namespace {
            const ErrorDomainId Domain{"application.shader_hot_reload"};
        }

        const ErrorCodeDescriptor Disabled{Domain, ErrorCode{"application.shader_hot_reload.disabled"}, ErrorSeverity::Warning,
                                           "Shader hot reload is disabled.", "Select an explicit development policy."};
        const ErrorCodeDescriptor Packaged{Domain, ErrorCode{"application.shader_hot_reload.packaged"}, ErrorSeverity::Error,
                                           "Packaged profiles cannot compile shader sources.", "Use admitted cooked variants."};
        const ErrorCodeDescriptor WrongThread{Domain, ErrorCode{"application.shader_hot_reload.wrong_thread"}, ErrorSeverity::Error,
                                              "Shader reload control requires its graphics owner.", "Dispatch at the owner safe point."};
        const ErrorCodeDescriptor Busy{Domain, ErrorCode{"application.shader_hot_reload.busy"}, ErrorSeverity::Warning,
                                       "Shader reload control cannot reenter.", "Retry at the next owner safe point."};
        const ErrorCodeDescriptor Closed{Domain, ErrorCode{"application.shader_hot_reload.closed"}, ErrorSeverity::Warning,
                                         "Shader reload admission has closed.", "Use a live host session."};
        const ErrorCodeDescriptor InvalidRequest{Domain, ErrorCode{"application.shader_hot_reload.invalid_request"}, ErrorSeverity::Error,
                                                 "The shader reload request is invalid.",
                                                 "Provide a complete bounded canonical program set."};
        const ErrorCodeDescriptor Capacity{Domain, ErrorCode{"application.shader_hot_reload.capacity"}, ErrorSeverity::Warning,
                                           "Shader reload exceeds its aggregate budget.", "Reduce the complete input/output envelope."};
        const ErrorCodeDescriptor Stale{Domain, ErrorCode{"application.shader_hot_reload.stale"}, ErrorSeverity::Warning,
                                        "The shader reload capture is stale.",
                                        "Recapture the current project, source, configuration and device."};
        const ErrorCodeDescriptor TargetFailure{Domain, ErrorCode{"application.shader_hot_reload.target_failure"}, ErrorSeverity::Error,
                                                "The shader target failed its replacement contract.",
                                                "Keep the last good generation and inspect diagnostics."};
    }  // namespace ShaderHotReloadErrors

    namespace {
        struct ControlGuard final {
            bool &busy;

            explicit ControlGuard(bool &flag) : busy(flag) {
                busy = true;
            }

            ~ControlGuard() {
                busy = false;
            }
        };

        Result<Render::ShaderHotReloadTargetState> Capture(Render::IShaderHotReloadTarget &target) {
            try {
                return target.CaptureState();
            } catch (...) {
                return Result<Render::ShaderHotReloadTargetState>::Failure(MakeError(ShaderHotReloadErrors::TargetFailure));
            }
        }

        bool ValidTarget(Render::ShaderHotReloadTargetState state, const ShaderHotReloadFence &fence) noexcept {
            return state.deviceIncarnation == fence.deviceIncarnation && state.liveGeneration != 0;
        }
    }  // namespace

    bool ShaderHotReloadFence::IsValid() const noexcept {
        return projectGeneration != 0 && configurationRevision != 0 && lifecycleGeneration != 0 && sourceGeneration != 0 &&
               deviceIncarnation != 0;
    }

    Result<void> ShaderHotReloadService::State::RequireOwner() const {
        if (std::this_thread::get_id() != owner)
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::WrongThread));
        if (busy)
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Busy));
        return Result<void>::Success();
    }

    Result<std::unique_ptr<ShaderHotReloadService>> ShaderHotReloadService::Create(ShaderBuildService &compiler, const JobSystem &jobs,
                                                                                   OperationStore &operations, BuildOutputStore &output,
                                                                                   Render::IShaderHotReloadTarget &target,
                                                                                   const ShaderHotReloadConfiguration &configuration) {
        using Created = Result<std::unique_ptr<ShaderHotReloadService>>;
        if (auto valid = ShaderHotReloadDetail::ValidateConfiguration(configuration); valid.HasError())
            return Created::Failure(valid.ErrorValue());
        auto captured = Capture(target);
        if (captured.HasError())
            return Created::Failure(captured.ErrorValue());
        if (!ValidTarget(captured.Value(), configuration.context) || jobs.WorkerCount() == 0)
            return Created::Failure(MakeError(ShaderHotReloadErrors::InvalidRequest));
        try {
            auto state = std::make_unique<State>(compiler, jobs, operations, output, target, configuration);
            return Created::Success(std::unique_ptr<ShaderHotReloadService>(new ShaderHotReloadService(std::move(state))));
        } catch (const std::bad_alloc &) {
            return Created::Failure(MakeError(Render::ShaderCompilerPipelineErrors::AllocationFailed));
        }
    }

    ShaderHotReloadService::ShaderHotReloadService(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    ShaderHotReloadService::~ShaderHotReloadService() {
        try {
            static_cast<void>(Shutdown());
        } catch (...) { /* Join remains mandatory even if diagnostic publication throws. */
        }
        state_->Join();
    }

    void ShaderHotReloadService::State::Join() noexcept {
        if (active)
            active->cancellation.RequestCancellation();
        if (pending)
            pending->cancellation.RequestCancellation();
        if (!job)
            return;
        try {
            static_cast<void>(job->RequestCancel());
        } catch (...) {
        }
        // No worker retains the service or target; borrowed compiler lifetime is fenced by this join.
        try {
            static_cast<void>(job->Wait());
        } catch (...) {
            std::terminate();
        }
    }

    void ShaderHotReloadService::State::Refresh() {
        if (snapshot.revision != std::numeric_limits<std::uint64_t>::max())
            ++snapshot.revision;
        snapshot.activeOperation = active ? std::optional{active->operation} : std::nullopt;
        snapshot.pendingOperation = pending ? std::optional{pending->operation} : std::nullopt;
        snapshot.activeJob = job ? std::optional{job->Id()} : std::nullopt;
    }

    void ShaderHotReloadService::State::CancelActive(bool obsolete) {
        if (!active)
            return;
        active->obsolete = active->obsolete || obsolete;
        active->cancellation.RequestCancellation();
        if (job)
            static_cast<void>(job->RequestCancel());
        try {
            static_cast<void>(operations.Update(active->operation, {.state = OperationState::Cancelling, .phase = "cancel"}));
        } catch (...) {
        }
    }

    void ShaderHotReloadService::State::DiscardPending(ShaderHotReloadOutcome outcome) {
        if (!pending)
            return;
        auto discarded = std::move(pending);
        discarded->cancellation.RequestCancellation();
        Finish(discarded, outcome,
               outcome == ShaderHotReloadOutcome::Stale ? MakeError(ShaderHotReloadErrors::Stale)
                                                        : ShaderHotReloadDetail::CancelledError());
    }

    Result<void> ShaderHotReloadService::SetContext(const ShaderHotReloadFence &context) {
        if (auto owner = state_->RequireOwner(); owner.HasError())
            return owner;
        if (state_->snapshot.closed)
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Closed));
        if (!context.IsValid())
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::InvalidRequest));
        if (context == state_->configuration.context)
            return Result<void>::Success();
        ControlGuard guard(state_->busy);
        state_->configuration.context = context;
        state_->snapshot.context = context;
        state_->CancelActive(true);
        state_->DiscardPending(ShaderHotReloadOutcome::Stale);
        state_->Refresh();
        return Result<void>::Success();
    }

    Result<std::shared_ptr<ShaderHotReloadDetail::Work>> ShaderHotReloadService::State::PrepareRequest(
        const ShaderHotReloadRequest &request, const CancellationToken &cancellation) {
        using Prepared = Result<std::shared_ptr<ShaderHotReloadDetail::Work>>;
        auto captured = Capture(target);
        if (captured.HasError())
            return Prepared::Failure(captured.ErrorValue());
        if (auto valid = ShaderHotReloadDetail::ValidateInput(request, configuration, captured.Value()); valid.HasError())
            return Prepared::Failure(valid.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Prepared::Failure(ShaderHotReloadDetail::CancelledError());
        if (generation == std::numeric_limits<std::uint64_t>::max())
            return Prepared::Failure(MakeError(ShaderHotReloadErrors::Capacity));
        try {
            return Prepared::Success(std::make_shared<ShaderHotReloadDetail::Work>(request, captured.Value(), cancellation));
        } catch (const std::bad_alloc &) {
            return Prepared::Failure(MakeError(Render::ShaderCompilerPipelineErrors::AllocationFailed));
        }
    }

    Result<void> ShaderHotReloadService::State::RegisterRequest(const std::shared_ptr<ShaderHotReloadDetail::Work> &work) {
        const std::weak_ptr weak = work;
        const auto operation = operations.Begin({.kind = OperationKind::Build,
                                                 .title = "Reload shader program set",
                                                 .phase = "queued",
                                                 .cancellable = true,
                                                 .requestCancel = [weak] {
            if (auto current = weak.lock())
                current->cancellation.RequestCancellation();
        }});
        if (!operation)
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Capacity));
        work->operation = *operation;
        const auto session = output.BeginSession();
        if (!session) {
            static_cast<void>(
                operations.Update(*operation, {.state = OperationState::Failed, .error = MakeError(ShaderHotReloadErrors::Capacity)}));
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Capacity));
        }
        work->outputSession = *session;
        work->generation = ++generation;
        for (auto &program : work->request.programs)
            program.build.operationId = *operation;
        return Result<void>::Success();
    }

    Result<ShaderHotReloadAdmission> ShaderHotReloadService::Request(const ShaderHotReloadRequest &request,
                                                                     const CancellationToken &cancellation) {
        using Admission = Result<ShaderHotReloadAdmission>;
        if (auto owner = state_->RequireOwner(); owner.HasError())
            return Admission::Failure(owner.ErrorValue());
        if (state_->snapshot.closed)
            return Admission::Failure(MakeError(ShaderHotReloadErrors::Closed));
        ControlGuard guard(state_->busy);
        auto prepared = state_->PrepareRequest(request, cancellation);
        if (prepared.HasError())
            return Admission::Failure(prepared.ErrorValue());
        const auto work = std::move(prepared).Value();
        if (auto registered = state_->RegisterRequest(work); registered.HasError())
            return Admission::Failure(registered.ErrorValue());
        state_->CancelActive(true);
        state_->DiscardPending(ShaderHotReloadOutcome::Stale);
        state_->pending = work;
        state_->Refresh();
        static_cast<void>(state_->StartPending());
        return Admission::Success({work->operation, work->generation});
    }

    Result<void> ShaderHotReloadService::Poll() {
        if (auto owner = state_->RequireOwner(); owner.HasError())
            return owner;
        if (state_->snapshot.closed)
            return Result<void>::Failure(MakeError(ShaderHotReloadErrors::Closed));
        ControlGuard guard(state_->busy);
        if (auto completed = state_->ProcessCompletion(); completed.HasError())
            return completed;
        return state_->StartPending();
    }

    Result<void> ShaderHotReloadService::Cancel(OperationId operation) {
        if (auto owner = state_->RequireOwner(); owner.HasError())
            return owner;
        ControlGuard guard(state_->busy);
        if (state_->pending && state_->pending->operation == operation) {
            state_->DiscardPending(ShaderHotReloadOutcome::Cancelled);
            state_->Refresh();
            return Result<void>::Success();
        }
        if (state_->active && state_->active->operation == operation) {
            state_->CancelActive(false);
            state_->Refresh();
            return Result<void>::Success();
        }
        return Result<void>::Failure(MakeError(ShaderHotReloadErrors::InvalidRequest));
    }

    Result<std::optional<ShaderHotReloadSnapshot>> ShaderHotReloadService::SnapshotIfChanged(std::uint64_t known) const {
        using Snapshot = Result<std::optional<ShaderHotReloadSnapshot>>;
        if (auto owner = state_->RequireOwner(); owner.HasError())
            return Snapshot::Failure(owner.ErrorValue());
        if (known == state_->snapshot.revision)
            return Snapshot::Success(std::nullopt);
        return Snapshot::Success(state_->snapshot);
    }

    Result<void> ShaderHotReloadService::Shutdown() {
        if (auto owner = state_->RequireOwner(); owner.HasError())
            return owner;
        ControlGuard guard(state_->busy);
        state_->snapshot.closed = true;
        state_->CancelActive(false);
        state_->DiscardPending(ShaderHotReloadOutcome::Cancelled);
        state_->Join();  // Explicit teardown, never Poll or ordinary frame execution.
        if (state_->active)
            state_->Finish(state_->active, ShaderHotReloadOutcome::Cancelled, ShaderHotReloadDetail::CancelledError());
        state_->active.reset();
        state_->job.reset();
        state_->Refresh();
        return Result<void>::Success();
    }
}  // namespace Horo::Application
