#pragma once
/** @file EditorVulkanViewportShader.h
 * @brief Portable authored viewport interface and final-artifact loading shared by offline cook and runtime consumer. */
#include "ShaderReflectionCook.h"

namespace Horo::Editor {
    [[nodiscard]] Render::ShaderTargetRequirement EditorVulkanViewportShaderTarget();
    [[nodiscard]] Render::ShaderManifest EditorVulkanViewportShaderManifest();
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorVulkanViewportReflectionNames();

    /** @brief Immutable ready-to-load Vulkan package; native shader/pipeline realization belongs to the frontend. */
    struct EditorVulkanViewportCook {
        std::vector<std::uint8_t> payload;
        Render::ReflectedShaderArtifact metadata;
    };

    /** @brief Rejects altered/mismatched cooked payload/layout before any native shader object is created. */
    [[nodiscard]] Result<EditorVulkanViewportCook> LoadEditorVulkanViewportCook(std::span<const std::uint8_t> payload,
                                                                                std::span<const std::uint8_t> metadata);
}  // namespace Horo::Editor
