#include "EditorVulkanViewportShader.h"

namespace Horo::Editor {
    /** @copydoc EditorVulkanViewportShaderTarget */
    Render::ShaderTargetRequirement EditorVulkanViewportShaderTarget() {
        return {.backend = Render::ShaderTargetBackend::Vulkan,
                .payloadFormat = Render::ShaderPayloadFormat::SpirV16,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 16,
                .maximumInlineConstantBytes = 128};
    }

    /** @copydoc EditorVulkanViewportShaderManifest */
    Render::ShaderManifest EditorVulkanViewportShaderManifest() {
        using namespace Render;
        return {.schemaVersion = 1,
                .sourceIdentity = "engine/editor/viewport.hlsl",
                .sourceRevision = 1,
                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                .bindings = {{{1},
                              ShaderResourceKind::UniformBuffer,
                              ShaderResourceAccess::ReadOnly,
                              1,
                              ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment}},
                .parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 1},
                               {{2}, {1}, ShaderValueType::Float32, 4, 4, 1},
                               {{3}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{4}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{5}, {1}, ShaderValueType::Float32, 1, 4, 1}},
                .targets = {EditorVulkanViewportShaderTarget()}};
    }

    /** @copydoc EditorVulkanViewportReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorVulkanViewportReflectionNames() {
        return {{{1},
                 "ViewportParameters",
                 {{{1}, "Mvp"}, {{2}, "NormalToWorld"}, {{3}, "Tint"}, {{4}, "LightDirection"}, {{5}, "LightColor"}}}};
    }

    /** @copydoc LoadEditorVulkanViewportCook */
    Result<EditorVulkanViewportCook> LoadEditorVulkanViewportCook(const std::span<const std::uint8_t> payload,
                                                                  const std::span<const std::uint8_t> metadata) {
        const auto reflection = Render::DecodeReflectedShaderArtifact(EditorVulkanViewportShaderManifest(),
                                                                      EditorVulkanViewportShaderTarget(), payload, metadata);
        if (reflection.HasError())
            return Result<EditorVulkanViewportCook>::Failure(reflection.ErrorValue());
        return Result<EditorVulkanViewportCook>::Success({{payload.begin(), payload.end()}, reflection.Value()});
    }
}  // namespace Horo::Editor
