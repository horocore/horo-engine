#include "Horo/Editor/ProjectCreationController.h"

#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/String.h"
#include "editor/project_model/RendererAvailability.h"

#include <string_view>
#include <system_error>
#include <utility>

/**
 * @file ProjectCreationController.cpp
 * @brief Implementation of the project-creation screen controller and inspection logic.
 */

namespace Horo::Editor {
    namespace {
        /**
         * @brief Checks if a string contains file path separators.
         * @param value String to inspect.
         * @return True if separator found.
         */
        [[nodiscard]] bool HasPathSeparator(const std::string_view value) {
            return value.find('/') != std::string::npos || value.find('\\') != std::string::npos;
        }

        /**
         * @brief Checks if a file status contains any write permissions.
         * @param permissions Filesystem permissions to check.
         * @return True if owner, group, or other write is permitted.
         */
        [[nodiscard]] bool HasAnyWritePermission(const std::filesystem::perms permissions) {
            using enum std::filesystem::perms;
            constexpr auto writeBits = owner_write | group_write | others_write;
            return (permissions & writeBits) != std::filesystem::perms::none;
        }

        /**
         * @brief Traverses up from a path to find the nearest existing directory.
         * @param path Start path.
         * @return The first existing ancestor, or empty if root reached without success.
         */
        [[nodiscard]] std::filesystem::path FindNearestExistingParent(std::filesystem::path path) {
            std::error_code error;
            while (!path.empty()) {
                if (std::filesystem::exists(path, error)) {
                    return error ? std::filesystem::path{} : path;
                }
                if (error && error != std::errc::no_such_file_or_directory) {
                    return {};
                }
                error.clear();
                const std::filesystem::path parent = path.parent_path();
                if (parent == path) {
                    return {};
                }
                path = parent;
            }
            return {};
        }

        /**
         * @brief Converts a valid draft into its service-facing request without performing I/O.
         * @param draft Current project settings.
         * @return The constructed request.
         */
        [[nodiscard]] ProjectCreationRequest MakeRequest(const ProjectCreationDraft &draft) {
            return {draft.templateId,       draft.projectName,           std::filesystem::path{draft.projectPath},
                    draft.projectVersion,   draft.defaultScene,          draft.renderBackend,
                    draft.physicsEnabled,   draft.targetFrameRate,       draft.buildProfile,
                    draft.assetCompression, draft.textureCompression,    draft.targetPlatform,
                    draft.compilerFamily,   draft.minimumCxxStandard,    draft.initializeGit,
                    draft.restorePackages,  draft.includeStarterContent, draft.generateCMakeProject};
        }
    }  // namespace

    /** @copydoc InspectProjectCreationLocation */
    ProjectCreationLocation InspectProjectCreationLocation(const std::filesystem::path &path) {
        using enum ProjectCreationLocationKind;
        if (path.empty()) {
            return {};
        }

        std::error_code error;
        const std::filesystem::file_status status = std::filesystem::status(path, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            return {ProjectCreationLocationKind::Inaccessible, {}, false};
        }

        if (std::filesystem::exists(status)) {
            if (!std::filesystem::is_directory(status)) {
                return {ProjectCreationLocationKind::ExistingNonDirectory, path.parent_path(), false};
            }

            const bool parentAppearsWritable = HasAnyWritePermission(status.permissions());
            const bool isEmpty = std::filesystem::is_empty(path, error);
            if (error) {
                return {ProjectCreationLocationKind::Inaccessible, path, parentAppearsWritable};
            }
            return {isEmpty ? ProjectCreationLocationKind::EmptyDirectory : ProjectCreationLocationKind::OccupiedDirectory, path,
                    parentAppearsWritable};
        }

        const std::filesystem::path parent = FindNearestExistingParent(path.parent_path());
        if (parent.empty()) {
            return {ProjectCreationLocationKind::Inaccessible, {}, false};
        }

        const std::filesystem::file_status parentStatus = std::filesystem::status(parent, error);
        if (error || !std::filesystem::is_directory(parentStatus)) {
            return {ProjectCreationLocationKind::Inaccessible, parent, false};
        }
        return {ProjectCreationLocationKind::Missing, parent, HasAnyWritePermission(parentStatus.permissions())};
    }

    /** @copydoc ProjectCreationController::ProjectCreationController(const RendererAvailabilitySnapshot&) */
    ProjectCreationController::ProjectCreationController(const RendererAvailabilitySnapshot &availability)
        : draft_{initialDraft_}, rendererAvailability_(&availability) {
        initialDraft_.renderBackend = std::string{availability.DefaultSelectableBackendId()};
        draft_.renderBackend = initialDraft_.renderBackend;
    }

