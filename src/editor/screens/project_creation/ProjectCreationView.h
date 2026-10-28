#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizedGui.h"
#include "Horo/Editor/ProjectCreationController.h"
#include "Horo/Runtime/Input.h"
#include "editor/project_model/RendererAvailability.h"

#include <array>
#include <string>

namespace Horo::Editor {
    /** @brief Shared route-screen and view localization scope. */
    inline constexpr GuiLocalizationScope ProjectCreationTextScope{"project_creation"};
    struct GuiContentRegion;

    /** @brief Transient ImGui state retained by the ProjectCreation route view presentation. */
    struct ProjectCreationViewState {  // NOSONAR(cpp:S1820) Transient form state container
        bool initialized = false;
        int step = 1;
        int highestUnlockedStep = 1;
        int lastRenderedStep = 0;
        std::string projectName;
        std::string projectPath;
        std::string projectVersion;
        std::string defaultScene;
        std::string targetFps;
        int renderBackendIndex = 0;
        int physicsIndex = 0;
        int buildProfileIndex = 0;
        int assetCompressionIndex = 0;
        int textureCompressionIndex = 0;
        int targetPlatformIndex = 0;
        int compilerFamilyIndex = 0;
        int cppStandardIndex = 0;
        std::string packageRegistryUrl = "registry://horo/starter-package";
        std::string packageVersion = "1.0.0";
        int firstPersonInputMapIndex = 0;
        bool demoObservabilityOverlays = true;
        bool demoBenchmarkScene = true;
        std::array<bool, 5> customSubsystems{true, true, true, false, false};
    };

    /** @brief Command emitted by the ProjectCreation route view presentation. */
    enum class ProjectCreationViewCommand {
        None,
        ReturnToWelcome,
        CreateProject,
    };

    /**
     * @brief Draws the ProjectCreation route as full application content.
     * @param controller Headless project-creation workflow state.
     * @param state Route-local presentation state.
     * @param ctx Editor GUI context and fonts.
     * @param rendererAvailability Machine-local renderer choices and disabled-state diagnostics.
     * @param logo Optional logo texture shown in the header.
     * @return Typed navigation command requested during this frame.
     */
    [[nodiscard]] ProjectCreationViewCommand DrawProjectCreationView(ProjectCreationController &controller, ProjectCreationViewState &state,
                                                                     const EditorGuiContext &ctx, Input::InputRouter &inputRouter,
                                                                     const RendererAvailabilitySnapshot &rendererAvailability,
                                                                     const GuiContentRegion &contentRegion, ImTextureID logo = 0);
}  // namespace Horo::Editor
