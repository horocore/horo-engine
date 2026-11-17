#pragma once
/** @file EditorClusteredLightingStartup.h @brief Explicit Metal host cold activation of an existing packaged lighting family. */
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "editor/renderer/EditorViewportLighting.h"

#include <filesystem>

namespace Horo::Editor {
    class EditorViewportRendererMetal;
    /** @brief Prepares actual packaged native clustered consumers before frontend recipe selection.
     * @details Host owner only, after viewport Initialize and before BeginFrame/RequestLightingProfile.
     * Absent assets return unready facts for the frontend's authored selection policy. Broken or failed native assets return errors.
     * No runtime compilation, tool discovery or fallback selection occurs. */
    [[nodiscard]] Result<Render::RenderLightPreparationReadiness> PreparePackagedMetalViewportLighting(
        EditorViewportRendererMetal &viewport, Render::RenderFrontend &frontend, const EditorViewportLightingProfile &profile,
        const std::filesystem::path &assetRoot);
}  // namespace Horo::Editor
