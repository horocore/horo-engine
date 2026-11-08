#include "Horo/Editor/ProjectMutation.h"

#include <mutex>
#include <unordered_set>

namespace Horo::Editor {
    namespace {
        const ErrorDomainId Domain{"horo.editor.project_mutation"};
        const ErrorCodeDescriptor Locked{.domain = Domain,
                                         .code = ErrorCode{"project.migration.locked"},
                                         .defaultSeverity = ErrorSeverity::Error,
                                         .summary = "Another project mutation is already active.",
                                         .remediationHint = "Wait for the active save, migration, or package operation to finish.",
                                         .retryable = true,
                                         .userActionable = true};

        std::string OwnerName(const ProjectMutationOwner owner) {
            using enum ProjectMutationOwner;
            switch (owner) {
                case Migration:
                    return "migration";
                case Save:
                    return "save";
                case Autosave:
                    return "autosave";
                case Package:
                    return "package";
                case Asset:
                    return "asset";
                case Cli:
                    return "cli";
                case Mcp:
                    return "mcp";
            }
            return "unknown";
        }
    }  // namespace

    struct ProjectMutationCoordinator::State {
        explicit State(DurableFileSystem &value) : files(value) {}

        DurableFileSystem &files;
        std::mutex mutex;
        std::unordered_set<std::filesystem::path> activeRoots;  // NOSONAR(cpp:S6045)
    };

    struct ProjectMutationLease::State {
        std::shared_ptr<ProjectMutationCoordinator::State> owner;
        std::filesystem::path rootKey;
        ExclusiveFileLock native;

        ~State() {
            std::lock_guard lock(owner->mutex);
            owner->activeRoots.erase(rootKey);
        }
    };

    /** @copydoc ProjectMutationLease */
    ProjectMutationLease::ProjectMutationLease(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    ProjectMutationLease::~ProjectMutationLease() = default;
    ProjectMutationLease::ProjectMutationLease(ProjectMutationLease &&) noexcept = default;
    ProjectMutationLease &ProjectMutationLease::operator=(ProjectMutationLease &&) noexcept = default;

    /** @copydoc ProjectMutationCoordinator::ProjectMutationCoordinator */
    ProjectMutationCoordinator::ProjectMutationCoordinator(DurableFileSystem &files) : state_(std::make_shared<State>(files)) {}

    ProjectMutationCoordinator::~ProjectMutationCoordinator() = default;

    /** @copydoc ProjectMutationCoordinator::TryAcquire */
    Result<ProjectMutationLease> ProjectMutationCoordinator::TryAcquire(const ProjectMutationRequest &request) const {
        std::error_code error;
        const auto root = std::filesystem::weakly_canonical(request.projectRoot, error);
        if (error || !std::filesystem::is_directory(root))
            return Result<ProjectMutationLease>::Failure(MakeError(Locked, "Project root is unavailable."));
        const std::filesystem::path &key = root;
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->activeRoots.emplace(key).second)
                return Result<ProjectMutationLease>::Failure(MakeError(Locked));
        }
        auto native = state_->files.TryAcquireExclusive(root / ".horo/local/project-mutation.lock",
                                                        OwnerName(request.owner) + ":" + request.operationId);
        if (native.HasError()) {
            std::lock_guard lock(state_->mutex);
            state_->activeRoots.erase(key);
            return Result<ProjectMutationLease>::Failure(MakeError(Locked, native.ErrorValue().message));
        }
        auto lease = std::make_unique<ProjectMutationLease::State>();
        lease->owner = state_;
        lease->rootKey = key;
        lease->native = std::move(native).Value();
        return Result<ProjectMutationLease>::Success(ProjectMutationLease(std::move(lease)));
    }
}  // namespace Horo::Editor
