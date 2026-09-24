#include "Horo/Editor/ProjectCreationService.h"

#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Foundation/Progress.h"
#include "Horo/Foundation/String.h"
#include "editor/EditorServiceErrors.h"

#include <cctype>
#include <chrono>
#include <exception>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Horo::Editor {
    namespace {
        [[nodiscard]] Error MakeFoundationError(const ErrorCodeDescriptor &descriptor, const char *message) {
            return MakeError(descriptor, message);
        }

        [[nodiscard]] bool IsSafeProjectRelativePath(const std::string &value) {
            const std::filesystem::path path{value};
            if (value.empty() || path.is_absolute())
                return false;
            return std::ranges::none_of(path, [](const auto &component) {
                return component == "..";
            });
        }

        [[nodiscard]] std::string EscapeJson(const std::string_view value) {
            std::string escaped;
            escaped.reserve(value.size() + 16);
            for (const unsigned char character : value) {
                switch (character) {
                    case '"':
                        escaped.append(R"(\")");
                        break;
                    case '\\':
                        escaped.append(R"(\\)");
                        break;
                    case '\b':
                        escaped.append(R"(\b)");
                        break;
                    case '\f':
                        escaped.append(R"(\f)");
                        break;
                    case '\n':
                        escaped.append(R"(\n)");
                        break;
                    case '\r':
                        escaped.append(R"(\r)");
                        break;
                    case '\t':
                        escaped.append(R"(\t)");
                        break;
                    default:
                        if (character < 0x20) {
                            std::format_to(std::back_inserter(escaped), "\\u00{:02x}", character);
                        } else {
                            escaped.push_back(static_cast<char>(character));
                        }
                }
            }
            return escaped;
        }

        [[nodiscard]] std::string MakeProjectId(const ProjectCreationOperationId operationId) {
            const auto now = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
            return std::format("proj_{:x}{:x}", now, static_cast<std::uint64_t>(operationId));
        }

        [[nodiscard]] std::string GameplayModuleId(const std::string_view projectName) {
            std::string slug;
            slug.reserve(projectName.size());
            for (const unsigned char character : projectName)
                slug.push_back(std::isalnum(character) ? static_cast<char>(std::tolower(character)) : '_');
            if (slug.empty())
                slug = "project";
            return "game." + slug;
        }

        [[nodiscard]] std::string UtcTimestamp() {
            return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::system_clock::now());
        }

        [[nodiscard]] bool WriteDurableFile(const std::filesystem::path &path, const std::string_view contents) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            output.flush();
            if (!output)
                return false;
            output.close();
#if defined(__APPLE__) || defined(__linux__)
            const int descriptor = open(path.c_str(), O_RDONLY);
            if (descriptor < 0)
                return false;
            const bool synced = fsync(descriptor) == 0;
            close(descriptor);
            return synced;
#else
            return true;
