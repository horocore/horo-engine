#include "editor/document/SceneFileWatchService.h"

#include <mutex>

namespace Horo::Editor {
    namespace {
        const ErrorDomainId SceneFileWatchDomain{"horo.editor.scene_file_watch"};
        const ErrorCodeDescriptor SceneFileWatchShutdown{
            .domain = SceneFileWatchDomain,
            .code = ErrorCode{"scene_file_watch.shutdown"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Scene file watcher is shut down.",
        };
        const ErrorCodeDescriptor SceneFileWatchBusy{
            .domain = SceneFileWatchDomain,
            .code = ErrorCode{"scene_file_watch.busy"},
            .defaultSeverity = ErrorSeverity::Warning,
            .summary = "Scene file watcher already owns an inspection.",
            .retryable = true,
        };

        struct Completion {
            std::mutex mutex;
            std::vector<SceneFileWatchUpdate> updates;
        };

        [[nodiscard]] bool IsTerminal(const JobState state) {
            using enum JobState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

    }  // namespace

    struct SceneFileWatchService::State {
        JobSystem &jobs;
        CancellationSource cancellation;
        std::optional<JobHandle> job;
        std::shared_ptr<Completion> completion{std::make_shared<Completion>()};
        std::uint64_t generation{};
        bool shutdown{};
    };

    /** @copydoc SceneFileWatchService::SceneFileWatchService */
    SceneFileWatchService::SceneFileWatchService(JobSystem &jobs) : state_(std::make_unique<State>(State{.jobs = jobs})) {}

    /** @copydoc SceneFileWatchService::~SceneFileWatchService */
    SceneFileWatchService::~SceneFileWatchService() {
        Shutdown();
    }

    /** @copydoc SceneFileWatchService::Request */
    Result<std::uint64_t> SceneFileWatchService::Request(const std::filesystem::path &absoluteProjectRoot,
                                                         const std::filesystem::path &absoluteScenePath) {
        if (state_->shutdown) {
            return Result<std::uint64_t>::Failure(MakeError(SceneFileWatchShutdown));
        }
        if (HasPendingInspection()) {
            return Result<std::uint64_t>::Failure(MakeError(SceneFileWatchBusy));
        }
        if (state_->job.has_value()) {
            static_cast<void>(state_->job->Wait());
            state_->job.reset();
        }

        state_->cancellation = CancellationSource{};
        const std::uint64_t generation = ++state_->generation;
        const std::shared_ptr<Completion> completion = state_->completion;
        auto submitted = state_->jobs.SubmitResult({.parentCancellation = state_->cancellation.Token()},
                                                   [absoluteProjectRoot, absoluteScenePath, generation,
                                                    completion](const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return JobCancelled();
            Result<SceneFileFingerprint> inspected = InspectProjectSceneFingerprint(absoluteProjectRoot, absoluteScenePath);
            if (cancellation.IsCancellationRequested())
                return JobCancelled();

            SceneFileWatchUpdate update{.generation = generation};
            if (inspected.HasError())
                update.error = inspected.ErrorValue();
            else
                update.fingerprint = std::move(inspected).Value();
            std::lock_guard lock(completion->mutex);
            completion->updates.push_back(std::move(update));
            return Result<void>::Success();
        });
        if (submitted.HasError())
            return Result<std::uint64_t>::Failure(submitted.ErrorValue());
        state_->job = std::move(submitted).Value();
        return Result<std::uint64_t>::Success(generation);
    }

    /** @copydoc SceneFileWatchService::DrainUpdates */
    std::vector<SceneFileWatchUpdate> SceneFileWatchService::DrainUpdates() {
        std::vector<SceneFileWatchUpdate> updates;
        {
            std::lock_guard lock(state_->completion->mutex);
            for (SceneFileWatchUpdate &update : state_->completion->updates) {
                if (update.generation == state_->generation)
                    updates.push_back(std::move(update));
            }
            state_->completion->updates.clear();
        }
        if (state_->job.has_value() && IsTerminal(state_->jobs.Query(state_->job->Id()).state)) {
            static_cast<void>(state_->job->Wait());
            state_->job.reset();
        }
        return updates;
    }

    /** @copydoc SceneFileWatchService::HasPendingInspection */
    bool SceneFileWatchService::HasPendingInspection() const noexcept {
        return state_->job.has_value() && !IsTerminal(state_->jobs.Query(state_->job->Id()).state);
    }

    /** @copydoc SceneFileWatchService::Reset */
    void SceneFileWatchService::Reset() noexcept {
        if (state_->shutdown)
            return;
        state_->cancellation.RequestCancellation();
        if (state_->job.has_value()) {
            static_cast<void>(state_->jobs.RequestCancel(state_->job->Id()));
            static_cast<void>(state_->job->Wait());
            state_->job.reset();
        }
        ++state_->generation;
        state_->cancellation = CancellationSource{};
        std::lock_guard lock(state_->completion->mutex);
        state_->completion->updates.clear();
    }

    /** @copydoc SceneFileWatchService::Shutdown */
    void SceneFileWatchService::Shutdown() noexcept {
        if (state_->shutdown)
            return;
        state_->shutdown = true;
        state_->cancellation.RequestCancellation();
        if (state_->job.has_value()) {
            static_cast<void>(state_->jobs.RequestCancel(state_->job->Id()));
            static_cast<void>(state_->job->Wait());
            state_->job.reset();
        }
        std::lock_guard lock(state_->completion->mutex);
        state_->completion->updates.clear();
    }
}  // namespace Horo::Editor
