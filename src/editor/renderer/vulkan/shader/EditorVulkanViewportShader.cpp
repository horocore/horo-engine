#include "EditorVulkanViewportShader.h"

#include "Horo/Runtime/Render/MultiviewShaderParameters.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"

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
                .sourceRevision = 2,
                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                .bindings = {{{1},
                              ShaderResourceKind::UniformBuffer,
                              ShaderResourceAccess::ReadOnly,
                              1,
                              ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment}},
                .parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 1},
                               {{2}, {1}, ShaderValueType::Float32, 4, 4, 1},
                               {{3}, {1}, ShaderValueType::Float32, 4, 4, 1},
                               {{4}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{5}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{6}, {1}, ShaderValueType::Float32, 1, 4, MaximumForwardLights},
                               {{7}, {1}, ShaderValueType::Float32, 1, 4, MaximumForwardLights},
                               {{8}, {1}, ShaderValueType::Float32, 1, 4, MaximumForwardLights},
                               {{9}, {1}, ShaderValueType::Float32, 1, 4, MaximumForwardLights},
                               {{10}, {1}, ShaderValueType::Float32, 1, 4, 3}},
                .targets = {EditorVulkanViewportShaderTarget()}};
    }

    /** @copydoc EditorVulkanViewportReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorVulkanViewportReflectionNames() {
        return {{{1},
                 "type.ViewportParameters",
                 {{{1}, "Mvp"},
                  {{2}, "NormalToWorld"},
                  {{3}, "ModelToWorld"},
                  {{4}, "Tint"},
                  {{5}, "CameraAndLightCount"},
                  {{6}, "LightPositionKind"},
                  {{7}, "LightDirectionRange"},
                  {{8}, "LightColorIntensity"},
                  {{9}, "LightCone"},
                  {{10}, "OverlayColors"}}}};
    }

    /** @copydoc LoadEditorVulkanViewportCook */
    Result<EditorVulkanViewportCook> LoadEditorVulkanViewportCook(const std::span<const std::uint8_t> payload,
                                                                  const std::span<const std::uint8_t> metadata) {
        const auto reflection = Render::DecodeReflectedShaderArtifact(EditorVulkanViewportShaderManifest(),
                                                                      EditorVulkanViewportShaderTarget(), payload, metadata);
        if (reflection.HasError())
            return Result<EditorVulkanViewportCook>::Failure(reflection.ErrorValue());
        bool position{}, normal{};
        for (const auto &attribute : reflection.Value().reflection.stageInterface) {
            if (attribute.stage != Render::ShaderStage::Vertex || attribute.direction != Render::ShaderInterfaceDirection::Input)
                continue;
            const auto columns = attribute.location == 2 ? 2 : 3;
            if (attribute.location > 2 || attribute.type != Render::ShaderValueType::Float32 || attribute.rows != 1 ||
                attribute.columns != columns || attribute.arrayCount != 1)
                return Result<EditorVulkanViewportCook>::Failure(MakeError(Render::ShaderReflectionErrors::StageInterfaceMismatch));
            position = position || attribute.location == 0;
            normal = normal || attribute.location == 1;
        }
        // UV is allowed to disappear after optimization; position and normal drive the shipped mesh recipe.
        if (!position || !normal)
            return Result<EditorVulkanViewportCook>::Failure(MakeError(Render::ShaderReflectionErrors::StageInterfaceMismatch));
        return Result<EditorVulkanViewportCook>::Success({{payload.begin(), payload.end()}, reflection.Value()});
    }

    /** @copydoc EditorVulkanMultiviewShaderManifest */
    Render::ShaderManifest EditorVulkanMultiviewShaderManifest() {
        using namespace Render;
        return {.schemaVersion = 1,
                .sourceIdentity = "assets/shaders/runtime/multiview_mesh.hlsl",
                .sourceRevision = 1,
                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                .bindings = {{{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex}},
                .parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 32},
                               {{2}, {1}, ShaderValueType::Float32, 4, 4, 1},
                               {{3}, {1}, ShaderValueType::Uint32, 1, 1, 32}},
                .targets = {EditorVulkanViewportShaderTarget()}};
    }

    /** @copydoc EditorVulkanMultiviewReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorVulkanMultiviewReflectionNames() {
        return {{{1}, "type.MultiviewParameters", {{{1}, "WorldToClip"}, {{2}, "LocalToWorld"}, {{3}, "ViewLayers"}}}};
    }

    /** @copydoc LoadEditorVulkanMultiviewCook */
    Result<EditorVulkanMultiviewCook> LoadEditorVulkanMultiviewCook(const std::span<const std::uint8_t> payload,
                                                                    const std::span<const std::uint8_t> metadata) {
        using Outcome = Result<EditorVulkanMultiviewCook>;
        auto reflection = Render::DecodeReflectedShaderArtifact(EditorVulkanMultiviewShaderManifest(), EditorVulkanViewportShaderTarget(),
                                                                payload, metadata);
        if (reflection.HasError())
            return Outcome::Failure(reflection.ErrorValue());
        bool position{};
        for (const auto &attribute : reflection.Value().reflection.stageInterface) {
            if (attribute.stage != Render::ShaderStage::Vertex || attribute.direction != Render::ShaderInterfaceDirection::Input)
                continue;
            // The canonical shader consumes position; unused MeshVertex normal/UV inputs may be optimized out.
            if (attribute.location != 0 || attribute.type != Render::ShaderValueType::Float32 || attribute.rows != 1 ||
                attribute.columns != 3 || attribute.arrayCount != 1)
                return Outcome::Failure(MakeError(Render::ShaderReflectionErrors::StageInterfaceMismatch));
            position = true;
        }
        if (!position)
            return Outcome::Failure(MakeError(Render::ShaderReflectionErrors::StageInterfaceMismatch));
        const auto layout = Render::PrepareMaterialBindingLayout(reflection.Value().reflection);
        if (layout.HasError())
            return Outcome::Failure(layout.ErrorValue());
        const auto bytes = Render::MultiviewShaderParameterBytes(layout.Value(), 2);
        if (bytes.HasError())
            return Outcome::Failure(bytes.ErrorValue());
        if (bytes.Value() == 0 || bytes.Value() > 4096)
            return Outcome::Failure(MakeError(Render::ShaderReflectionErrors::LayoutMismatch));
        return Outcome::Success({{payload.begin(), payload.end()}, std::move(reflection).Value()});
    }
}  // namespace Horo::Editor