#endif
        }

        [[nodiscard]] bool PromoteWithoutReplace(const std::filesystem::path &staging, const std::filesystem::path &destination,
                                                 std::error_code &error) {
            if (std::filesystem::exists(destination, error)) {
                if (std::filesystem::is_directory(destination, error) && std::filesystem::is_empty(destination, error)) {
                    std::filesystem::remove(destination, error);
                    if (error)
                        return false;
                } else {
                    error = std::make_error_code(std::errc::file_exists);
                    return false;
                }
            }
            if (error)
                return false;
#if defined(__APPLE__)
            if (renameatx_np(AT_FDCWD, staging.c_str(), AT_FDCWD, destination.c_str(), RENAME_EXCL) == 0)
                return true;
            error = std::error_code(errno, std::generic_category());
            return false;
#elif defined(__linux__) && defined(SYS_renameat2)
            if (syscall(SYS_renameat2, AT_FDCWD, staging.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) == 0)
                return true;
            error = std::error_code(errno, std::generic_category());
            return false;
#else
            if (std::filesystem::exists(destination, error) || error)
                return false;
            std::filesystem::rename(staging, destination, error);
            return !error;
#endif
        }

        [[nodiscard]] bool IsTerminal(const ProjectCreationOperationState state) noexcept {
            using enum ProjectCreationOperationState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }
    }  // namespace

    struct ProjectCreationServiceState {
        class Synchronization {
        public:
            [[nodiscard]] std::mutex &Mutex() const noexcept {
                return mutex;
            }

        private:
            mutable std::mutex mutex;
        };

        Synchronization synchronization;

        struct Operation {
            ProjectCreationSnapshot snapshot;
            ProgressTracker progress;
            JobId jobId = 0;
        };

        explicit ProjectCreationServiceState(EngineDataBus &bus) : dataBus(bus) {}

        EngineDataBus &dataBus;
        ProjectCreationOperationId nextOperationId = 1;
        ProjectCreationRevision revision = 0;
        std::unordered_map<ProjectCreationOperationId, std::shared_ptr<Operation>> operations;
    };

    namespace {
        void UpdateProgress(const std::shared_ptr<ProjectCreationServiceState> &state,
                            const std::shared_ptr<ProjectCreationServiceState::Operation> &operation,
                            const ProjectCreationOperationState operationState, const ProjectCreationOperationPhase phase,
                            const std::string_view progressPhase, const float progress) {
            ProjectCreationProgressEvent progressEvent;
            {
                std::lock_guard lock(state->synchronization.Mutex());
                if (IsTerminal(operation->snapshot.state))
                    return;
                if (operation->snapshot.state != ProjectCreationOperationState::Cancelling)
                    operation->snapshot.state = operationState;
                operation->snapshot.phase = phase;
                static_cast<void>(operation->progress.Report(progressPhase, progress));
                operation->snapshot.progress = operation->progress.Snapshot().value;
                progressEvent.operationId = operation->snapshot.id;
                progressEvent.phase = phase;
                progressEvent.progress = operation->snapshot.progress;
                progressEvent.revision = ++state->revision;
            }
            state->dataBus.PublishAsync(progressEvent);
        }

        void SetFailure(const std::shared_ptr<ProjectCreationServiceState> &state,
                        const std::shared_ptr<ProjectCreationServiceState::Operation> &operation, const ProjectCreationErrorCode code,
                        std::string message) {
            ProjectCreationRevisionChangedEvent revisionEvent;
            {
                std::lock_guard lock(state->synchronization.Mutex());
                if (IsTerminal(operation->snapshot.state))
                    return;
                operation->snapshot.state = ProjectCreationOperationState::Failed;
                operation->snapshot.phase = ProjectCreationOperationPhase::Failed;
                operation->snapshot.error = ProjectCreationError{code, std::move(message)};
                revisionEvent.revision = ++state->revision;
            }
            LOG_ERROR("editor.project_creation", "Operation %llu failed [code=%d]: %s",
                      static_cast<unsigned long long>(operation->snapshot.id), static_cast<int>(code),
                      operation->snapshot.error->message.c_str());
            state->dataBus.PublishAsync(revisionEvent);
        }

        void SetCancelled(const std::shared_ptr<ProjectCreationServiceState> &state,
                          const std::shared_ptr<ProjectCreationServiceState::Operation> &operation) {
            ProjectCreationRevisionChangedEvent revisionEvent;
            {
                std::lock_guard lock(state->synchronization.Mutex());
                if (IsTerminal(operation->snapshot.state))
                    return;
                operation->snapshot.state = ProjectCreationOperationState::Cancelled;
                operation->snapshot.phase = ProjectCreationOperationPhase::Cancelled;
                operation->snapshot.error = ProjectCreationError{ProjectCreationErrorCode::Cancelled, "Project creation was cancelled."};
                revisionEvent.revision = ++state->revision;
            }
            LOG_INFO("editor.project_creation", "Operation %llu cancelled.", static_cast<unsigned long long>(operation->snapshot.id));
            state->dataBus.PublishAsync(revisionEvent);
        }

        [[nodiscard]] bool IsCancelled(const CancellationToken &cancellation, const std::shared_ptr<ProjectCreationServiceState> &state,
                                       const std::shared_ptr<ProjectCreationServiceState::Operation> &operation) {
            if (!cancellation.IsCancellationRequested())
                return false;
            SetCancelled(state, operation);
            return true;
        }

        [[nodiscard]] std::string ProjectJson(const ProjectCreationRequest &request, const std::string &projectId) {
            const Application::EngineReleaseVersion release = Application::CurrentEngineReleaseVersion();
            const Application::ReleaseCompatibilityDecision *decision = Application::BuiltInReleaseCompatibilityRegistry().Find(release);
            if (decision == nullptr)
                std::terminate();

            return std::format(R"({{
  "horoVersion": "{}",
  "persistentContract": "{}",
  "projectId": "{}",
  "name": "{}",
  "projectVersion": "{}",
  "createdAt": "{}",
  "settings": {{
    "renderBackend": "{}",
    "physicsEnabled": {},
    "targetFrameRate": {},
    "defaultScene": "{}",
    "assetCompression": "{}",
    "textureCompression": "{}",
    "buildProfile": "{}",
    "requiredToolchain": {{
      "targetPlatform": "{}",
      "compilerFamily": "{}",
      "minimumCxxStandard": {}
    }}
  }}
}})",
                               Application::FormatHoroVersion(release.value),
                               Application::FormatPersistentContractHash(decision->persistentContract), EscapeJson(projectId),
                               EscapeJson(request.projectName), EscapeJson(request.projectVersion), UtcTimestamp(),
                               EscapeJson(request.renderBackend), request.physicsEnabled ? "true" : "false", request.targetFrameRate,
                               EscapeJson(request.defaultScene), EscapeJson(request.assetCompression),
                               EscapeJson(request.textureCompression), EscapeJson(request.buildProfile), EscapeJson(request.targetPlatform),
                               EscapeJson(request.compilerFamily), request.minimumCxxStandard);
        }

        struct StagingPreparationFailure {
            ProjectCreationErrorCode code;
            const char *message;
        };

        [[nodiscard]] std::optional<StagingPreparationFailure> PrepareStagingDirectory(const std::filesystem::path &destination,
                                                                                       const std::filesystem::path &parent,
                                                                                       const std::filesystem::path &staging,
                                                                                       std::error_code &error) {
            using enum ProjectCreationErrorCode;
            if (std::filesystem::exists(destination, error) &&
                (!std::filesystem::is_directory(destination, error) || !std::filesystem::is_empty(destination, error))) {
                return StagingPreparationFailure{DestinationOccupied, "Project destination became occupied before promotion."};
            }
            if (error)
                return StagingPreparationFailure{DestinationOccupied, "Project destination became occupied before promotion."};
            std::filesystem::create_directories(parent, error);
            if (error)
                return StagingPreparationFailure{ParentUnavailable, "Unable to create the project destination parent directory."};
            std::filesystem::create_directory(staging, error);
            if (error)
                return StagingPreparationFailure{StagingFailed, "Unable to create a sibling project staging directory."};
            return std::nullopt;
        }

        [[nodiscard]] std::optional<StagingPreparationFailure> WriteScaffolding(const std::filesystem::path &staging,
                                                                                const ProjectCreationRequest &request,
                                                                                const std::string &projectId) {
            std::error_code error;
            std::filesystem::create_directories(staging / ".horo", error);
            if (error || !WriteDurableFile(staging / ".horo/project.json", ProjectJson(request, projectId)) ||
                !WriteDurableFile(staging / ".horo/plugins.json", "{\n  \"schemaVersion\": 1,\n  \"requestedPlugins\": []\n}\n") ||
                !WriteDurableFile(staging / ".horo/input.json",
                                  "{\n  \"schemaVersion\": 1,\n  \"profileId\": \"project-default\",\n  \"overrides\": []\n}\n")) {
                return StagingPreparationFailure{ProjectCreationErrorCode::WriteFailed,
                                                 "Unable to write project metadata into the staging directory."};
            }

            for (const char *directory : {"Assets/Models", "Assets/Textures", "Assets/Materials", "Assets/Shaders", "Assets/Scenes"}) {
                std::filesystem::create_directories(staging / directory, error);
                if (error)
                    return StagingPreparationFailure{ProjectCreationErrorCode::WriteFailed, "Unable to create project asset scaffolding."};
            }

            if (request.includeStarterContent) {
                const std::filesystem::path starterScene = staging / request.defaultScene;
                std::filesystem::create_directories(starterScene.parent_path(), error);
                if (error || !WriteDurableFile(starterScene, "{\n  \"schemaVersion\": 1,\n  \"objects\": []\n}\n"))
                    return StagingPreparationFailure{ProjectCreationErrorCode::WriteFailed, "Unable to write the requested starter scene."};
            }

            if (request.initializeGit) {
                const std::string gitignore =
                    std::format("build/\n.horo/local/\n.horo/editor_workspace.json\n{}\n", ProjectLayout::AssetIndexPath);
                if (!WriteDurableFile(staging / ".gitignore", gitignore))
                    return StagingPreparationFailure{ProjectCreationErrorCode::WriteFailed, "Unable to write the project git ignore file."};
            }

            if (request.generateCMakeProject) {
                const std::string cmakeProject = "cmake_minimum_required(VERSION 3.25)\n"
                                                 "project(HoroGame LANGUAGES CXX)\n\n"
                                                 "find_package(HoroEngineGameplay CONFIG REQUIRED)\n\n"
                                                 "file(GLOB_RECURSE HORO_GAMEPLAY_SOURCES CONFIGURE_DEPENDS \"source/gameplay/*.cpp\")\n"
                                                 "if(HORO_GAMEPLAY_SOURCES)\n"
                                                 "    horo_add_gameplay_module(HoroGameGameplay\n"
                                                 "        MODULE_ID " +
                                                 GameplayModuleId(request.projectName) +
                                                 "\n"
                                                 "        SOURCES ${HORO_GAMEPLAY_SOURCES}\n"
                                                 "    )\n"
                                                 "endif()\n";
                std::filesystem::create_directories(staging / "source" / "gameplay", error);
                if (error || !WriteDurableFile(staging / "CMakeLists.txt", cmakeProject))
                    return StagingPreparationFailure{ProjectCreationErrorCode::WriteFailed, "Unable to write the project CMake file."};
            }

            return std::nullopt;
        }

        void RunCreate(const std::shared_ptr<ProjectCreationServiceState> &state,
                       const std::shared_ptr<ProjectCreationServiceState::Operation> &operation, const ProjectCreationRequest &request,
                       const CancellationToken &cancellation) {
            const std::filesystem::path destination = request.projectRoot;
            const std::filesystem::path parent =
                destination.has_parent_path() ? destination.parent_path() : std::filesystem::current_path();
            const std::filesystem::path staging =
                parent / std::format(".{}.horo-create-{}", destination.filename().string(), operation->snapshot.id);
            std::error_code error;
            auto cleanup = [&] {
                std::filesystem::remove_all(staging, error);
            };

            LOG_DEBUG("editor.project_creation", "Starting worker execution for operation %llu. Destination: '%s'",
                      static_cast<unsigned long long>(operation->snapshot.id), destination.string().c_str());
            LOG_DEBUG("editor.project_creation", "Resolved parent directory: '%s', staging directory: '%s'", parent.string().c_str(),
                      staging.string().c_str());

            UpdateProgress(state, operation, ProjectCreationOperationState::Running, ProjectCreationOperationPhase::Validating,
                           "validating", 0.02F);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (IsCancelled(cancellation, state, operation))
                return;

            if (std::filesystem::exists(destination, error) &&
                (!std::filesystem::is_directory(destination, error) || !std::filesystem::is_empty(destination, error))) {
                SetFailure(state, operation, ProjectCreationErrorCode::DestinationOccupied,
                           "Project destination already exists and will not be overwritten.");
                return;
            }
            if (error) {
                SetFailure(state, operation, ProjectCreationErrorCode::InvalidRequest, "Project destination cannot be inspected.");
                return;
            }

            UpdateProgress(state, operation, ProjectCreationOperationState::Running, ProjectCreationOperationPhase::Staging, "staging",
                           0.05F);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (IsCancelled(cancellation, state, operation))
                return;
            if (const auto failure = PrepareStagingDirectory(destination, parent, staging, error)) {
                SetFailure(state, operation, failure->code, failure->message);
                return;
            }

            UpdateProgress(state, operation, ProjectCreationOperationState::Running, ProjectCreationOperationPhase::WritingMetadata,
                           "metadata", 0.20F);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (IsCancelled(cancellation, state, operation)) {
                cleanup();
                return;
            }

            if (const auto failure = WriteScaffolding(staging, request, operation->snapshot.projectId)) {
                cleanup();
                SetFailure(state, operation, failure->code, failure->message);
                return;
            }

            UpdateProgress(state, operation, ProjectCreationOperationState::Running, ProjectCreationOperationPhase::WritingScaffolding,
                           "scaffolding", 0.50F);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (IsCancelled(cancellation, state, operation)) {
                cleanup();
                return;
            }

            UpdateProgress(state, operation, ProjectCreationOperationState::Running, ProjectCreationOperationPhase::Promoting, "promoting",
                           0.90F);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (IsCancelled(cancellation, state, operation)) {
                cleanup();
                return;
            }
            LOG_DEBUG("editor.project_creation", "Promoting staging directory '%s' to target destination '%s'", staging.string().c_str(),
                      destination.string().c_str());
            if (!PromoteWithoutReplace(staging, destination, error)) {
                cleanup();
                const ProjectCreationErrorCode code = error == std::errc::file_exists ? ProjectCreationErrorCode::DestinationOccupied
                                                                                      : ProjectCreationErrorCode::PromotionFailed;
                SetFailure(state, operation, code, "Unable to atomically promote the staged project directory.");
                return;
            }

            ProjectCreatedEvent created;
            ProjectCreationRevisionChangedEvent changed;
            {
                std::lock_guard lock(state->synchronization.Mutex());
                operation->snapshot.state = ProjectCreationOperationState::Succeeded;
                operation->snapshot.phase = ProjectCreationOperationPhase::Completed;
                static_cast<void>(operation->progress.Report("completed", 1.0F));
                operation->snapshot.progress = operation->progress.Snapshot().value;
                created.operationId = operation->snapshot.id;
                created.projectId = operation->snapshot.projectId;
                created.revision = ++state->revision;
                changed.revision = created.revision;
            }
            LOG_DEBUG("editor.project_creation", "Operation %llu completed cleanly. Publishing ProjectCreatedEvent (revision %llu)",
                      static_cast<unsigned long long>(created.operationId), static_cast<unsigned long long>(created.revision));
            state->dataBus.PublishAsync(created);
            state->dataBus.PublishAsync(changed);
        }

        [[nodiscard]] std::optional<Error> ValidateRequestSync(const ProjectCreationRequest &request) {
            if (Text::IsBlank(request.templateId) || Text::IsBlank(request.projectName) ||
                request.projectName.find('/') != std::string::npos || request.projectName.find('\\') != std::string::npos)
                return MakeFoundationError(ProjectCreationErrors::InvalidRequest,
                                           "Project name is required and must not contain path separators.");
            if (request.projectRoot.empty() || request.projectRoot.filename().empty())
                return MakeFoundationError(ProjectCreationErrors::InvalidRequest, "Project root must name a destination directory.");
            if (request.includeStarterContent) {
                if (!IsSafeProjectRelativePath(request.defaultScene) || std::filesystem::path(request.defaultScene).filename().empty())
                    return MakeFoundationError(ProjectCreationErrors::InvalidRequest,
                                               "Starter content requires a portable project-relative default scene path.");
            } else if (!request.defaultScene.empty()) {
                return MakeFoundationError(ProjectCreationErrors::InvalidRequest,
                                           "Default scene must be empty when starter content is excluded.");
            }
            if (request.targetFrameRate <= 0 || request.minimumCxxStandard < 20)
                return MakeFoundationError(ProjectCreationErrors::InvalidRequest, "Project numeric settings are outside supported bounds.");
            std::error_code error;
            if (std::filesystem::exists(request.projectRoot, error) &&
                (!std::filesystem::is_directory(request.projectRoot, error) || !std::filesystem::is_empty(request.projectRoot, error))) {
                return MakeFoundationError(ProjectCreationErrors::DestinationOccupied,
                                           "Project destination already exists and will not be overwritten.");
            }
            if (error)
                return MakeFoundationError(ProjectCreationErrors::InvalidRequest, "Project destination cannot be inspected.");
            return std::nullopt;
        }
    }  // namespace

    ProjectCreationService::ProjectCreationService(JobSystem &jobs, EngineDataBus &dataBus)
        : state_(std::make_shared<ProjectCreationServiceState>(dataBus)), jobs_(jobs) {}

    Result<ProjectCreationOperationHandle> ProjectCreationService::StartCreate(ProjectCreationRequest request) const {
        LOG_DEBUG("editor.project_creation", "StartCreate requested for project '%s' at path '%s', template '%s'",
                  request.projectName.c_str(), request.projectRoot.string().c_str(), request.templateId.c_str());
        if (const auto validation = ValidateRequestSync(request)) {
            LOG_DEBUG("editor.project_creation", "StartCreate validation failed: %s", validation->message.c_str());
            return Result<ProjectCreationOperationHandle>::Failure(*validation);
        }

        auto operation = std::make_shared<ProjectCreationServiceState::Operation>();
        {
            std::lock_guard lock(state_->synchronization.Mutex());
            operation->snapshot.id = state_->nextOperationId++;
            operation->snapshot.projectRoot = request.projectRoot;
            operation->snapshot.projectId = MakeProjectId(operation->snapshot.id);
            state_->operations.try_emplace(operation->snapshot.id, operation);
        }
        const auto submitted = jobs_.get().Submit(JobDescriptor{}, [state = state_, operation,
                                                                    request = std::move(request)](const CancellationToken &cancellation) {
            RunCreate(state, operation, request, cancellation);
        });
        if (submitted.HasError()) {
            std::lock_guard lock(state_->synchronization.Mutex());
            state_->operations.erase(operation->snapshot.id);
            LOG_DEBUG("editor.project_creation", "StartCreate job submission failed: %s", submitted.ErrorValue().message.c_str());
            return Result<ProjectCreationOperationHandle>::Failure(
                MakeFoundationError(ProjectCreationErrors::JobSubmissionFailed, submitted.ErrorValue().message.c_str()));
        }
        {
            std::lock_guard lock(state_->synchronization.Mutex());
            operation->jobId = submitted.Value().Id();
        }
        LOG_INFO("editor.project_creation", "Project creation started: '%s' path='%s' (op=%llu job=%llu).",
                 operation->snapshot.projectId.c_str(), operation->snapshot.projectRoot.string().c_str(),
                 static_cast<unsigned long long>(operation->snapshot.id), static_cast<unsigned long long>(operation->jobId));
        return Result<ProjectCreationOperationHandle>::Success(ProjectCreationOperationHandle{operation->snapshot.id});
    }

    std::optional<ProjectCreationSnapshot> ProjectCreationService::Query(const ProjectCreationOperationId id) const {
        std::lock_guard lock(state_->synchronization.Mutex());
        const auto found = state_->operations.find(id);
        if (found == state_->operations.end())
            return std::nullopt;
        return found->second->snapshot;
    }

    Result<void> ProjectCreationService::RequestCancel(const ProjectCreationOperationId id) const {
        JobId jobId = 0;
        std::shared_ptr<ProjectCreationServiceState::Operation> operation;
        {
            std::lock_guard lock(state_->synchronization.Mutex());
            const auto found = state_->operations.find(id);
            if (found == state_->operations.end()) {
                LOG_WARN("editor.project_creation", "RequestCancel: operation %llu not found.", static_cast<unsigned long long>(id));
                return Result<void>::Failure(
                    MakeFoundationError(ProjectCreationErrors::NotFound, "Project creation operation is not known."));
            }
            operation = found->second;
            if (IsTerminal(operation->snapshot.state)) {
                LOG_DEBUG("editor.project_creation", "RequestCancel: operation %llu already in terminal state.",
                          static_cast<unsigned long long>(id));
                return Result<void>::Success();
            }
            jobId = operation->jobId;
            operation->snapshot.state = ProjectCreationOperationState::Cancelling;
        }
        if (jobId == 0)
            return Result<void>::Failure(
                MakeFoundationError(ProjectCreationErrors::NotReady, "Project creation operation is not ready for cancellation."));
        if (const auto cancelled = jobs_.get().RequestCancel(jobId); cancelled.HasError())
            return cancelled;
        if (jobs_.get().Query(jobId).state == JobState::Cancelled)
            SetCancelled(state_, operation);
        LOG_INFO("editor.project_creation", "Cancellation requested for operation %llu (job=%llu).", static_cast<unsigned long long>(id),
                 static_cast<unsigned long long>(jobId));
        return Result<void>::Success();
    }

    void ProjectCreationService::PumpMainThread() const {
        state_->dataBus.DispatchQueued();
    }
}  // namespace Horo::Editor
