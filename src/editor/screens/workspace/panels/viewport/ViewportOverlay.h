#pragma once

#include "editor/project_model/EditorTransformTool.h"
#include "editor/project_model/EditorViewportCamera.h"

#include <cstddef>
#include <imgui.h>
#include <optional>

namespace Horo::Editor {
    class ILocalizationService;

    namespace Theme {
        struct Fonts;
    }

    /** @brief Backend-independent state presented by the viewport overlay. */
    struct ViewportOverlayState {
        EditorViewportCamera camera;
        EditorTransformTool tool{EditorTransformTool::Select};
        bool gridVisible{true};
        bool canFocusSelection{false};
        std::size_t objectCount{};
        std::optional<std::size_t> vertexCount;
        std::optional<std::size_t> triangleCount;
    };

    /** @brief One user intent from the viewport controls. */
    struct ViewportOverlayAction {
        std::optional<Runtime::CameraProjection> projection;
        std::optional<EditorTransformTool> tool;
        std::optional<EditorViewportAxisView> axisView;
        bool toggleGrid{};
        bool focusSelection{};
    };

    /** @brief Toolbar rows shared by transform controls, navigation options and pointer admission. */
    struct ViewportToolbarLayout {
        ImVec2 toolsPosition;
        ImVec2 optionsPosition;
        float bottom{};
    };

    /** @brief Places options directly below the tools, including their wrapped row in narrow viewports. */
    [[nodiscard]] ViewportToolbarLayout ResolveViewportToolbar(ImVec2 origin, ImVec2 size);

    /** @brief Measured footer slots shared by viewport statistics and actions. */
    struct ViewportStatusRowLayout {
        ImVec2 objectsPosition;
        float objectsWidth{};
        ImVec2 actionsPosition;
        float actionsWidth{};
        float height{};
    };

    /** @brief Reserves the object badge first, then gives remaining row space to viewport actions. */
    [[nodiscard]] ViewportStatusRowLayout ResolveViewportStatusRow(ImVec2 origin, ImVec2 size, std::size_t objectCount,
                                                                   const ILocalizationService &localization);

    /** @brief Draws the same responsive viewport chrome in the editor and UI preview. */
    [[nodiscard]] ViewportOverlayAction DrawViewportOverlay(ImVec2 origin, ImVec2 size, const ViewportOverlayState &state,
                                                            const Theme::Fonts &fonts, const ILocalizationService &localization);
}  // namespace Horo::Editor
