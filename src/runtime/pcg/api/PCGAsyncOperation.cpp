#include "Horo/PCG/PCGAsyncOperation.h"

#include "Horo/PCG/PCGErrors.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::PCG {
    namespace {
        [[nodiscard]] bool SameScope(const PCGAsyncFence &left, const PCGAsyncFence &right) noexcept {
            return left.scene == right.scene && left.cell == right.cell && left.graph.graph == right.graph.graph;
        }

        [[nodiscard]] bool AdvancesFence(const PCGAsyncFence &before, const PCGAsyncFence &after) noexcept {
            if (!SameScope(before, after) || after.graph.revision < before.graph.revision ||
                after.runtimeGeneration < before.runtimeGeneration)
                return false;
            if (after.graph.revision == before.graph.revision && after.sourceDigest != before.sourceDigest)
                return false;
            if (after.runtimeGeneration != before.runtimeGeneration)
                return true;
            return after.planGeneration >= before.planGeneration && after.inputGeneration >= before.inputGeneration &&
                   after.authorityGeneration >= before.authorityGeneration;
        }

        [[nodiscard]] bool WorkerTerminal(const JobHandle &job) {
            const auto snapshot = job.Snapshot();
            return snapshot.has_value() && snapshot->terminalResult.has_value();
        }
    }  // namespace

    PCGAsyncPrepareContext::PCGAsyncPrepareContext(const JobExecutionContext &job, TaskGroup &children,
                                                   std::atomic_size_t &accepted) noexcept
        : job_(job), children_(children), accepted_(accepted) {}

    /** @copydoc PCGAsyncPrepareContext::Cancellation */
    const CancellationToken &PCGAsyncPrepareContext::Cancellation() const noexcept {
        return job_.Cancellation();
    }

    /** @copydoc PCGAsyncPrepareContext::UpdateProgress */
    Result<void> PCGAsyncPrepareContext::UpdateProgress(const std::string_view phase, const std::optional<float> value) const {
        return job_.UpdateProgress(phase, value);
    }

    /** @copydoc PCGAsyncPrepareContext::SpawnChild */
    Result<JobId> PCGAsyncPrepareContext::SpawnChild(JobFunction work) {
        if (!work)
            return Result<JobId>::Failure(MakeError(PCGErrors::AsyncInvalid));
        JobDescriptor descriptor{.operationId = job_.Operation(), .configuration = job_.Configuration()};
        auto result = children_.Spawn(std::move(descriptor), std::move(work));
        if (result.HasValue())
            accepted_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    /** @brief Runs pure preparation and drains every accepted child before parent completion. */
    Result<void> PCGAsyncOperations::RunPreparation(const JobExecutionContext &job, JobSystem &scheduler,
                                                    const std::function<Result<void>(PCGAsyncPrepareContext &)> &prepare,
                                                    std::atomic_size_t &acceptedChildren, const Duration childJoinTimeout) {
        if (job.Cancellation().IsCancellationRequested())
            return JobCancelled();
        TaskGroup children(scheduler, TaskGroupFailurePolicy::FailFast, job.Cancellation());
        PCGAsyncPrepareContext adapter(job, children, acceptedChildren);
        auto prepared = prepare(adapter);
        if (prepared.HasError())
            children.RequestCancel();
        const auto joined = children.Join({WaitPolicy::WorkerOnly, childJoinTimeout});
        if (joined.HasError() && !children.Outcome().has_value()) {
            const Error interruption = joined.ErrorValue();
            children.RequestCancel();
            const auto drained = children.Join();  // Lifetime fallback: accepted children must be accounted for.
            if (drained.HasError() && !IsJobCancelled(drained.ErrorValue()) &&
                (!prepared.HasError() || IsJobCancelled(prepared.ErrorValue())))
                return drained;
            if (prepared.HasError())
                return prepared;
            return Result<void>::Failure(interruption);
        }
        if (prepared.HasError() && IsJobCancelled(prepared.ErrorValue()) && joined.HasError() && !IsJobCancelled(joined.ErrorValue()))
            return joined;
        if (prepared.HasError())
            return prepared;
        if (joined.HasError())
            return joined;
        if (job.Cancellation().IsCancellationRequested())
            return JobCancelled();
        return Result<void>::Success();
    }

    struct PCGAsyncOperations::Impl final {
        struct Scope final {
            PCGAsyncFence fence;
            bool open{true};
        };

        struct Record final {
            PCGAsyncSnapshot snapshot;
            CancellationSource cancellation;
            JobHandle job;
            std::shared_ptr<std::atomic_size_t> acceptedChildren{std::make_shared<std::atomic_size_t>(0)};
            std::function<Result<void>(const CancellationToken &)> publish;
            std::uint64_t workUnits{};

            explicit Record(const CancellationToken &parent) : cancellation(parent) {}
        };

        JobSystem &jobs;
        const std::thread::id ownerThread{std::this_thread::get_id()};
        PCGAsyncLimits limits;
        std::vector<Scope> scopes;
        std::vector<std::unique_ptr<Record>> records;
        std::uint64_t nextId{1};
        bool accepting{true};
        bool publishing{false};

        Impl(JobSystem &scheduler, const PCGAsyncLimits bounds) : jobs(scheduler), limits(bounds) {
            scopes.reserve(limits.maximumScopes);
            records.reserve(limits.maximumTracked);
        }

        [[nodiscard]] bool OnOwnerThread() const noexcept {
            return std::this_thread::get_id() == ownerThread;
        }

        [[nodiscard]] Scope *FindScope(const PCGAsyncFence &fence) noexcept {
            const auto found = std::ranges::find_if(scopes, [&fence](const Scope &scope) {
                return SameScope(scope.fence, fence);
            });
            return found == scopes.end() ? nullptr : &*found;
        }

        [[nodiscard]] const Scope *FindScope(const PCGAsyncFence &fence) const noexcept {
            const auto found = std::ranges::find_if(scopes, [&fence](const Scope &scope) {
                return SameScope(scope.fence, fence);
            });
            return found == scopes.end() ? nullptr : &*found;
        }

        [[nodiscard]] Record *FindRecord(const PCGAsyncOperationId id) const noexcept {
            const auto found = std::ranges::find_if(records, [id](const std::unique_ptr<Record> &record) {
                return record->snapshot.id == id;
            });
            return found == records.end() ? nullptr : found->get();
        }

        /** @brief Validates exact scope, capacity and cancellation before creating an accepted job. */
        [[nodiscard]] Result<void> ValidateSubmission(const PCGAsyncKind kind, const PCGAsyncRequest &request) const {
            if (!OnOwnerThread())
                return Result<void>::Failure(MakeError(PCGErrors::AsyncWrongThread));
            if (publishing)
                return Result<void>::Failure(MakeError(PCGErrors::AsyncNotReady));
            if (!accepting)
                return Result<void>::Failure(MakeError(PCGErrors::AsyncClosed));
            if (kind >= PCGAsyncKind::Count || !request.fence.IsValid() || request.workUnits == 0 ||
                request.workUnits > limits.maximumWorkUnits || !request.prepare || !request.publish ||
                (kind == PCGAsyncKind::Evaluate && request.fence.planGeneration == 0))
                return Result<void>::Failure(MakeError(PCGErrors::AsyncInvalid));
            const Scope *const scope = FindScope(request.fence);
            if (scope == nullptr)
                return Result<void>::Failure(MakeError(PCGErrors::AsyncUnknown));
            if (!scope->open)
                return Result<void>::Failure(MakeError(PCGErrors::AsyncClosed));
            if (scope->fence != request.fence)
                return Result<void>::Failure(MakeError(PCGErrors::AsyncStale));
            if (request.parentCancellation.IsCancellationRequested())
                return Result<void>::Failure(JobCancelled().ErrorValue());
            if (jobs.WorkerCount() < 2 || records.size() >= limits.maximumTracked)
                return Result<void>::Failure(MakeError(PCGErrors::AsyncCapacityExceeded));
            if (nextId == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(PCGErrors::AsyncGenerationExhausted));
            return Result<void>::Success();
        }

        void Cancel(Record &record) const {
            if (record.snapshot.terminal.has_value())
                return;
            record.cancellation.RequestCancellation();
            static_cast<void>(record.job.RequestCancel());
        }

        void Finish(Record &record, const PCGAsyncState state, std::optional<Error> error = std::nullopt) const {
            record.snapshot.state = state;
            record.snapshot.terminal =
                PCGAsyncTerminalResult{state, std::move(error), record.acceptedChildren->load(std::memory_order_relaxed), record.workUnits};
            record.publish = {};
        }

        /** @brief Invokes only an immutable-candidate owner callback after the exact fence check. */
        void Publish(Record &record) {
            record.snapshot.state = PCGAsyncState::CandidateReady;
            publishing = true;
            try {
                const auto published = record.publish(record.cancellation.Token());
                if (published.HasError()) {
                    const bool cancelled = IsJobCancelled(published.ErrorValue());
                    Finish(record, cancelled ? PCGAsyncState::Cancelled : PCGAsyncState::Failed, published.ErrorValue());
                } else {
                    Finish(record, PCGAsyncState::Succeeded);
                }
            } catch (...) {  // User callbacks may throw non-std exceptions; no exception may escape the owner boundary.
                Finish(record, PCGAsyncState::Failed, MakeError(PCGErrors::AsyncPublicationFailed));
            }
            publishing = false;
        }
    };

    PCGAsyncOperations::PCGAsyncOperations(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    PCGAsyncOperations::~PCGAsyncOperations() {
        if (impl_ && impl_->OnOwnerThread())
            static_cast<void>(BeginShutdown());
    }

    /** @copydoc PCGAsyncOperations::Create */
    Result<std::unique_ptr<PCGAsyncOperations>> PCGAsyncOperations::Create(JobSystem &jobs, const PCGAsyncLimits limits) {
        if (!limits.IsValid())
            return Result<std::unique_ptr<PCGAsyncOperations>>::Failure(MakeError(PCGErrors::AsyncInvalid));
        auto owner = std::unique_ptr<PCGAsyncOperations>(new PCGAsyncOperations(std::make_unique<Impl>(jobs, limits)));
        return Result<std::unique_ptr<PCGAsyncOperations>>::Success(std::move(owner));
    }

    /** @copydoc PCGAsyncOperations::RegisterFence */
    Result<void> PCGAsyncOperations::RegisterFence(const PCGAsyncFence &fence) {
        if (!impl_->OnOwnerThread())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncWrongThread));
        if (impl_->publishing)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncNotReady));
        if (!impl_->accepting)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncClosed));
        if (!fence.IsValid())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncInvalid));
        if (Impl::Scope *const existing = impl_->FindScope(fence)) {
            if (existing->open)
                return Result<void>::Failure(MakeError(PCGErrors::AsyncInvalid));
            if (!AdvancesFence(existing->fence, fence) || existing->fence == fence)
                return Result<void>::Failure(MakeError(PCGErrors::AsyncStale));
            if (std::ranges::any_of(impl_->records, [&](const std::unique_ptr<Impl::Record> &record) {
                return SameScope(record->snapshot.fence, fence) && (!record->snapshot.terminal.has_value() || !WorkerTerminal(record->job));
            }))
                return Result<void>::Failure(MakeError(PCGErrors::AsyncNotReady));
            existing->fence = fence;
            existing->open = true;
            return Result<void>::Success();
        }
        if (impl_->scopes.size() >= impl_->limits.maximumScopes)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncCapacityExceeded));
        impl_->scopes.push_back({fence});
        return Result<void>::Success();
    }

    /** @copydoc PCGAsyncOperations::ReplaceFence */
    Result<void> PCGAsyncOperations::ReplaceFence(const PCGAsyncFence &fence) {
        if (!impl_->OnOwnerThread())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncWrongThread));
        if (impl_->publishing)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncNotReady));
        if (!impl_->accepting)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncClosed));
        if (!fence.IsValid())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncInvalid));
        Impl::Scope *const scope = impl_->FindScope(fence);
        if (scope == nullptr)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncUnknown));
        if (!scope->open)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncClosed));
        if (!AdvancesFence(scope->fence, fence))
            return Result<void>::Failure(MakeError(PCGErrors::AsyncStale));
        if (scope->fence == fence)
            return Result<void>::Success();
        scope->fence = fence;
        for (const auto &record : impl_->records) {
            if (SameScope(record->snapshot.fence, fence))
                impl_->Cancel(*record);
        }
        return Result<void>::Success();
    }

    /** @copydoc PCGAsyncOperations::Submit */
    Result<PCGAsyncOperationId> PCGAsyncOperations::Submit(const PCGAsyncKind kind, PCGAsyncRequest request) {
        const auto admission = impl_->ValidateSubmission(kind, request);
        if (admission.HasError())
            return Result<PCGAsyncOperationId>::Failure(admission.ErrorValue());
        const auto id = PCGAsyncOperationId::Create(impl_->nextId);
        if (id.HasError())
            return Result<PCGAsyncOperationId>::Failure(id.ErrorValue());

        auto record = std::make_unique<Impl::Record>(request.parentCancellation);
        record->snapshot = {.id = id.Value(), .kind = kind, .fence = request.fence};
        record->workUnits = request.workUnits;
        record->publish = std::move(request.publish);
        auto acceptedChildren = record->acceptedChildren;
        JobDescriptor descriptor{.parentCancellation = record->cancellation.Token(),
                                 .operationId = request.operationId,
                                 .configuration = std::move(request.configuration)};
        auto submitted = impl_->jobs.SubmitContext(std::move(descriptor),
                                                   [scheduler = &impl_->jobs, prepare = std::move(request.prepare), acceptedChildren,
                                                    childJoinTimeout = impl_->limits.childJoinTimeout](const JobExecutionContext &context) {
            return RunPreparation(context, *scheduler, prepare, *acceptedChildren, childJoinTimeout);
        });
        if (submitted.HasError())
            return Result<PCGAsyncOperationId>::Failure(submitted.ErrorValue());
        record->job = std::move(submitted).Value();
        record->snapshot.jobId = record->job.Id();
        impl_->records.push_back(std::move(record));
        ++impl_->nextId;
        return Result<PCGAsyncOperationId>::Success(id.Value());
    }

    /** @copydoc PCGAsyncOperations::Advance */
    Result<PCGAsyncSnapshot> PCGAsyncOperations::Advance(const PCGAsyncOperationId id) {
        if (!impl_->OnOwnerThread())
            return Result<PCGAsyncSnapshot>::Failure(MakeError(PCGErrors::AsyncWrongThread));
        if (impl_->publishing)
            return Result<PCGAsyncSnapshot>::Failure(MakeError(PCGErrors::AsyncNotReady));
        Impl::Record *const record = impl_->FindRecord(id);
        if (record == nullptr)
            return Result<PCGAsyncSnapshot>::Failure(MakeError(PCGErrors::AsyncUnknown));
        if (record->snapshot.terminal.has_value())
            return Result<PCGAsyncSnapshot>::Success(record->snapshot);
        const auto job = record->job.Snapshot();
        if (!job.has_value())
            return Result<PCGAsyncSnapshot>::Failure(MakeError(PCGErrors::AsyncUnknown));
        if (!job->terminalResult.has_value()) {
            record->snapshot.state = job->state == JobState::Running ? PCGAsyncState::Running : PCGAsyncState::Queued;
            return Result<PCGAsyncSnapshot>::Success(record->snapshot);
        }

        const auto &worker = *job->terminalResult;
        if (worker.state == JobState::Cancelled) {
            impl_->Finish(*record, PCGAsyncState::Cancelled, worker.error.value_or(JobCancelled().ErrorValue()));
        } else if (worker.state == JobState::Failed) {
            impl_->Finish(*record, PCGAsyncState::Failed, worker.error);
        } else {
            const Impl::Scope *const scope = impl_->FindScope(record->snapshot.fence);
            if (scope == nullptr || !scope->open || scope->fence != record->snapshot.fence || !impl_->accepting)
                impl_->Finish(*record, PCGAsyncState::Stale, MakeError(PCGErrors::AsyncStale));
            else if (record->cancellation.Token().IsCancellationRequested())
                impl_->Finish(*record, PCGAsyncState::Cancelled, JobCancelled().ErrorValue());
            else
                impl_->Publish(*record);
        }
        return Result<PCGAsyncSnapshot>::Success(record->snapshot);
    }

    /** @copydoc PCGAsyncOperations::AdvanceAll */
    Result<std::size_t> PCGAsyncOperations::AdvanceAll() {
        if (!impl_->OnOwnerThread())
            return Result<std::size_t>::Failure(MakeError(PCGErrors::AsyncWrongThread));
        if (impl_->publishing)
            return Result<std::size_t>::Failure(MakeError(PCGErrors::AsyncNotReady));
        std::size_t pending{};
        for (const auto &record : impl_->records) {
            auto advanced = Advance(record->snapshot.id);
            if (advanced.HasError())
                return Result<std::size_t>::Failure(advanced.ErrorValue());
            if (!advanced.Value().terminal.has_value())
                ++pending;
        }
        return Result<std::size_t>::Success(pending);
    }

    /** @copydoc PCGAsyncOperations::Snapshot */
    Result<PCGAsyncSnapshot> PCGAsyncOperations::Snapshot(const PCGAsyncOperationId id) const {
        if (!impl_->OnOwnerThread())
            return Result<PCGAsyncSnapshot>::Failure(MakeError(PCGErrors::AsyncWrongThread));
        const Impl::Record *const record = impl_->FindRecord(id);
        return record == nullptr ? Result<PCGAsyncSnapshot>::Failure(MakeError(PCGErrors::AsyncUnknown))
                                 : Result<PCGAsyncSnapshot>::Success(record->snapshot);
    }

    /** @copydoc PCGAsyncOperations::RequestCancel */
    Result<void> PCGAsyncOperations::RequestCancel(const PCGAsyncOperationId id) {
        if (!impl_->OnOwnerThread())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncWrongThread));
        if (impl_->publishing)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncNotReady));
        Impl::Record *const record = impl_->FindRecord(id);
        if (record == nullptr)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncUnknown));
        impl_->Cancel(*record);
        return Result<void>::Success();
    }

    /** @copydoc PCGAsyncOperations::InvalidateGraph */
    Result<void> PCGAsyncOperations::InvalidateGraph(const PCGAsyncSceneId scene, const GenerationCellId cell, const GraphId graph) {
        if (!impl_->OnOwnerThread())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncWrongThread));
        if (impl_->publishing)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncNotReady));
        if (!scene.IsValid() || !cell.IsValid() || !graph.IsValid())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncInvalid));
        for (auto &scope : impl_->scopes) {
            if (scope.fence.scene == scene && scope.fence.cell == cell && scope.fence.graph.graph == graph)
                scope.open = false;
        }
        for (const auto &record : impl_->records) {
            const auto &fence = record->snapshot.fence;
            if (fence.scene == scene && fence.cell == cell && fence.graph.graph == graph)
                impl_->Cancel(*record);
        }
        return Result<void>::Success();
    }

    /** @copydoc PCGAsyncOperations::InvalidateCell */
    Result<void> PCGAsyncOperations::InvalidateCell(const PCGAsyncSceneId scene, const GenerationCellId cell) {
        if (!impl_->OnOwnerThread())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncWrongThread));
        if (impl_->publishing)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncNotReady));
        if (!scene.IsValid() || !cell.IsValid())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncInvalid));
        for (auto &scope : impl_->scopes) {
            if (scope.fence.scene == scene && scope.fence.cell == cell)
                scope.open = false;
        }
        for (const auto &record : impl_->records) {
            const auto &fence = record->snapshot.fence;
            if (fence.scene == scene && fence.cell == cell)
                impl_->Cancel(*record);
        }
        return Result<void>::Success();
    }

    /** @copydoc PCGAsyncOperations::InvalidateScene */
    Result<void> PCGAsyncOperations::InvalidateScene(const PCGAsyncSceneId scene) {
        if (!impl_->OnOwnerThread())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncWrongThread));
        if (impl_->publishing)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncNotReady));
        if (!scene.IsValid())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncInvalid));
        for (auto &scope : impl_->scopes) {
            if (scope.fence.scene == scene)
                scope.open = false;
        }
        for (const auto &record : impl_->records) {
            if (record->snapshot.fence.scene == scene)
                impl_->Cancel(*record);
        }
        return Result<void>::Success();
    }

    /** @copydoc PCGAsyncOperations::BeginShutdown */
    Result<void> PCGAsyncOperations::BeginShutdown() {
        if (!impl_->OnOwnerThread())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncWrongThread));
        if (impl_->publishing)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncNotReady));
        impl_->accepting = false;
        for (auto &scope : impl_->scopes)
            scope.open = false;
        for (const auto &record : impl_->records)
            impl_->Cancel(*record);
        return Result<void>::Success();
    }

    /** @copydoc PCGAsyncOperations::IsDrained */
    bool PCGAsyncOperations::IsDrained() const {
        return impl_->OnOwnerThread() && std::ranges::all_of(impl_->records, [](const std::unique_ptr<Impl::Record> &record) {
            return record->snapshot.terminal.has_value() && WorkerTerminal(record->job);
        });
    }

    /** @copydoc PCGAsyncOperations::IsGraphDrained */
    bool PCGAsyncOperations::IsGraphDrained(const PCGAsyncSceneId scene, const GenerationCellId cell, const GraphId graph) const {
        return impl_->OnOwnerThread() && scene.IsValid() && cell.IsValid() && graph.IsValid() &&
               std::ranges::all_of(impl_->records, [&](const std::unique_ptr<Impl::Record> &record) {
            const auto &fence = record->snapshot.fence;
            return (fence.scene != scene || fence.cell != cell || fence.graph.graph != graph) ||
                   (record->snapshot.terminal.has_value() && WorkerTerminal(record->job));
        });
    }

    /** @copydoc PCGAsyncOperations::IsCellDrained */
    bool PCGAsyncOperations::IsCellDrained(const PCGAsyncSceneId scene, const GenerationCellId cell) const {
        return impl_->OnOwnerThread() && scene.IsValid() && cell.IsValid() &&
               std::ranges::all_of(impl_->records, [&](const std::unique_ptr<Impl::Record> &record) {
            const auto &fence = record->snapshot.fence;
            return (fence.scene != scene || fence.cell != cell) || (record->snapshot.terminal.has_value() && WorkerTerminal(record->job));
        });
    }

    /** @copydoc PCGAsyncOperations::IsSceneDrained */
    bool PCGAsyncOperations::IsSceneDrained(const PCGAsyncSceneId scene) const {
        return impl_->OnOwnerThread() && scene.IsValid() &&
               std::ranges::all_of(impl_->records, [&](const std::unique_ptr<Impl::Record> &record) {
            return record->snapshot.fence.scene != scene || (record->snapshot.terminal.has_value() && WorkerTerminal(record->job));
        });
    }

    /** @copydoc PCGAsyncOperations::RetireScope */
    Result<void> PCGAsyncOperations::RetireScope(const PCGAsyncSceneId scene, const GenerationCellId cell, const GraphId graph) {
        if (!impl_->OnOwnerThread())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncWrongThread));
        if (impl_->publishing)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncNotReady));
        if (!scene.IsValid() || !cell.IsValid() || !graph.IsValid())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncInvalid));
        const auto found = std::ranges::find_if(impl_->scopes, [&](const Impl::Scope &scope) {
            return scope.fence.scene == scene && scope.fence.cell == cell && scope.fence.graph.graph == graph;
        });
        if (found == impl_->scopes.end())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncUnknown));
        if (found->open || std::ranges::any_of(impl_->records, [&](const std::unique_ptr<Impl::Record> &record) {
            const auto &fence = record->snapshot.fence;
            return fence.scene == scene && fence.cell == cell && fence.graph.graph == graph;
        }))
            return Result<void>::Failure(MakeError(PCGErrors::AsyncNotReady));
        impl_->scopes.erase(found);
        return Result<void>::Success();
    }

    /** @copydoc PCGAsyncOperations::Forget */
    Result<void> PCGAsyncOperations::Forget(const PCGAsyncOperationId id) {
        if (!impl_->OnOwnerThread())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncWrongThread));
        if (impl_->publishing)
            return Result<void>::Failure(MakeError(PCGErrors::AsyncNotReady));
        const auto found = std::ranges::find_if(impl_->records, [id](const std::unique_ptr<Impl::Record> &record) {
            return record->snapshot.id == id;
        });
        if (found == impl_->records.end())
            return Result<void>::Failure(MakeError(PCGErrors::AsyncUnknown));
        if (!(*found)->snapshot.terminal.has_value() || !WorkerTerminal((*found)->job))
            return Result<void>::Failure(MakeError(PCGErrors::AsyncNotReady));
        impl_->records.erase(found);
        return Result<void>::Success();
    }
}  // namespace Horo::PCG
