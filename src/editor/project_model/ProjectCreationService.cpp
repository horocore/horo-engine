#include "Horo/Editor/ProjectCreationService.h"

#include "../physics/CollisionIdentityGeneration.h"
#include "../physics/ProjectCollisionTemplate.h"
#include "Horo/Application/PlatformServicesAuthoring.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectSurfaceSettings.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Progress.h"
#include "Horo/Foundation/String.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/Security/SecureMemory.h"
#include "Horo/Security/SecurityErrors.h"
#include "editor/EditorServiceErrors.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <exception>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        /** @brief Encodes native path diagnostics without a Windows locale-dependent narrow conversion. */
        [[nodiscard]] std::string Utf8Path(const std::filesystem::path &path) {
            const auto text = path.u8string();
            return {text.begin(), text.end()};
        }

        [[nodiscard]] Error MakeFoundationError(const ErrorCodeDescriptor &descriptor, const char *message) {
            return MakeError(descriptor, message);
        }

        [[nodiscard]] bool IsSafeProjectRelativePath(const std::string &value) {
            if (value.empty() || value.find('\0') != std::string::npos || !IsValidUtf8ScalarSequence(value))
                return false;
            const std::filesystem::path path{std::u8string{value.begin(), value.end()}};
            if (path.has_root_path())
                return false;
            for (const auto &component : path) {
                auto name = component.native();
                std::ranges::transform(name, name.begin(), [](const std::filesystem::path::value_type character) {
                    return character >= 'A' && character <= 'Z' ? static_cast<std::filesystem::path::value_type>(character + ('a' - 'A'))
                                                                : character;
                });
                if (name == std::filesystem::path{".."}.native() || name == std::filesystem::path{".horo"}.native())
                    return false;
            }
            return true;
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

        /** @brief Writes only within an exclusively owned staging tree and retains original typed failures. */
        class CreationWriter final {
        public:
            CreationWriter(DurableFileSystem &files, std::filesystem::path root)
                : files_(files), root_(std::move(root)), directories_{root_} {}

            bool Fail(Error error) {
                if (!failure_)
                    failure_ = std::move(error);
                return false;
            }

            bool Directory(const std::filesystem::path &path) {
                const auto relative = path.lexically_relative(root_);
                if (relative.is_absolute() || relative.empty() || *relative.begin() == "..")
                    return Fail(MakeError(ProjectCreationErrors::InvalidRequest, "Staging directory escapes its owned root."));
                auto current = root_;
                for (const auto &component : relative) {
                    if (component == ".")
                        continue;
                    current /= component;
                    if (std::ranges::find(directories_, current) != directories_.end())
                        continue;
                    if (directories_.size() >= 256)
                        return Fail(
                            MakeError(ProjectCreationErrors::InvalidRequest, "Project scaffolding exceeds its 256-directory bound."));
                    auto created = files_.CreateDirectoryPath(current, DirectoryCreationMode::Single);
                    if (created.HasError())
                        return Fail(std::move(created).ErrorValue());
                    if (!created.Value())
                        return Fail(MakeError(ProjectCreationErrors::DestinationOccupied, "An unowned staging entry already exists."));
                    directories_.push_back(current);
                }
                return true;
            }

            bool Write(const std::filesystem::path &path, const std::string_view text) {
                if (failure_ || !Directory(path.parent_path()))
                    return false;
                auto written = files_.WriteDurable(path, std::as_bytes(std::span{text.data(), text.size()}));
                return written.HasValue() || Fail(std::move(written).ErrorValue());
            }

            bool Sync() {
                for (auto path = directories_.rbegin(); path != directories_.rend(); ++path) {
                    auto synced = files_.SyncDirectory(*path);
                    if (synced.HasError())
                        return Fail(std::move(synced).ErrorValue());
                }
                return true;
            }

            const std::optional<Error> &Failure() const noexcept {
                return failure_;
            }

        private:
            DurableFileSystem &files_;
            std::filesystem::path root_;
            std::vector<std::filesystem::path> directories_;
            std::optional<Error> failure_;
        };

        /** @brief Deletes only a sibling directory exclusively created by this operation; never removes published output. */
        class OwnedStaging final {
        public:
            OwnedStaging(std::filesystem::path parent, std::filesystem::path path, const AtomicFileReplacementReceipt &receipt)
                : path_(std::move(path)), receipt_(receipt),
                  safe_(path_.is_absolute() && path_ == path_.lexically_normal() && path_.parent_path() == parent && path_ != parent) {}

            ~OwnedStaging() {
                Cleanup();
            }

            OwnedStaging(const OwnedStaging &) = delete;
            OwnedStaging &operator=(const OwnedStaging &) = delete;

            bool Safe() const noexcept {
                return safe_;
            }

            void Own() noexcept {
                owned_ = true;
            }

            void Published() noexcept {
                owned_ = false;
            }

            void Cleanup() noexcept {
                if (!safe_ || !owned_ || receipt_.WasCommitted())
                    return;
                try {
                    std::error_code ignored;
                    std::filesystem::remove_all(path_, ignored);
                } catch (...) {  // ADR-008: failed cleanup retains private staging; destruction must not terminate the operation.
                }
                owned_ = false;
            }

        private:
            std::filesystem::path path_;
            const AtomicFileReplacementReceipt &receipt_;
            bool safe_{};
            bool owned_{};
        };

        /** @brief Removes only an explicitly accepted empty destination; publication itself remains atomic no-replace. */
        [[nodiscard]] bool DestinationAbsent(const std::filesystem::path &destination, std::error_code &error) {
            const auto status = std::filesystem::symlink_status(destination, error);
            if (error == std::errc::no_such_file_or_directory)
                error.clear();
            if (error)
                return false;
            if (status.type() == std::filesystem::file_type::not_found)
                return true;
            if (status.type() != std::filesystem::file_type::directory || !std::filesystem::is_empty(destination, error) || error) {
                error = std::make_error_code(std::errc::file_exists);
                return false;
            }
            return std::filesystem::remove(destination, error) && !error;
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
            // Worker-only commit facts; all other threads read the mutex-protected public snapshot.
            AtomicFileReplacementReceipt publicationReceipt;
            bool durabilityConfirmed{};
            ProjectCreationError beforeException{ProjectCreationErrorCode::UnexpectedFailure,
                                                 "Project creation stopped before publication.",
                                                 MakeError(ProjectCreationErrors::UnexpectedFailure)};
            ProjectCreationError afterException{ProjectCreationErrorCode::DurabilityUnconfirmed,
                                                "Project files were published but durability could not be confirmed.",
                                                MakeError(ProjectCreationErrors::UnexpectedFailure)};
        };

        explicit ProjectCreationServiceState(EngineDataBus &bus, std::shared_ptr<Security::SecureRandomSource> entropy,
                                             std::shared_ptr<DurableFileSystem> writer)
            : dataBus(bus), random(std::move(entropy)), files(std::move(writer)) {}

        EngineDataBus &dataBus;
        std::shared_ptr<Security::SecureRandomSource> random;
        std::shared_ptr<DurableFileSystem> files;
        ProjectCreationOperationId nextOperationId = 1;
        ProjectCreationRevision revision = 0;
        std::unordered_map<ProjectCreationOperationId, std::shared_ptr<Operation>> operations;
    };

    namespace {
        /** @brief Completes an exceptional job using only preallocated operation storage; published output is never rolled back. */
        void SetUnexpectedFailure(const std::shared_ptr<ProjectCreationServiceState> &state,
                                  const std::shared_ptr<ProjectCreationServiceState::Operation> &operation) {
            static_assert(std::is_nothrow_move_constructible_v<ProjectCreationError>);
            static_assert(std::is_nothrow_move_assignable_v<ProjectCreationError>);
            ProjectCreationRevisionChangedEvent changed;
            {
                std::lock_guard lock(state->synchronization.Mutex());
                if (IsTerminal(operation->snapshot.state))
                    return;
                if (operation->durabilityConfirmed) {
                    operation->snapshot.state = ProjectCreationOperationState::Succeeded;
                    operation->snapshot.phase = ProjectCreationOperationPhase::Completed;
                    operation->snapshot.publication = ProjectCreationPublicationState::Durable;
                    operation->snapshot.progress = 1.0F;
                } else {
                    const bool committed = operation->publicationReceipt.WasCommitted();
                    operation->snapshot.state = ProjectCreationOperationState::Failed;
                    operation->snapshot.phase = ProjectCreationOperationPhase::Failed;
                    operation->snapshot.publication =
                        committed ? ProjectCreationPublicationState::ReconciliationRequired : ProjectCreationPublicationState::Unpublished;
                    operation->snapshot.error.emplace(std::move(committed ? operation->afterException : operation->beforeException));
                }
                changed.revision = ++state->revision;
            }
            try {
                state->dataBus.PublishAsync(changed);
            } catch (...) {  // ADR-008: allocation/notification failure cannot undo the authoritative terminal snapshot.
            }
        }

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
                        std::string message, std::optional<Error> cause = {},
                        const ProjectCreationPublicationState publication = ProjectCreationPublicationState::Unpublished) {
            ProjectCreationRevisionChangedEvent revisionEvent;
            {
                std::lock_guard lock(state->synchronization.Mutex());
                if (IsTerminal(operation->snapshot.state))
                    return;
                operation->snapshot.state = ProjectCreationOperationState::Failed;
                operation->snapshot.phase = ProjectCreationOperationPhase::Failed;
                operation->snapshot.publication = publication;
                operation->snapshot.error = ProjectCreationError{code, std::move(message), std::move(cause)};
                revisionEvent.revision = ++state->revision;
            }
            LOG_ERROR("editor.project_creation", "Operation %llu failed [code=%d]: %s",
                      static_cast<unsigned long long>(operation->snapshot.id), static_cast<int>(code),
                      operation->snapshot.error->message.c_str());
            state->dataBus.PublishAsync(revisionEvent);
        }

        /** @brief Preserves the original typed identity cause and its message before either is moved into the terminal snapshot. */
        void SetIdentityFailure(const std::shared_ptr<ProjectCreationServiceState> &state,
                                const std::shared_ptr<ProjectCreationServiceState::Operation> &operation, Error cause) {
            const std::string message = cause.message;
            SetFailure(state, operation, ProjectCreationErrorCode::IdentityPreparationFailed, message, std::move(cause));
        }

        /** @brief Commits cancellation with optional scheduler provenance, without replacing terminal publication truth. */
        void SetCancelled(const std::shared_ptr<ProjectCreationServiceState> &state,
                          const std::shared_ptr<ProjectCreationServiceState::Operation> &operation, std::optional<Error> cause = {}) {
            ProjectCreationRevisionChangedEvent revisionEvent;
            {
                std::lock_guard lock(state->synchronization.Mutex());
                if (IsTerminal(operation->snapshot.state))
                    return;
                operation->snapshot.state = ProjectCreationOperationState::Cancelled;
                operation->snapshot.phase = ProjectCreationOperationPhase::Cancelled;
                operation->snapshot.error =
                    ProjectCreationError{ProjectCreationErrorCode::Cancelled, "Project creation was cancelled.", std::move(cause)};
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
            std::optional<Error> cause;
        };

        [[nodiscard]] std::optional<StagingPreparationFailure> PrepareStagingDirectory(DurableFileSystem &files,
                                                                                       const std::filesystem::path &parent,
                                                                                       const std::filesystem::path &staging,
                                                                                       OwnedStaging &owner, bool &createdParent) {
            if (!owner.Safe())
                return StagingPreparationFailure{ProjectCreationErrorCode::StagingFailed, "Staging is not an absolute owned sibling."};
            auto parents = files.CreateDirectoryPath(parent, DirectoryCreationMode::Parents);
            if (parents.HasError())
                return StagingPreparationFailure{ProjectCreationErrorCode::ParentUnavailable,
                                                 "Unable to create project parent directories.", std::move(parents).ErrorValue()};
            createdParent = parents.Value();
            auto created = files.CreateDirectoryPath(staging, DirectoryCreationMode::Single);
            if (created.HasError())
                return StagingPreparationFailure{ProjectCreationErrorCode::StagingFailed, "Unable to create a sibling staging directory.",
                                                 std::move(created).ErrorValue()};
            if (!created.Value())
                return StagingPreparationFailure{ProjectCreationErrorCode::StagingFailed,
                                                 "The sibling staging directory is already owned by another operation."};
            owner.Own();
            return std::nullopt;
        }

        /** @brief Writes the explicit validated template only inside the unpublished whole-project staging directory. */
        [[nodiscard]] bool WriteCollisionScaffolding(CreationWriter &writer, const std::filesystem::path &staging,
                                                     const std::string &projectId) {
            auto collision = CreateProjectCollisionTemplate(projectId);
            if (collision.HasError())
                return writer.Fail(std::move(collision).ErrorValue());
            auto source = Application::EncodeProjectCollisionSettings(collision.Value());
            if (source.HasError())
                return writer.Fail(std::move(source).ErrorValue());
            return writer.Write(staging / ".horo/collision.json", source.Value());
        }

        /** @brief Authors one real template surface UUID only in the unpublished new-project directory. */
        [[nodiscard]] bool WriteSurfaceScaffolding(CreationWriter &writer, const std::filesystem::path &staging,
                                                   const std::string &projectId) {
            auto bytes = AllocateCollisionIdentityBytes(nullptr);
            if (bytes.HasError())
                return writer.Fail(std::move(bytes).ErrorValue());
            const auto id = Physics::SurfaceMaterialId::FromBytes(bytes.Value());
            Application::ProjectSurfaceSettingsInput input;
            input.projectId = projectId;
            input.defaultSurfaceId = id;
            input.surfaces.push_back({id, "Default"});
            auto settings = Application::ProjectSurfaceSettings::Create(std::move(input));
            if (settings.HasError())
                return writer.Fail(std::move(settings).ErrorValue());
            auto source = Application::EncodeProjectSurfaceSettings(settings.Value());
            if (source.HasError())
                return writer.Fail(std::move(source).ErrorValue());
            return writer.Write(staging / ".horo/surfaces.json", source.Value());
        }

        /** @brief Writes portable root metadata before adding semantic settings to the staged project. */
        [[nodiscard]] bool WritePortableScaffolding(CreationWriter &writer, const std::filesystem::path &staging,
                                                    const Application::PlatformServicesInitialDocuments &initial) {
            if (!writer.Write(staging / ".horo/project.json", initial.projectSource) ||
                !writer.Write(staging / ".horo/plugins.json", "{\n  \"schemaVersion\": 1,\n  \"requestedPlugins\": []\n}\n") ||
                !writer.Write(staging / ".horo/input.json",
                              "{\n  \"schemaVersion\": 1,\n  \"profileId\": \"project-default\",\n  \"overrides\": []\n}\n")) {
                return false;
            }

            const std::array<std::pair<std::string_view, std::string_view>, 5> sources{
                {{Application::PlatformServicesDocumentPaths::Ledger, initial.ledgerSource},
                 {Application::PlatformServicesDocumentPaths::Achievements, initial.achievementSource},
                 {Application::PlatformServicesDocumentPaths::Stats, initial.statSource},
                 {Application::PlatformServicesDocumentPaths::Leaderboards, initial.leaderboardSource},
                 {Application::PlatformServicesDocumentPaths::Presence, initial.presenceSource}}};
            for (const auto &[path, text] : sources)
                if (!writer.Write(staging / path, text))
                    return false;
            return true;
        }

        /** @brief Creates the ordinary asset directories without choosing any surface semantics. */
        [[nodiscard]] bool WriteAssetScaffolding(CreationWriter &writer, const std::filesystem::path &staging) {
            for (const char *directory : {"Assets/Models", "Assets/Textures", "Assets/Materials", "Assets/Shaders", "Assets/Scenes"}) {
                if (!writer.Directory(staging / directory))
                    return false;
            }

            return true;
        }

        /** @brief Writes only explicitly requested starter/build/VCS authoring into unpublished staging. */
        [[nodiscard]] std::optional<StagingPreparationFailure> WriteOptionalScaffolding(CreationWriter &writer,
                                                                                        const std::filesystem::path &staging,
                                                                                        const ProjectCreationRequest &request) {
            if (request.includeStarterContent) {
                const std::filesystem::path starterScene =
                    staging / std::filesystem::path{std::u8string{request.defaultScene.begin(), request.defaultScene.end()}};
                if (!writer.Write(starterScene, "{\n  \"schemaVersion\": 1,\n  \"objects\": []\n}\n"))
                    return StagingPreparationFailure{ProjectCreationErrorCode::WriteFailed, "Unable to write the requested starter scene."};
            }

            if (request.initializeGit) {
                const std::string gitignore =
                    std::format("build/\n.horo/local/\n.horo/editor_workspace.json\n{}\n", ProjectLayout::AssetIndexPath);
                if (!writer.Write(staging / ".gitignore", gitignore))
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
                if (!writer.Directory(staging / "source" / "gameplay") || !writer.Write(staging / "CMakeLists.txt", cmakeProject))
                    return StagingPreparationFailure{ProjectCreationErrorCode::WriteFailed, "Unable to write the project CMake file."};
            }

            return std::nullopt;
        }

        /** @brief Publishes no project until all complete staging phases succeed. */
        [[nodiscard]] std::optional<StagingPreparationFailure> WriteScaffolding(
            CreationWriter &writer, const std::filesystem::path &staging, const ProjectCreationRequest &request,
            const std::string &projectId, const Application::PlatformServicesInitialDocuments &initial) {
            using enum Horo::Editor::ProjectCreationErrorCode;
            if (!WritePortableScaffolding(writer, staging, initial))
                return StagingPreparationFailure{WriteFailed, "Unable to write project metadata into the staging directory."};
            if (!WriteCollisionScaffolding(writer, staging, projectId))
                return StagingPreparationFailure{WriteFailed, "Unable to write project collision settings."};
            if (!WriteSurfaceScaffolding(writer, staging, projectId))
                return StagingPreparationFailure{WriteFailed, "Unable to write project surface settings."};
            if (!WriteAssetScaffolding(writer, staging))
                return StagingPreparationFailure{WriteFailed, "Unable to create project asset scaffolding."};
            return WriteOptionalScaffolding(writer, staging, request);
        }

        void RunCreate(const std::shared_ptr<ProjectCreationServiceState> &state,
                       const std::shared_ptr<ProjectCreationServiceState::Operation> &operation, const ProjectCreationRequest &request,
                       const CancellationToken &cancellation) {
            const std::filesystem::path destination = request.projectRoot;
            const std::filesystem::path parent = destination.parent_path();
            std::filesystem::path stagingName{"."};
            stagingName += destination.filename();
            stagingName += std::format(".horo-create-{}", operation->snapshot.id);
            const std::filesystem::path staging = parent / stagingName;
            OwnedStaging stagingOwner{parent, staging, operation->publicationReceipt};
            std::error_code error;
            if (!state->files) {
                SetFailure(state, operation, ProjectCreationErrorCode::WriteFailed, "The host did not supply a durable project writer.");
                return;
            }

            LOG_DEBUG("editor.project_creation", "Starting worker execution for operation %llu. Destination: '%s'",
                      static_cast<unsigned long long>(operation->snapshot.id), Utf8Path(destination).c_str());
            LOG_DEBUG("editor.project_creation", "Resolved parent directory: '%s', staging directory: '%s'", Utf8Path(parent).c_str(),
                      Utf8Path(staging).c_str());

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

            PlatformServices::PlatformServicesIdSalt salt;
            auto entropy = [&] {
                std::lock_guard lock(state->synchronization.Mutex());
                if (!state->random)
                    return Result<void>::Failure(MakeError(SecurityErrors::EntropyUnavailable));
                return state->random->Fill(salt.bytes);
            }();
            if (entropy.HasError()) {
                SetIdentityFailure(state, operation, std::move(entropy).ErrorValue());
                return;
            }
            auto initial = Application::PreparePlatformServicesInitialDocuments(ProjectJson(request, operation->snapshot.projectId), salt);
            if (initial.HasError()) {
                SetIdentityFailure(state, operation, std::move(initial).ErrorValue());
                return;
            }
            UpdateProgress(state, operation, ProjectCreationOperationState::Running, ProjectCreationOperationPhase::Staging, "staging",
                           0.05F);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (IsCancelled(cancellation, state, operation))
                return;
            bool createdParent{};
            if (const auto failure = PrepareStagingDirectory(*state->files, parent, staging, stagingOwner, createdParent)) {
                SetFailure(state, operation, failure->code, failure->message, failure->cause);
                return;
            }
            CreationWriter writer{*state->files, staging};

            UpdateProgress(state, operation, ProjectCreationOperationState::Running, ProjectCreationOperationPhase::WritingMetadata,
                           "metadata", 0.20F);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (cancellation.IsCancellationRequested()) {
                stagingOwner.Cleanup();
                SetCancelled(state, operation);
                return;
            }

            if (const auto failure = WriteScaffolding(writer, staging, request, operation->snapshot.projectId, initial.Value())) {
                stagingOwner.Cleanup();
                SetFailure(state, operation, failure->code, failure->message, writer.Failure());
                return;
            }
            if (!writer.Sync()) {
                stagingOwner.Cleanup();
                SetFailure(state, operation, ProjectCreationErrorCode::WriteFailed, "Unable to synchronize staged project directories.",
                           writer.Failure());
                return;
            }
            if (createdParent) {
                for (auto directory = parent; !directory.empty(); directory = directory.parent_path()) {
                    auto synced = state->files->SyncDirectory(directory);
                    if (synced.HasError()) {
                        stagingOwner.Cleanup();
                        SetFailure(state, operation, ProjectCreationErrorCode::WriteFailed,
                                   "Unable to synchronize project parent directories.", std::move(synced).ErrorValue());
                        return;
                    }
                    if (directory == directory.root_path())
                        break;
                }
            }

            UpdateProgress(state, operation, ProjectCreationOperationState::Running, ProjectCreationOperationPhase::WritingScaffolding,
                           "scaffolding", 0.50F);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (cancellation.IsCancellationRequested()) {
                stagingOwner.Cleanup();
                SetCancelled(state, operation);
                return;
            }

            UpdateProgress(state, operation, ProjectCreationOperationState::Running, ProjectCreationOperationPhase::Promoting, "promoting",
                           0.90F);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (cancellation.IsCancellationRequested()) {
                stagingOwner.Cleanup();
                SetCancelled(state, operation);
                return;
            }
            LOG_DEBUG("editor.project_creation", "Promoting staging directory '%s' to target destination '%s'", Utf8Path(staging).c_str(),
                      Utf8Path(destination).c_str());
            ProjectCreatedEvent created{.operationId = operation->snapshot.id, .projectId = operation->snapshot.projectId};
            ProjectCreationRevisionChangedEvent changed;
            if (!DestinationAbsent(destination, error)) {
                stagingOwner.Cleanup();
                SetFailure(state, operation, ProjectCreationErrorCode::DestinationOccupied,
                           "Project destination became occupied before publication.");
                return;
            }
            auto &receipt = operation->publicationReceipt;
            auto published = state->files->PublishDirectoryNoReplaceTracked(staging, destination, receipt);
            if (receipt.WasCommitted())
                stagingOwner.Published();
            if (published.HasError()) {
                stagingOwner.Cleanup();
                auto failure = std::move(receipt.WasCommitted() ? operation->afterException : operation->beforeException);
                if (!receipt.WasCommitted())
                    failure.code = ProjectCreationErrorCode::PromotionFailed;
                failure.cause = std::move(published).ErrorValue();
                SetFailure(state, operation, failure.code, std::move(failure.message), std::move(failure.cause),
                           receipt.WasCommitted() ? ProjectCreationPublicationState::ReconciliationRequired
                                                  : ProjectCreationPublicationState::Unpublished);
                return;
            }
            if (!receipt.WasCommitted()) {
                stagingOwner.Cleanup();
                auto failure = std::move(operation->beforeException);
                SetFailure(state, operation, ProjectCreationErrorCode::PromotionFailed, std::move(failure.message),
                           std::move(failure.cause));
                return;
            }
            operation->durabilityConfirmed = true;
            {
                std::lock_guard lock(state->synchronization.Mutex());
                operation->snapshot.state = ProjectCreationOperationState::Succeeded;
                operation->snapshot.publication = ProjectCreationPublicationState::Durable;
                operation->snapshot.phase = ProjectCreationOperationPhase::Completed;
                operation->snapshot.progress = 1.0F;
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
                if (!IsSafeProjectRelativePath(request.defaultScene) ||
                    std::filesystem::path{std::u8string{request.defaultScene.begin(), request.defaultScene.end()}}.filename().empty())
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

    /** @copydoc ProjectCreationService::ProjectCreationService */
    ProjectCreationService::ProjectCreationService(JobSystem &jobs, EngineDataBus &dataBus,
                                                   std::shared_ptr<Security::SecureRandomSource> random,
                                                   std::shared_ptr<DurableFileSystem> files)
        : state_(std::make_shared<ProjectCreationServiceState>(dataBus, std::move(random), std::move(files))), jobs_(jobs) {}

    Result<ProjectCreationOperationHandle> ProjectCreationService::StartCreate(ProjectCreationRequest request) const {
        if (request.projectRoot.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
            return Result<ProjectCreationOperationHandle>::Failure(
                MakeFoundationError(ProjectCreationErrors::InvalidRequest, "Project destination contains an embedded NUL."));
        if (!request.projectRoot.empty()) {
            std::error_code error;
            request.projectRoot = std::filesystem::absolute(request.projectRoot, error).lexically_normal();
            if (error || std::distance(request.projectRoot.begin(), request.projectRoot.end()) > 256)
                return Result<ProjectCreationOperationHandle>::Failure(
                    MakeFoundationError(ProjectCreationErrors::InvalidRequest,
                                        "Project destination cannot be normalized within its path-depth bound."));
        }
        LOG_DEBUG("editor.project_creation", "StartCreate requested for project '%s' at path '%s', template '%s'",
                  request.projectName.c_str(), Utf8Path(request.projectRoot).c_str(), request.templateId.c_str());
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
            try {
                RunCreate(state, operation, request, cancellation);
            } catch (...) {  // ADR-008: every exceptional host boundary must leave a terminal public operation, retaining commit truth.
                SetUnexpectedFailure(state, operation);
            }
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
        try {
            LOG_INFO("editor.project_creation", "Project creation started: '%s' path='%s' (op=%llu job=%llu).",
                     operation->snapshot.projectId.c_str(), Utf8Path(operation->snapshot.projectRoot).c_str(),
                     static_cast<unsigned long long>(operation->snapshot.id), static_cast<unsigned long long>(operation->jobId));
        } catch (...) {  // ADR-008: logging cannot hide the already accepted operation handle.
        }
        return Result<ProjectCreationOperationHandle>::Success(ProjectCreationOperationHandle{operation->snapshot.id});
    }

    /** @copydoc ProjectCreationService::Query */
    std::optional<ProjectCreationSnapshot> ProjectCreationService::Query(const ProjectCreationOperationId id) const {
        std::shared_ptr<ProjectCreationServiceState::Operation> operation;
        JobId jobId{};
        {
            std::lock_guard lock(state_->synchronization.Mutex());
            const auto found = state_->operations.find(id);
            if (found == state_->operations.end())
                return std::nullopt;
            operation = found->second;
            if (IsTerminal(operation->snapshot.state))
                return operation->snapshot;
            jobId = operation->jobId;
        }
        // Never query the scheduler while holding the operation mutex. Queued revocation skips our callback entirely;
        // a retained exact Cancelled record is required, and SetCancelled cannot replace an already terminal publication.
        if (jobId != 0) {
            auto job = jobs_.get().Find(jobId);
            if (job && job->state == JobState::Cancelled)
                SetCancelled(state_, operation, std::move(job->error));
        }
        std::lock_guard lock(state_->synchronization.Mutex());
        return operation->snapshot;
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
