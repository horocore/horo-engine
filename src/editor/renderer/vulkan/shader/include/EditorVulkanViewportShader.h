#pragma once
/** @file EditorVulkanViewportShader.h
 * @brief Portable authored viewport interface and final-artifact loading shared by offline cook and runtime consumer. */
#include "ShaderReflectionCook.h"

namespace Horo::Editor {
    [[nodiscard]] Render::ShaderTargetRequirement EditorVulkanViewportShaderTarget();
    [[nodiscard]] Render::ShaderManifest EditorVulkanViewportShaderManifest();
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorVulkanViewportReflectionNames();

    /** @brief Separate actual cooked depth artifact from the canonical shared shadow source. */
    struct EditorVulkanShadowCook {
        std::vector<std::uint8_t> payload;
        Render::ReflectedShaderArtifact metadata;
    };

    [[nodiscard]] Render::ShaderManifest EditorVulkanShadowShaderManifest();
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorVulkanShadowReflectionNames();
    [[nodiscard]] Result<EditorVulkanShadowCook> LoadEditorVulkanShadowCook(std::span<const std::uint8_t> payload,
                                                                            std::span<const std::uint8_t> metadata);

    /** @brief Immutable ready-to-load Vulkan package; native shader/pipeline realization belongs to the frontend. */
    struct EditorVulkanViewportCook {
        std::vector<std::uint8_t> payload;
        Render::ReflectedShaderArtifact metadata;
        std::optional<EditorVulkanShadowCook> shadow;
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
    /** @brief Distinct offline-compiled shader interface for the optional one-color motion plus depth pass. */
    [[nodiscard]] Render::ShaderManifest EditorVulkanMultiviewMotionShaderManifest();
    /** @brief Exact compiler-reflected block/member names for motion input. */
    [[nodiscard]] std::vector<Render::ShaderReflectionBindingName> EditorVulkanMultiviewMotionReflectionNames();
    /** @brief Admits authenticated float4 motion output and both historic/current clip-transform arrays.
     * @param payload Same-operation SPIR-V package. @param metadata Exact reflected sidecar bytes.
     * @return Owned cook or a typed target/interface/packing failure. Performs no device work.
     */
    [[nodiscard]] Result<EditorVulkanMultiviewCook> LoadEditorVulkanMultiviewMotionCook(std::span<const std::uint8_t> payload,
                                                                                        std::span<const std::uint8_t> metadata);
}  // namespace Horo::Editor
