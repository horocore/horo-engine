#include "Horo/Terrain/TerrainAsyncJobs.h"

#include "Horo/Terrain/TerrainErrors.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Terrain {
    namespace {
        [[nodiscard]] bool IsWorkerTerminal(const JobHandle &job) {
            const auto snapshot = job.Snapshot();
            return snapshot.has_value() && snapshot->terminalResult.has_value();
        }

        [[nodiscard]] Error CancelledWithCause(const Error &cause) {
            return JobCancelled(cause).ErrorValue();
        }

        [[nodiscard]] bool RegressesWithinIncarnation(const TerrainAsyncWorkFence &current, const TerrainAsyncWorkFence &next) noexcept {
            if (current.runtime != next.runtime)
                return false;
            const auto &before = current.revisions;
            const auto &after = next.revisions;
            return after.content < before.content || after.residency < before.residency || after.mutation < before.mutation ||
                   after.capability < before.capability ||
                   (current.registry.registry == next.registry.registry && current.registry.revision > next.registry.revision);
        }
    }  // namespace

    struct TerrainAsyncJobs::Impl final {
        struct Record final {
            TerrainAsyncWorkSnapshot snapshot;
            CancellationSource cancellation;
            JobHandle job;
            std::function<Result<void>(const CancellationToken &)> publish;

            explicit Record(const CancellationToken &parent) : cancellation(parent) {}
        };

        JobSystem &jobs;
        const std::thread::id ownerThread;
        TerrainAsyncWorkFence currentFence;
        TerrainFoliageCapabilitySet available;
        TerrainAsyncJobLimits limits;
        std::vector<std::unique_ptr<Record>> records;
        std::uint64_t nextId{1};
        bool accepting{true};
        bool publishing{false};

        Impl(JobSystem &scheduler, const TerrainAsyncWorkFence fence, const TerrainFoliageCapabilitySet capabilities,
             const TerrainAsyncJobLimits bounds)
            : jobs(scheduler), ownerThread(std::this_thread::get_id()), currentFence(fence), available(capabilities), limits(bounds) {}

        [[nodiscard]] bool OnOwnerThread() const noexcept {
            return std::this_thread::get_id() == ownerThread;
        }

        [[nodiscard]] Record *Find(const TerrainAsyncWorkId id) const noexcept {
            const auto found = std::ranges::find_if(records, [id](const std::unique_ptr<Record> &record) {
                return record->snapshot.id == id;
            });
            return found == records.end() ? nullptr : found->get();
        }

        /** @brief Runs one prepared candidate's owner callback and records its exact terminal result. */
        void Publish(Record &record) {
            record.snapshot.state = TerrainAsyncWorkState::Prepared;
            publishing = true;
            try {
                const Result<void> published = record.publish(record.cancellation.Token());
                if (published.HasError()) {
                    record.snapshot.state =
                        IsJobCancelled(published.ErrorValue()) ? TerrainAsyncWorkState::Cancelled : TerrainAsyncWorkState::Failed;
                    record.snapshot.error = published.ErrorValue();
                } else {
                    record.snapshot.state = TerrainAsyncWorkState::Succeeded;
                }
            } catch (const std::exception &) {
                record.snapshot.state = TerrainAsyncWorkState::Failed;
                record.snapshot.error = MakeError(TerrainErrors::WorkPublicationFailed);
            } catch (...) {  // NOSONAR(cpp:S1181) Contain foreign callback exceptions at the owner boundary.
                record.snapshot.state = TerrainAsyncWorkState::Failed;
                record.snapshot.error = MakeError(TerrainErrors::WorkPublicationFailed);
            }
            publishing = false;
        }
    };

    TerrainAsyncJobs::TerrainAsyncJobs(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    TerrainAsyncJobs::~TerrainAsyncJobs() {
        BeginShutdown();
    }

    /** @copydoc TerrainAsyncJobs::Create */
    Result<std::unique_ptr<TerrainAsyncJobs>> TerrainAsyncJobs::Create(JobSystem &jobs, const TerrainAsyncWorkFence fence,
                                                                       const TerrainFoliageCapabilitySet available,
                                                                       const TerrainAsyncJobLimits limits) {
        if (!fence.IsValid() || !available.IsValid() || !limits.IsValid())
            return Result<std::unique_ptr<TerrainAsyncJobs>>::Failure(MakeError(TerrainErrors::WorkInvalid));
        return Result<std::unique_ptr<TerrainAsyncJobs>>::Success(
            std::unique_ptr<TerrainAsyncJobs>(new TerrainAsyncJobs(std::make_unique<Impl>(jobs, fence, available, limits))));
    }

    /** @copydoc TerrainAsyncJobs::SubmitCook */
    Result<TerrainAsyncWorkId> TerrainAsyncJobs::SubmitCook(TerrainAsyncWorkRequest request) {
        return Submit(TerrainAsyncWorkKind::Cook, std::move(request));
    }

    /** @copydoc TerrainAsyncJobs::SubmitLoad */
    Result<TerrainAsyncWorkId> TerrainAsyncJobs::SubmitLoad(TerrainAsyncWorkRequest request) {
        return Submit(TerrainAsyncWorkKind::Load, std::move(request));
    }

    /** @copydoc TerrainAsyncJobs::SubmitEditPreview */
    Result<TerrainAsyncWorkId> TerrainAsyncJobs::SubmitEditPreview(TerrainAsyncWorkRequest request) {
        return Submit(TerrainAsyncWorkKind::EditPreview, std::move(request));
    }

    Result<TerrainAsyncWorkId> TerrainAsyncJobs::Submit(const TerrainAsyncWorkKind kind, TerrainAsyncWorkRequest request) {
        if (!impl_->OnOwnerThread())
            return Result<TerrainAsyncWorkId>::Failure(MakeError(TerrainErrors::WorkWrongThread));
        if (!impl_->accepting)
            return Result<TerrainAsyncWorkId>::Failure(MakeError(TerrainErrors::LifecycleUnavailable));
        if (request.workUnits == 0 || request.workUnits > impl_->limits.maximumWorkUnits || !request.requiredCapabilities.IsValid() ||
            !request.prepare || !request.publish)
            return Result<TerrainAsyncWorkId>::Failure(MakeError(TerrainErrors::WorkInvalid));
        if (!impl_->available.ContainsAll(request.requiredCapabilities))
            return Result<TerrainAsyncWorkId>::Failure(MakeError(TerrainErrors::CapabilityUnsupported));
        if (request.parentCancellation.IsCancellationRequested())
            return Result<TerrainAsyncWorkId>::Failure(JobCancelled().ErrorValue());
        if (impl_->records.size() >= impl_->limits.maximumTracked)
            return Result<TerrainAsyncWorkId>::Failure(MakeError(TerrainErrors::CapacityExceeded));
        if (impl_->nextId == std::numeric_limits<std::uint64_t>::max())
            return Result<TerrainAsyncWorkId>::Failure(MakeError(TerrainErrors::GenerationExhausted));

        const auto id = TerrainAsyncWorkId::Create(impl_->nextId);
        if (id.HasError())
            return Result<TerrainAsyncWorkId>::Failure(id.ErrorValue());
        auto record = std::make_unique<Impl::Record>(request.parentCancellation);
        record->snapshot.id = id.Value();
        record->snapshot.kind = kind;
        record->snapshot.fence = impl_->currentFence;
        record->publish = std::move(request.publish);
        JobDescriptor descriptor{.parentCancellation = record->cancellation.Token(),
                                 .operationId = request.operationId,
                                 .configuration = std::move(request.configuration)};
        auto submitted =
            impl_->jobs.SubmitContext(std::move(descriptor), [prepare = std::move(request.prepare)](const JobExecutionContext &context) {
            if (context.Cancellation().IsCancellationRequested())
                return JobCancelled();
            Result<void> result = prepare(context);
            if (context.Cancellation().IsCancellationRequested()) {
                if (result.HasError()) {
                    if (IsJobCancelled(result.ErrorValue()))
                        return result;
                    return JobCancelled(result.ErrorValue());
                }
                return JobCancelled();
            }
            return result;
        });
        if (submitted.HasError())
            return Result<TerrainAsyncWorkId>::Failure(submitted.ErrorValue());
        record->job = std::move(submitted).Value();
        record->snapshot.jobId = record->job.Id();
        impl_->records.push_back(std::move(record));
        ++impl_->nextId;
        return Result<TerrainAsyncWorkId>::Success(id.Value());
    }

    /** @copydoc TerrainAsyncJobs::Advance */
    Result<TerrainAsyncWorkSnapshot> TerrainAsyncJobs::Advance(const TerrainAsyncWorkId id) {
        if (!impl_->OnOwnerThread())
            return Result<TerrainAsyncWorkSnapshot>::Failure(MakeError(TerrainErrors::WorkWrongThread));
        if (impl_->publishing)
            return Result<TerrainAsyncWorkSnapshot>::Failure(MakeError(TerrainErrors::WorkNotReady));
        Impl::Record *const record = impl_->Find(id);
        if (record == nullptr)
            return Result<TerrainAsyncWorkSnapshot>::Failure(MakeError(TerrainErrors::WorkUnknown));
        if (record->snapshot.IsTerminal())
            return Result<TerrainAsyncWorkSnapshot>::Success(record->snapshot);
        const auto job = record->job.Snapshot();
        if (!job.has_value())
            return Result<TerrainAsyncWorkSnapshot>::Failure(MakeError(TerrainErrors::WorkUnknown));
        if (!job->terminalResult.has_value()) {
            record->snapshot.state = job->state == JobState::Running ? TerrainAsyncWorkState::Running : TerrainAsyncWorkState::Queued;
            return Result<TerrainAsyncWorkSnapshot>::Success(record->snapshot);
        }

        const auto &terminal = *job->terminalResult;
        if (terminal.state == JobState::Cancelled) {
            record->snapshot.state = TerrainAsyncWorkState::Cancelled;
            record->snapshot.error = terminal.error.value_or(JobCancelled().ErrorValue());
        } else if (terminal.state == JobState::Failed) {
            record->snapshot.state = TerrainAsyncWorkState::Failed;
            record->snapshot.error = terminal.error;
        } else if (!impl_->accepting || record->cancellation.Token().IsCancellationRequested() ||
                   record->snapshot.fence != impl_->currentFence) {
            record->snapshot.state = TerrainAsyncWorkState::Cancelled;
            record->snapshot.error = record->snapshot.fence != impl_->currentFence
                                         ? CancelledWithCause(MakeError(TerrainErrors::RevisionStale))
                                         : JobCancelled().ErrorValue();
        } else {
            impl_->Publish(*record);
        }
        record->publish = {};
        return Result<TerrainAsyncWorkSnapshot>::Success(record->snapshot);
    }

    /** @copydoc TerrainAsyncJobs::Snapshot */
    Result<TerrainAsyncWorkSnapshot> TerrainAsyncJobs::Snapshot(const TerrainAsyncWorkId id) const {
        if (!impl_->OnOwnerThread())
            return Result<TerrainAsyncWorkSnapshot>::Failure(MakeError(TerrainErrors::WorkWrongThread));
        const Impl::Record *const record = impl_->Find(id);
        return record == nullptr ? Result<TerrainAsyncWorkSnapshot>::Failure(MakeError(TerrainErrors::WorkUnknown))
                                 : Result<TerrainAsyncWorkSnapshot>::Success(record->snapshot);
    }

    /** @copydoc TerrainAsyncJobs::RequestCancel */
    Result<void> TerrainAsyncJobs::RequestCancel(const TerrainAsyncWorkId id) {
        if (!impl_->OnOwnerThread())
            return Result<void>::Failure(MakeError(TerrainErrors::WorkWrongThread));
        Impl::Record *const record = impl_->Find(id);
        if (record == nullptr)
            return Result<void>::Failure(MakeError(TerrainErrors::WorkUnknown));
        if (!record->snapshot.IsTerminal()) {
            record->cancellation.RequestCancellation();
            static_cast<void>(record->job.RequestCancel());
        }
        return Result<void>::Success();
    }

    /** @copydoc TerrainAsyncJobs::ReplaceFence */
    Result<void> TerrainAsyncJobs::ReplaceFence(const TerrainAsyncWorkFence fence, const TerrainFoliageCapabilitySet available) {
        if (!impl_->OnOwnerThread())
            return Result<void>::Failure(MakeError(TerrainErrors::WorkWrongThread));
        if (!impl_->accepting)
            return Result<void>::Failure(MakeError(TerrainErrors::LifecycleUnavailable));
        if (impl_->publishing)
            return Result<void>::Failure(MakeError(TerrainErrors::WorkNotReady));
        if (!fence.IsValid() || !available.IsValid() || fence.runtime.dataset != impl_->currentFence.runtime.dataset)
            return Result<void>::Failure(MakeError(TerrainErrors::WorkInvalid));
        if (RegressesWithinIncarnation(impl_->currentFence, fence))
            return Result<void>::Failure(MakeError(TerrainErrors::RevisionStale));
        if (available != impl_->available && fence.runtime == impl_->currentFence.runtime &&
            fence.revisions.capability == impl_->currentFence.revisions.capability)
            return Result<void>::Failure(MakeError(TerrainErrors::WorkInvalid));
        if (fence == impl_->currentFence && available == impl_->available)
            return Result<void>::Success();
        impl_->currentFence = fence;
        impl_->available = available;
        for (const auto &record : impl_->records) {
            if (!record->snapshot.IsTerminal()) {
                record->cancellation.RequestCancellation();
                static_cast<void>(record->job.RequestCancel());
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc TerrainAsyncJobs::BeginShutdown */
    void TerrainAsyncJobs::BeginShutdown() {
        if (!impl_ || !impl_->OnOwnerThread() || !impl_->accepting)
            return;
        impl_->accepting = false;
        for (const auto &record : impl_->records) {
            if (!record->snapshot.IsTerminal()) {
                record->cancellation.RequestCancellation();
                static_cast<void>(record->job.RequestCancel());
            }
        }
    }

    /** @copydoc TerrainAsyncJobs::IsDrained */
    bool TerrainAsyncJobs::IsDrained() const {
        return impl_ && impl_->OnOwnerThread() && std::ranges::all_of(impl_->records, [](const std::unique_ptr<Impl::Record> &record) {
            return IsWorkerTerminal(record->job);
        });
    }

    /** @copydoc TerrainAsyncJobs::Forget */
    Result<void> TerrainAsyncJobs::Forget(const TerrainAsyncWorkId id) {
        if (!impl_->OnOwnerThread())
            return Result<void>::Failure(MakeError(TerrainErrors::WorkWrongThread));
        const auto found = std::ranges::find_if(impl_->records, [id](const std::unique_ptr<Impl::Record> &record) {
            return record->snapshot.id == id;
        });
        if (found == impl_->records.end())
            return Result<void>::Failure(MakeError(TerrainErrors::WorkUnknown));
        if (!(*found)->snapshot.IsTerminal() || !IsWorkerTerminal((*found)->job))
            return Result<void>::Failure(MakeError(TerrainErrors::WorkNotReady));
        impl_->records.erase(found);
        return Result<void>::Success();
    }
}  // namespace Horo::Terrain
