#include "Horo/Editor/RecentProjectInspectionService.h"

#include "editor/EditorServiceErrors.h"

#include <algorithm>
#include <mutex>

namespace Horo::Editor {
    namespace {
        struct Completion {
            std::mutex mutex;
            std::vector<RecentProjectInspectionUpdate> updates;
        };
    }  // namespace

    struct RecentProjectInspectionService::State {
        JobSystem &jobs;
        const ProjectOpenPreflightService &preflight;
        CancellationSource cancellation;
        std::vector<JobHandle> jobsInFlight;
        std::shared_ptr<Completion> completion{std::make_shared<Completion>()};
        std::uint64_t generation{};
        bool shutdown{};

        void CancelJobsInFlight() {
            if (jobsInFlight.empty())
                return;
            cancellation.RequestCancellation();
            for (const JobHandle &job : jobsInFlight)
                static_cast<void>(jobs.RequestCancel(job.Id()));
            for (const JobHandle &job : jobsInFlight)
                static_cast<void>(job.Wait());
            jobsInFlight.clear();
        }
    };

    /** @copydoc RecentProjectInspectionService::RecentProjectInspectionService */
    RecentProjectInspectionService::RecentProjectInspectionService(JobSystem &jobs, const ProjectOpenPreflightService &preflight)
        : state_(std::make_unique<State>(jobs, preflight)) {}

    /** @copydoc RecentProjectInspectionService::~RecentProjectInspectionService */
    RecentProjectInspectionService::~RecentProjectInspectionService() {
        Shutdown();
    }

    /** @copydoc RecentProjectInspectionService::Refresh */
    Result<std::uint64_t> RecentProjectInspectionService::Refresh(const std::span<const RecentProjectEntry> projects) {
        if (state_->shutdown)
            return Result<std::uint64_t>::Failure(MakeError(ProjectOpenErrors::Cancelled));
        state_->CancelJobsInFlight();
        state_->cancellation = CancellationSource{};
        const std::uint64_t generation = ++state_->generation;
        std::vector<std::pair<std::string, std::filesystem::path>> roots;
        roots.reserve(std::min<std::size_t>(projects.size(), 128));
        for (const RecentProjectEntry &project : projects.first(std::min<std::size_t>(projects.size(), 128)))
            if (IsDisplayableRecentProject(project))
                roots.emplace_back(project.rootPath, std::filesystem::path(project.rootPath));
        const auto sharedRoots = std::make_shared<const std::vector<std::pair<std::string, std::filesystem::path>>>(std::move(roots));
        const std::size_t concurrency = std::min<std::size_t>(4, sharedRoots->size());
        if (concurrency == 0)
            return Result<std::uint64_t>::Success(generation);
        const auto completion = state_->completion;
        const auto *preflight = &state_->preflight;
        for (std::size_t lane = 0; lane < concurrency; ++lane) {
            auto submitted = state_->jobs.SubmitResult({.parentCancellation = state_->cancellation.Token()},
                                                       [sharedRoots, lane, concurrency, generation, completion,
                                                        preflight](const CancellationToken &cancellation) {
                for (std::size_t index = lane; index < sharedRoots->size(); index += concurrency) {
                    if (cancellation.IsCancellationRequested())
                        return JobCancelled();
                    const auto &[rootText, root] = (*sharedRoots)[index];
                    ProjectOpenPreflightSnapshot inspected = preflight->Inspect(root);
                    RecentProjectCompatibilityProjection projection{.projectVersion =
                                                                        inspected.compatibility.metadata.has_value()
                                                                            ? std::optional(inspected.compatibility.metadata->horoVersion)
                                                                            : std::nullopt,
                                                                    .status = inspected.compatibility.status,
                                                                    .targetVersion = inspected.compatibility.targetVersion,
                                                                    .inspectionState = RecentProjectInspectionState::Fresh};
                    std::lock_guard lock(completion->mutex);
                    completion->updates.emplace_back(generation, rootText, std::move(projection));
                }
                return Result<void>::Success();
            });
            if (submitted.HasError()) {
                state_->CancelJobsInFlight();
                return Result<std::uint64_t>::Failure(submitted.ErrorValue());
            }
            state_->jobsInFlight.push_back(std::move(submitted).Value());
        }
        return Result<std::uint64_t>::Success(generation);
    }

    /** @copydoc RecentProjectInspectionService::DrainUpdates */
    std::vector<RecentProjectInspectionUpdate> RecentProjectInspectionService::DrainUpdates() const {
        std::vector<RecentProjectInspectionUpdate> updates;
        std::lock_guard lock(state_->completion->mutex);
        for (RecentProjectInspectionUpdate &update : state_->completion->updates)
            if (update.generation == state_->generation)
                updates.push_back(std::move(update));
        state_->completion->updates.clear();
        return updates;
    }

    /** @copydoc RecentProjectInspectionService::Shutdown */
    void RecentProjectInspectionService::Shutdown() noexcept {
        if (!state_ || state_->shutdown)
            return;
        state_->shutdown = true;
        state_->CancelJobsInFlight();
    }

}  // namespace Horo::Editor
