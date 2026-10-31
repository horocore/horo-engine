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

    /** @brief Exact canonical runtime multiview interface, consumed through the same compiled/reflected producer. */
    [[nodiscard]] Render::ShaderManifest EditorVulkanMultiviewShaderManifest();
    /** @brief Exact authored member names; offsets and strides always come from actual compiler reflection. */
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorVulkanMultiviewReflectionNames();

    /** @brief Ready selected-Vulkan multipass program; no native realization or availability is implied. */
    struct EditorVulkanMultiviewCook final {
        std::vector<std::uint8_t> payload;
        Render::ReflectedShaderArtifact metadata;
    };

    /** @brief Validates actual immutable payload, canonical reflection and bounded multiview uniform ABI before realization.
     * @param payload Same compile-boundary SPIR-V package. @param metadata Its canonical immutable reflected record.
     * @return Exact accepted multipass program or original altered/target/interface/layout error; no reflected layout is invented.
     */
    [[nodiscard]] Result<EditorVulkanMultiviewCook> LoadEditorVulkanMultiviewCook(std::span<const std::uint8_t> payload,
                                                                                  std::span<const std::uint8_t> metadata);
}  // namespace Horo::Editor