    /** @copydoc ProjectCreationController::Draft */
    const ProjectCreationDraft &ProjectCreationController::Draft() const noexcept {
        return draft_;
    }

    /** @copydoc ProjectCreationController::SetProjectName */
    void ProjectCreationController::SetProjectName(std::string projectName) {
        draft_.projectName = std::move(projectName);
    }

    /** @copydoc ProjectCreationController::SetProjectPath */
    void ProjectCreationController::SetProjectPath(std::string projectPath) {
        draft_.projectPath = std::move(projectPath);
    }

    /** @copydoc ProjectCreationController::SetTemplateId */
    void ProjectCreationController::SetTemplateId(std::string templateId) {
        if (draft_.templateId != templateId) {
            draft_.templateId = std::move(templateId);
            if (draft_.templateId == "empty") {
                draft_.includeStarterContent = false;
                draft_.defaultScene = "";
            } else if (draft_.templateId == "3d-starter" || draft_.templateId == "custom") {
                draft_.includeStarterContent = true;
                if (draft_.defaultScene.empty()) {
                    draft_.defaultScene = "Assets/Scenes/main.horo";
                }
            } else if (draft_.templateId == "first-person") {
                draft_.includeStarterContent = true;
                draft_.physicsEnabled = true;
                if (draft_.defaultScene.empty()) {
                    draft_.defaultScene = "Assets/Scenes/first_person.horo";
                }
            } else if (draft_.templateId == "tech-demo") {
                draft_.includeStarterContent = true;
                draft_.targetFrameRate = 120;
                if (draft_.defaultScene.empty()) {
                    draft_.defaultScene = "Assets/Scenes/benchmark.horo";
                }
            } else if (draft_.templateId == "package-based") {
                draft_.includeStarterContent = true;
                draft_.restorePackages = true;
                if (draft_.defaultScene.empty()) {
                    draft_.defaultScene = "Assets/Scenes/main.horo";
                }
            }
        }
    }

    /** @copydoc ProjectCreationController::MutableDraft */
    ProjectCreationDraft &ProjectCreationController::MutableDraft() noexcept {
        return draft_;
    }

    /** @copydoc ProjectCreationController::SetProjectVersion */
    void ProjectCreationController::SetProjectVersion(std::string projectVersion) {
        draft_.projectVersion = std::move(projectVersion);
    }

    /** @copydoc ProjectCreationController::SetDefaultScene */
    void ProjectCreationController::SetDefaultScene(std::string defaultScene) {
        draft_.defaultScene = std::move(defaultScene);
    }

    /** @copydoc ProjectCreationController::SetRenderBackend */
    void ProjectCreationController::SetRenderBackend(std::string renderBackend) {
        draft_.renderBackend = std::move(renderBackend);
    }

    /** @copydoc ProjectCreationController::SetPhysicsEnabled */
    void ProjectCreationController::SetPhysicsEnabled(const bool physicsEnabled) {
        draft_.physicsEnabled = physicsEnabled;
    }

    /** @copydoc ProjectCreationController::SetTargetFrameRate */
    void ProjectCreationController::SetTargetFrameRate(const int targetFrameRate) {
        draft_.targetFrameRate = targetFrameRate;
    }

    /** @copydoc ProjectCreationController::SetBuildProfile */
    void ProjectCreationController::SetBuildProfile(std::string buildProfile) {
        draft_.buildProfile = std::move(buildProfile);
    }

    /** @copydoc ProjectCreationController::SetAssetCompression */
    void ProjectCreationController::SetAssetCompression(std::string assetCompression) {
        draft_.assetCompression = std::move(assetCompression);
    }

    /** @copydoc ProjectCreationController::SetTextureCompression */
    void ProjectCreationController::SetTextureCompression(std::string textureCompression) {
        draft_.textureCompression = std::move(textureCompression);
    }

    /** @copydoc ProjectCreationController::SetTargetPlatform */
    void ProjectCreationController::SetTargetPlatform(std::string targetPlatform) {
        draft_.targetPlatform = std::move(targetPlatform);
    }

    /** @copydoc ProjectCreationController::SetCompilerFamily */
    void ProjectCreationController::SetCompilerFamily(std::string compilerFamily) {
        draft_.compilerFamily = std::move(compilerFamily);
    }

    /** @copydoc ProjectCreationController::SetMinimumCxxStandard */
    void ProjectCreationController::SetMinimumCxxStandard(const int minimumCxxStandard) {
        draft_.minimumCxxStandard = minimumCxxStandard;
    }

