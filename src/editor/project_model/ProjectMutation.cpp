#include "Horo/Editor/ProjectMutation.h"

#include <mutex>
#include <unordered_set>
#include <utility>

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

    namespace {
        /** @brief Rolls back this admission's in-process reservation until the complete lease takes ownership. */
        struct PendingMutationRegistration final {
            PendingMutationRegistration(std::shared_ptr<ProjectMutationCoordinator::State> value, const std::filesystem::path &key)
                : owner(std::move(value)), rootKey(key) {}

            PendingMutationRegistration(const PendingMutationRegistration &) = delete;
            PendingMutationRegistration &operator=(const PendingMutationRegistration &) = delete;

            ~PendingMutationRegistration() {
                if (!armed)
                    return;
                std::lock_guard lock(owner->mutex);
                owner->activeRoots.erase(rootKey);
            }

            std::shared_ptr<ProjectMutationCoordinator::State> owner;
            const std::filesystem::path &rootKey;
            bool armed{};
        };
    }  // namespace

    struct ProjectMutationLease::State {
        State(const std::shared_ptr<ProjectMutationCoordinator::State> &value, const std::filesystem::path &key, ExclusiveFileLock &&held)
            : owner(value), rootKey(key), native(std::move(held)) {}

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
        PendingMutationRegistration admission{state_, key};
        {
            std::lock_guard lock(admission.owner->mutex);
            if (!admission.owner->activeRoots.emplace(key).second)
                return Result<ProjectMutationLease>::Failure(MakeError(Locked));
            admission.armed = true;
        }
        auto native = admission.owner->files.TryAcquireExclusive(root / ".horo/local/project-mutation.lock",
                                                                 OwnerName(request.owner) + ":" + request.operationId);
        if (native.HasError())
            return Result<ProjectMutationLease>::Failure(MakeError(Locked, native.ErrorValue().message));
        auto lease = std::make_unique<ProjectMutationLease::State>(admission.owner, key, std::move(native).Value());
        admission.armed = false;
        return Result<ProjectMutationLease>::Success(ProjectMutationLease(std::move(lease)));
    }
}  // namespace Horo::Editor
