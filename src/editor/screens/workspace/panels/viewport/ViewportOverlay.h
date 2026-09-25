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

    /** @brief Draws the same responsive viewport chrome in the editor and UI preview. */
    [[nodiscard]] ViewportOverlayAction DrawViewportOverlay(ImVec2 origin, ImVec2 size, const ViewportOverlayState &state,
                                                            const Theme::Fonts &fonts, const ILocalizationService &localization);
}  // namespace Horo::Editor
