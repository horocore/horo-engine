#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Editor {
    class RendererAvailabilitySnapshot;

    /**
     * @file ProjectCreationController.h
     * @brief Headless workflow contract for the project-creation controller and validation logic.
     */

    /** @brief Filesystem state of a requested project destination. */
    enum class ProjectCreationLocationKind {
        Missing,
        EmptyDirectory,
        OccupiedDirectory,
        ExistingNonDirectory,
        Inaccessible,
    };

    /** @brief Result of a non-mutating project-destination inspection. */
    struct ProjectCreationLocation {
        ProjectCreationLocationKind kind = ProjectCreationLocationKind::Inaccessible;
        std::filesystem::path nearestExistingParent;
        bool parentAppearsWritable = false;
    };

    /** @brief Stable code for a project-creation validation diagnostic. */
    enum class ProjectCreationDiagnosticCode {
        ProjectNameRequired,
        ProjectNameContainsPathSeparator,
        ProjectPathRequired,
        ProjectPathOccupied,
        ProjectPathNotDirectory,
        ProjectPathInaccessible,
        ProjectParentNotWritable,
        RendererBackendUnavailable,
    };

    /** @brief Validation diagnostic presented by the project-creation screen. */
    struct ProjectCreationDiagnostic {
        ProjectCreationDiagnosticCode code;
        std::string message;
    };

    /** @brief Typed validation result for the current project-creation draft. */
    struct ProjectCreationValidation {
        std::vector<ProjectCreationDiagnostic> diagnostics;

        /** @brief Reports whether no blocking validation diagnostics were produced. */
        [[nodiscard]] bool IsValid() const noexcept {
            return diagnostics.empty();
        }
    };

    /** @brief Typed, portable draft values collected by the project-creation screen. */
    struct ProjectCreationDraft {
        std::string templateId = "3d-starter";
        std::string projectName;
        std::string projectPath;
        std::string projectVersion = "0.1.0";
        std::string defaultScene = "Assets/Scenes/main.horo";
        std::string renderBackend = "opengl";
        bool physicsEnabled = true;
        int targetFrameRate = 60;
        std::string buildProfile = "desktop-debug";
        std::string assetCompression = "lz4";
        std::string textureCompression = "bc7";
        std::string targetPlatform = "host";
        std::string compilerFamily = "default";
        int minimumCxxStandard = 20;
        bool initializeGit = true;
        bool restorePackages = true;
        bool includeStarterContent = true;
        bool generateCMakeProject = true;

        [[nodiscard]] bool operator==(const ProjectCreationDraft &) const = default;
    };

    /** @brief Validated input that a future ProjectCreationService may consume. */
    struct ProjectCreationRequest {
        std::string templateId;
        std::string projectName;
        std::filesystem::path projectRoot;
        std::string projectVersion;
        std::string defaultScene;
        std::string renderBackend;
        bool physicsEnabled = true;
        int targetFrameRate = 60;
        std::string buildProfile;
        std::string assetCompression;
        std::string textureCompression;
        std::string targetPlatform;
        std::string compilerFamily;
        int minimumCxxStandard = 20;
        bool initializeGit = true;
        bool restorePackages = true;
        bool includeStarterContent = true;
        bool generateCMakeProject = false;
    };

    /** @brief Leave policy requested by a project-creation draft. */
    enum class ProjectCreationLeaveIntent {
        Allow,
        RequireDiscardConfirmation,
    };

    /**
     * @brief Inspects a destination without creating a project directory or metadata.
     * @param path Candidate project root.
     * @return Existing/missing destination state and a conservative parent-write indication.
     */
    [[nodiscard]] ProjectCreationLocation InspectProjectCreationLocation(const std::filesystem::path &path);

    /** @brief Headless state and workflow controller for the ProjectCreation route. */
    class ProjectCreationController {
    public:
        /**
         * @brief Creates the controller with the active machine renderer as its backend default.
         * @param availability Machine-local renderer availability used for defaulting and validation.
         */
        explicit ProjectCreationController(const RendererAvailabilitySnapshot &availability);

        /** @brief Returns the current typed draft. */
        [[nodiscard]] const ProjectCreationDraft &Draft() const noexcept;

        /** @brief Updates the project name draft value. */
        void SetProjectName(std::string projectName);

        /** @brief Updates the project root draft value. */
        void SetProjectPath(std::string projectPath);

        /** @brief Updates the selected template identifier. */
        void SetTemplateId(std::string templateId);

        /** @brief Returns mutable access to the current typed draft. */
        [[nodiscard]] ProjectCreationDraft &MutableDraft() noexcept;

        /** @brief Updates the project version draft value. */
        void SetProjectVersion(std::string projectVersion);

        /** @brief Updates the default scene draft value. */
        void SetDefaultScene(std::string defaultScene);

        /** @brief Updates the render backend draft value. */
        void SetRenderBackend(std::string renderBackend);

        /** @brief Updates whether physics is enabled in the draft. */
        void SetPhysicsEnabled(bool physicsEnabled);

        /** @brief Updates the target frame rate in the draft. */
        void SetTargetFrameRate(int targetFrameRate);

        /** @brief Updates the build profile draft value. */
        void SetBuildProfile(std::string buildProfile);

        /** @brief Updates the asset compression draft value. */
        void SetAssetCompression(std::string assetCompression);

        /** @brief Updates the texture compression draft value. */
        void SetTextureCompression(std::string textureCompression);

        /** @brief Updates the target platform draft value. */
        void SetTargetPlatform(std::string targetPlatform);

        /** @brief Updates the compiler family draft value. */
        void SetCompilerFamily(std::string compilerFamily);

        /** @brief Updates the minimum C++ standard draft value. */
        void SetMinimumCxxStandard(int minimumCxxStandard);

        /** @brief Updates whether git initialization is enabled in the draft. */
        void SetInitializeGit(bool initializeGit);

        /** @brief Updates whether package restoration is enabled in the draft. */
        void SetRestorePackages(bool restorePackages);

        /** @brief Updates whether starter content inclusion is enabled in the draft. */
        void SetIncludeStarterContent(bool includeStarterContent);

        /** @brief Updates whether CMake project generation is enabled in the draft. */
        void SetGenerateCMakeProject(bool generateCMakeProject);

        /** @brief Validates name and filesystem destination state without creating project files. */
        [[nodiscard]] ProjectCreationValidation Validate() const;

        /** @brief Builds a request only when the current draft validates; does not create a project. */
        [[nodiscard]] std::optional<ProjectCreationRequest> BuildCreationRequest() const;

        /** @brief Reports whether the draft differs from the state that entered this screen instance. */
        [[nodiscard]] bool IsDirty() const noexcept;

        /** @brief Returns the explicit leave decision required for the current draft. */
        [[nodiscard]] ProjectCreationLeaveIntent LeaveIntent() const noexcept;

        /** @brief Restores the entry draft after a confirmed discard decision. */
        void DiscardDraft();

    private:
        ProjectCreationDraft initialDraft_;
        ProjectCreationDraft draft_;
        const RendererAvailabilitySnapshot *rendererAvailability_{nullptr};
    };
}  // namespace Horo::Editor
