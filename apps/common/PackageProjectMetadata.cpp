#include "PackageProjectHostInternal.h"

namespace Horo::Application::Internal {
    using PackageHost::Failure;

    Result<void> PackageProjectHost::Impl::Admission() const {
        if (auto valid = Lane(); valid.HasError())
            return valid;
        if (closed || closing || retiring || !current)
            return Result<void>::Failure(Failure("packages.host.project_stale"));
        if (diagnostic)
            return Result<void>::Failure(*diagnostic);
        if (!Fresh(clock.MonotonicNow()))
            return Result<void>::Failure(Failure("packages.host.metadata_unavailable"));
        return Result<void>::Success();
    }

    /** @brief Transfers only immutable metadata and an owned completion cell to the I/O worker. */
    Result<void> PackageProjectHost::Impl::StartMetadata() {
        auto destination = std::make_shared<PackageHost::Completion>();
        auto captured = policy;
        const auto sampleTime = clock.MonotonicNow();
        auto submitted = jobs.SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Io},
                                           [destination, captured = std::move(captured)](const CancellationToken &cancellation) {
            auto result = PreparePackageProject(captured, cancellation);
            std::lock_guard guard(destination->mutex);
            destination->result.emplace(std::move(result));
            return Result<void>::Success();
        });
        if (submitted.HasError())
            return Result<void>::Failure(submitted.ErrorValue());
        completion = std::move(destination);
        jobRoot = policy.projectRoot;
        jobSampleTime = sampleTime;
        metadataJob.emplace(std::move(submitted).Value());
        return Result<void>::Success();
    }

    /** @copydoc PackageProjectHost::Impl::RejectMetadata */
    void PackageProjectHost::Impl::RejectMetadata(Error error) {
        diagnostic = std::move(error);
        if (current && !retiring) {
            const auto advanced = AdvanceGeneration();
            if (advanced.HasError())
                diagnostic = advanced.ErrorValue();
        }
        retiring = true;
    }

    /** @brief Consumes terminal callbacks before deciding whether any old generation can still activate. */
    void PackageProjectHost::Impl::ReceiveMetadata(const Duration now) {
        if (!metadataJob)
            return;
        const auto snapshot = metadataJob->Snapshot();
        if (!snapshot || snapshot->state == JobState::Running || snapshot->state == JobState::Queued)
            return;
        std::optional<Result<PreparedPackageProject>> result;
        {
            std::lock_guard guard(completion->mutex);
            result = std::move(completion->result);
        }
        metadataJob.reset();
        completion.reset();
        if (jobRoot != policy.projectRoot)
            return;
        sampledAt = jobSampleTime;
        if (!Fresh(now)) {
            RejectMetadata(Failure("packages.host.metadata_expired"));
            return;
        }
        if (!result || result->HasError()) {
            RejectMetadata(result ? result->ErrorValue() : snapshot->error.value_or(Failure("packages.host.metadata_failed")));
            return;
        }
        auto candidate = std::move(*result).Value();
        const bool changed =
            !current || candidate.lockDigest != lockDigest || candidate.application.restore.expectedRequestHash != requestDigest;
        diagnostic.reset();
        if (changed) {
            if (current && !retiring) {
                if (auto advanced = AdvanceGeneration(); advanced.HasError()) {
                    diagnostic = advanced.ErrorValue();
                    retiring = true;
                    return;
                }
            }
            retiring = true;
            prepared.emplace(std::move(candidate));
        }
    }

    /** @brief Keeps the concrete authority owner alive until every callback and native retirement is complete. */
    Result<void> PackageProjectHost::Impl::RetireCurrent() {
        if (current) {
            if (auto retired = current->Close(); retired.HasError())
                return retired;
            current.reset();
        }
        retiring = false;
        return Result<void>::Success();
    }

    /** @brief Composes only after the prior generation is fully closed; the borrowed validator cannot outlive live admission. */
    Result<void> PackageProjectHost::Impl::ComposePrepared() {
        prepared->application.validateAdmission = [this] {
            return Admission();
        };
        const auto nextRequest = prepared->application.restore.expectedRequestHash;
        const auto nextLock = prepared->lockDigest;
        auto session = PackageProjectSession::Create(jobs, files, wallClock, trust, policy, std::move(*prepared), source, publishers);
        prepared.reset();
        if (session.HasError()) {
            diagnostic = session.ErrorValue();
            return Result<void>::Failure(*diagnostic);
        }
        current = std::move(session).Value();
        requestDigest = nextRequest;
        lockDigest = nextLock;
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