    /** @copydoc ProjectCreationController::SetInitializeGit */
    void ProjectCreationController::SetInitializeGit(const bool initializeGit) {
        draft_.initializeGit = initializeGit;
    }

    /** @copydoc ProjectCreationController::SetRestorePackages */
    void ProjectCreationController::SetRestorePackages(const bool restorePackages) {
        draft_.restorePackages = restorePackages;
    }

    /** @copydoc ProjectCreationController::SetIncludeStarterContent */
    void ProjectCreationController::SetIncludeStarterContent(const bool includeStarterContent) {
        draft_.includeStarterContent = includeStarterContent;
        if (!includeStarterContent) {
            draft_.defaultScene.clear();
        } else if (draft_.defaultScene.empty()) {
            draft_.defaultScene = "Assets/Scenes/main.horo";
        }
    }

    /** @copydoc ProjectCreationController::SetGenerateCMakeProject */
    void ProjectCreationController::SetGenerateCMakeProject(const bool generateCMakeProject) {
        draft_.generateCMakeProject = generateCMakeProject;
    }

    /** @copydoc ProjectCreationController::Validate */
    ProjectCreationValidation ProjectCreationController::Validate() const {
        using enum ProjectCreationDiagnosticCode;
        using enum ProjectCreationLocationKind;
        ProjectCreationValidation validation;
        if (rendererAvailability_ != nullptr) {
            const RendererBackendAvailability *backend = rendererAvailability_->Find(draft_.renderBackend);
            if (backend == nullptr || !backend->IsSelectable()) {
                validation.diagnostics.emplace_back(ProjectCreationDiagnosticCode::RendererBackendUnavailable,
                                                    backend != nullptr && !backend->diagnostic.empty()
                                                        ? backend->diagnostic
                                                        : "Selected renderer backend is unavailable on this editor installation.");
            }
        }
        if (Text::IsBlank(draft_.projectName)) {
            validation.diagnostics.emplace_back(ProjectNameRequired, "Project name is required.");
        } else if (HasPathSeparator(draft_.projectName)) {
            validation.diagnostics.emplace_back(ProjectNameContainsPathSeparator, "Project name must not contain path separators.");
        }

        if (Text::IsBlank(draft_.projectPath)) {
            validation.diagnostics.emplace_back(ProjectPathRequired, "Project location is required.");
            return validation;
        }

        switch (const ProjectCreationLocation location = InspectProjectCreationLocation(draft_.projectPath); location.kind) {
            case OccupiedDirectory:
                validation.diagnostics.emplace_back(ProjectPathOccupied,
                                                    "Project directory already contains files; choose an empty folder or import it.");
                break;
            case ExistingNonDirectory:
                validation.diagnostics.emplace_back(ProjectPathNotDirectory, "Project location exists but is not a directory.");
                break;
            case Inaccessible:
                validation.diagnostics.emplace_back(ProjectPathInaccessible, "Project location cannot be inspected.");
                break;
            case EmptyDirectory:
            case Missing:
                if (!location.parentAppearsWritable) {
                    validation.diagnostics.emplace_back(ProjectParentNotWritable, "Project location's parent does not appear writable.");
                }
                break;
        }
        return validation;
    }

    /** @copydoc ProjectCreationController::BuildCreationRequest */
    std::optional<ProjectCreationRequest> ProjectCreationController::BuildCreationRequest() const {
        if (const auto val = Validate(); !val.IsValid()) {
            LOG_DEBUG("editor.project_creation", "BuildCreationRequest failed validation (%zu diagnostics)", val.diagnostics.size());
            return std::nullopt;
        }
        LOG_DEBUG("editor.project_creation", "BuildCreationRequest succeeded for project '%s' at '%s'", draft_.projectName.c_str(),
                  draft_.projectPath.c_str());
        return MakeRequest(draft_);
    }

    /** @copydoc ProjectCreationController::IsDirty */
    bool ProjectCreationController::IsDirty() const noexcept {
        return draft_ != initialDraft_;
    }

    /** @copydoc ProjectCreationController::LeaveIntent */
    ProjectCreationLeaveIntent ProjectCreationController::LeaveIntent() const noexcept {
        return IsDirty() ? ProjectCreationLeaveIntent::RequireDiscardConfirmation : ProjectCreationLeaveIntent::Allow;
    }

    /** @copydoc ProjectCreationController::DiscardDraft */
    void ProjectCreationController::DiscardDraft() {
        draft_ = initialDraft_;
    }
}  // namespace Horo::Editor
