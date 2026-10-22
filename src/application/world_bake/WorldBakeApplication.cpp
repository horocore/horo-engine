#include "WorldBakeInternal.h"

#include <mutex>

namespace Horo::Application {
    namespace WorldBakeErrors {
        namespace {
            /** @brief Declares inert application error metadata without registering or invoking any service. */
            ErrorCodeDescriptor Descriptor(const char *code, const char *message) {
                return {.domain = ErrorDomainId{"horo.world-bake"},
                        .code = ErrorCode{code},
                        .defaultSeverity = ErrorSeverity::Error,
                        .summary = message,
                        .retryable = true,
                        .userActionable = true};
            }
        }  // namespace

        const ErrorCodeDescriptor Invalid = Descriptor("world.bake.invalid", "The world bake capture is invalid.");
        const ErrorCodeDescriptor Capacity = Descriptor("world.bake.capacity", "The complete world bake exceeds its bounded capacity.");
        const ErrorCodeDescriptor MissingPage = Descriptor("world.bake.missing-page", "An exact source page is not loaded.");
        const ErrorCodeDescriptor Dependency =
            Descriptor("world.bake.dependency", "A required verified project dependency is unavailable.");
        const ErrorCodeDescriptor Unsupported =
            Descriptor("world.bake.unsupported", "The captured content requires an unavailable cook provider.");
        const ErrorCodeDescriptor Conflict = Descriptor("world.bake.conflict", "The complete source identities or placement conflict.");
        const ErrorCodeDescriptor Stale = Descriptor("world.bake.stale", "The world or project capture was superseded before publication.");
        const ErrorCodeDescriptor Cancelled = Descriptor("world.bake.cancelled", "The unpublished world bake was cancelled.");
        const ErrorCodeDescriptor Closed =
            Descriptor("world.bake.closed", "The world bake application is closed or called from another thread.");
    }  // namespace WorldBakeErrors

    namespace WorldBakeDetail {
        /** @brief Worker owns request and writes result under mutex; host reads only after terminal job observation.
         * Cancellation is thread-safe; all ID/facade fields are host-thread-only. No callback captures the facade or host stack.
         */
        struct Attempt final {
            WorldBakeRequest request;
            CancellationSource cancellation;
            OperationId operation{};
            BuildOutputSessionId session;
            std::mutex mutex;
            std::shared_ptr<const WorldBakeOutput> output;
            std::optional<Error> error;
            bool finished{};
            bool sealed{}; /**< The same mutex linearizes cancellation admission and host-only publication. */
        };
    }  // namespace WorldBakeDetail

    namespace {
        /** @brief Requires the existing project writer reservation for every application request that can read files. */
        Result<void> AdmitApplicationRequest(const WorldBakeRequest &request) {
            if (auto valid = WorldBakeDetail::Admit(request); valid.HasError())
                return valid;
            if ((request.sourceAuthority.files || request.project.dependencySource) && !request.sourceAuthority.reservation)
                return Result<void>::Failure(MakeError(WorldBakeErrors::Dependency));
            return Result<void>::Success();
        }

        /** @brief Checks the exact typed cancellation domain rather than matching arbitrary code text. */
        bool Cancelled(const Error &error) {
            const auto matches = [&](const ErrorCodeDescriptor &descriptor) {
                return error.domain.Value() == descriptor.domain.Value() && error.code.Value() == descriptor.code.Value();
            };
            return matches(WorldBakeErrors::Cancelled) || matches(Runtime::SceneCellPayloadErrors::Cancelled) || IsJobCancelled(error);
        }

        /** @brief Checks source and independent project authority on the host thread before any publication. */
        bool Fresh(const WorldBakeDetail::Attempt &attempt, const Editor::PartitionedWorldSnapshotLease &current,
                   const WorldBakeProjectFence &project) {
            return current && current->fence == attempt.request.document->fence && project == attempt.request.project.fence;
        }

        /** @brief Runs actual production conversion/cook/validation while retaining only owned worker state. */
        Result<void> Execute(const std::shared_ptr<WorldBakeDetail::Attempt> &attempt, const JobExecutionContext &job) {
            (void)job.UpdateProgress("world-bake", 0.0F);
            auto result = CookWorldPages(attempt->request, job.Cancellation(), [&](const float progress) {
                (void)job.UpdateProgress("world-bake", progress);
            });
            std::lock_guard lock(attempt->mutex);
            if (result.HasError()) {
                attempt->error = result.ErrorValue();
                return Cancelled(*attempt->error) ? JobCancelled(*attempt->error) : Result<void>::Failure(*attempt->error);
            }
            attempt->output = std::make_shared<const WorldBakeOutput>(std::move(result).Value());
            (void)job.UpdateProgress("world-bake", 1.0F);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc WorldBakeApplication::WorldBakeApplication */
    WorldBakeApplication::WorldBakeApplication(JobSystem &jobs, OperationStore &operations, BuildOutputStore &output)
        : jobs_(jobs), operations_(operations), output_(output) {}

    /** @copydoc WorldBakeApplication::~WorldBakeApplication */
    WorldBakeApplication::~WorldBakeApplication() {
        Close();
    }

    /** @copydoc WorldBakeApplication::Submit */
    Result<OperationId> WorldBakeApplication::Submit(WorldBakeRequest request) {
        if (closed_ || std::this_thread::get_id() != thread_)
            return Result<OperationId>::Failure(MakeError(WorldBakeErrors::Closed));
        if (auto admitted = AdmitApplicationRequest(request); admitted.HasError())
            return Result<OperationId>::Failure(admitted.ErrorValue());
        auto attempt = std::make_shared<WorldBakeDetail::Attempt>();
        attempt->request = std::move(request);
        const auto session = output_.BeginSession();
        if (!session)
            return Result<OperationId>::Failure(MakeError(WorldBakeErrors::Capacity));
        attempt->session = *session;
        const std::weak_ptr<WorldBakeDetail::Attempt> cancellationOwner = attempt;
        const auto operation =
            operations_.Begin({.kind = attempt->request.mode == WorldBakeMode::Bake ? OperationKind::Cook : OperationKind::Validation,
                               .title = attempt->request.title,
                               .phase = "world-bake",
                               .progress = 0.0F,
                               .cancellable = true,
                               .requestCancel = [cancellationOwner] {
            if (const auto owner = cancellationOwner.lock()) {
                std::lock_guard lock(owner->mutex);
                if (!owner->sealed)
                    owner->cancellation.RequestCancellation();
            }
        }});
        if (!operation)
            return Result<OperationId>::Failure(MakeError(WorldBakeErrors::Capacity));
        attempt->operation = *operation;
        CancelSuperseded();
        pending_ = std::move(attempt);
        StartPending();
        return Result<OperationId>::Success(*operation);
    }

    /** @copydoc WorldBakeApplication::StartPending */
    void WorldBakeApplication::StartPending() {
        if (!pending_ || active_ || closed_)
            return;
        const auto candidate = pending_;
        auto accepted =
            jobs_.SubmitContext({.parentCancellation = candidate->cancellation.Token(),
                                 .operationId = candidate->operation,
                                 .resource = (candidate->request.sourceAuthority.files || candidate->request.project.dependencySource)
                                                 ? JobResource::Io
                                                 : JobResource::Cpu},
                                [attempt = candidate](const JobExecutionContext &job) {
            return Execute(attempt, job);
        });
        if (accepted.HasError()) {
            Finish(pending_, OperationState::Failed, accepted.ErrorValue());
            pending_.reset();
            return;
        }
        active_ = std::move(pending_);
        job_ = std::move(accepted).Value();
    }

    /** @copydoc WorldBakeApplication::Finish */
    void WorldBakeApplication::Finish(const std::shared_ptr<WorldBakeDetail::Attempt> &attempt, const OperationState state,
                                      std::optional<Error> error) {
        if (attempt->finished)
            return;
        (void)operations_.Update(attempt->operation, {.state = state,
                                                      .phase = "world-bake",
                                                      .progress = state == OperationState::Succeeded ? std::optional{1.0F} : std::nullopt,
                                                      .error = std::move(error)});
        attempt->finished = true;
    }

    /** @copydoc WorldBakeApplication::CancelSuperseded */
    void WorldBakeApplication::CancelSuperseded() {
        if (pending_) {
            pending_->cancellation.RequestCancellation();
            Finish(pending_, OperationState::Cancelled, MakeError(WorldBakeErrors::Cancelled));
        }
        if (active_) {
            active_->cancellation.RequestCancellation();
            (void)job_.RequestCancel();
        }
    }

    /** @copydoc WorldBakeApplication::RetirePending */
    void WorldBakeApplication::RetirePending(const Editor::PartitionedWorldSnapshotLease &current, const WorldBakeProjectFence &project) {
        if (!pending_)
            return;
        const bool fresh = Fresh(*pending_, current, project);
        if (fresh && !pending_->cancellation.Token().IsCancellationRequested())
            return;
        Finish(pending_, OperationState::Cancelled, MakeError(fresh ? WorldBakeErrors::Cancelled : WorldBakeErrors::Stale));
        pending_.reset();
    }

    /** @copydoc WorldBakeApplication::PublishFailure */
    void WorldBakeApplication::PublishFailure(const std::shared_ptr<WorldBakeDetail::Attempt> &attempt, const JobSnapshot &snapshot) {
        auto error = attempt->error ? attempt->error : snapshot.error;
        if (!error)
            error = MakeError(WorldBakeErrors::Invalid);
        attempt->sealed = true;
        BuildOutputRecord diagnostic{.sessionId = attempt->session,
                                     .operationId = attempt->operation,
                                     .severity = DiagnosticSeverity::Error,
                                     .result = BuildOutputResult::Failed,
                                     .stage = "world-bake",
                                     .code = DiagnosticCode{error->code.Value()},
                                     .message = error->message};
        if (!error->diagnostics.empty() && !error->diagnostics.back().location.source.empty()) {
            const auto &source = error->diagnostics.back().location;
            diagnostic.source = DiagnosticSourceLocation{source.source, source.line, source.column};
        } else if (!attempt->request.sources.empty())
            diagnostic.source = attempt->request.sources.front().source;
        output_.Append(std::move(diagnostic));
        Finish(attempt, OperationState::Failed, std::move(error));
    }

    /** @copydoc WorldBakeApplication::PublishResult */
    void WorldBakeApplication::PublishResult(const std::shared_ptr<WorldBakeDetail::Attempt> &attempt) {
        attempt->sealed = true;
        for (auto diagnostic : attempt->output->validation.diagnostics) {
            diagnostic.sessionId = attempt->session;
            diagnostic.operationId = attempt->operation;
            output_.Append(std::move(diagnostic));
        }
        const bool succeeded = attempt->output->validation.CanPublish();
        if (succeeded && attempt->request.mode == WorldBakeMode::Bake)
            published_ = attempt->output;
        output_.Append({.sessionId = attempt->session,
                        .operationId = attempt->operation,
                        .severity = succeeded ? DiagnosticSeverity::Note : DiagnosticSeverity::Error,
                        .result = succeeded ? BuildOutputResult::Succeeded : BuildOutputResult::Failed,
                        .stage = "world-bake",
                        .code = DiagnosticCode{"world.bake.complete"},
                        .message = attempt->request.title});
        Finish(attempt, succeeded ? OperationState::Succeeded : OperationState::Failed);
    }

    /** @copydoc WorldBakeApplication::Complete */
    void WorldBakeApplication::Complete(const std::shared_ptr<WorldBakeDetail::Attempt> &attempt, const JobSnapshot &snapshot,
                                        const bool fresh) {
        std::lock_guard lock(attempt->mutex);
        if (!fresh) {
            Finish(attempt, OperationState::Cancelled, MakeError(WorldBakeErrors::Stale));
            return;
        }
        if (attempt->cancellation.Token().IsCancellationRequested() || snapshot.state == JobState::Cancelled) {
            Finish(attempt, OperationState::Cancelled, MakeError(WorldBakeErrors::Cancelled));
            return;
        }
        if (attempt->error || snapshot.state == JobState::Failed || !attempt->output)
            PublishFailure(attempt, snapshot);
        else
            PublishResult(attempt);
    }

    /** @copydoc WorldBakeApplication::Pump */
    void WorldBakeApplication::Pump(Editor::PartitionedWorldSnapshotLease current, const WorldBakeProjectFence project) {
        if (closed_ || std::this_thread::get_id() != thread_)
            return;
        RetirePending(current, project);
        if (!active_) {
            StartPending();
            return;
        }
        const bool fresh = Fresh(*active_, current, project);
        if (!fresh) {
            active_->cancellation.RequestCancellation();
            (void)job_.RequestCancel();
        }
        const auto snapshot = job_.Snapshot();
        if (!snapshot)
            return;
        if (snapshot->state == JobState::Queued || snapshot->state == JobState::Running) {
            (void)operations_.Update(active_->operation,
                                     {.state = active_->cancellation.Token().IsCancellationRequested() ? OperationState::Cancelling
                                                                                                       : OperationState::Running,
                                      .phase = "world-bake",
                                      .progress = snapshot->progress.value});
            return;
        }
        const auto attempt = std::move(active_);
        Complete(attempt, *snapshot, fresh);
        StartPending();
    }

    /** @copydoc WorldBakeApplication::Close */
    void WorldBakeApplication::Close() noexcept {
        if (closed_)
            return;
        closed_ = true;
        for (const auto &attempt : {active_, pending_})
            if (attempt)
                attempt->cancellation.RequestCancellation();
        try {
            if (active_)
                (void)job_.RequestCancel();
            for (const auto &attempt : {active_, pending_})
                if (attempt)
                    Finish(attempt, OperationState::Cancelled);
        } catch (...) {
            // Cancellation already fenced every publisher. Teardown cannot propagate allocation/history-sink failures.
        }
        pending_.reset();
        active_.reset();
        job_ = JobHandle{};
    }

    /** @copydoc WorldBakeApplication::ActiveOperation */
    std::optional<OperationId> WorldBakeApplication::ActiveOperation() const noexcept {
        if (closed_)
            return std::nullopt;
        if (pending_)
            return pending_->operation;
        if (active_)
            return active_->operation;
        return std::nullopt;
    }

    /** @copydoc WorldBakeApplication::Published */
    std::shared_ptr<const WorldBakeOutput> WorldBakeApplication::Published() const noexcept {
        return published_;
    }
}  // namespace Horo::Application
