#include "EditorVulkanViewportShader.h"

#include "EditorViewportShaderRecipe.h"
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
                .sourceRevision = 4,
                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                .bindings = {{{1},
                              ShaderResourceKind::UniformBuffer,
                              ShaderResourceAccess::ReadOnly,
                              1,
                              ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment},
                             {{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment}},
                .parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 1},
                               {{2}, {1}, ShaderValueType::Float32, 4, 4, 1},
                               {{3}, {1}, ShaderValueType::Float32, 4, 4, 1},
                               {{4}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{5}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{6}, {1}, ShaderValueType::Float32, 1, 4, MaximumForwardLights},
                               {{7}, {1}, ShaderValueType::Float32, 1, 4, MaximumForwardLights},
                               {{8}, {1}, ShaderValueType::Float32, 1, 4, MaximumForwardLights},
                               {{9}, {1}, ShaderValueType::Float32, 1, 4, MaximumForwardLights},
                               {{10}, {1}, ShaderValueType::Float32, 1, 4, 3},
                               {{11}, {1}, ShaderValueType::Float32, 4, 4, 4},
                               {{12}, {1}, ShaderValueType::Float32, 1, 4, 4},
                               {{13}, {1}, ShaderValueType::Float32, 1, 4, 4},
                               {{14}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{15}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{16}, {1}, ShaderValueType::Float32, 1, 4, 1},
                               {{20}, {1}, ShaderValueType::Float32, 1, 4, 1}},
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
                  {{10}, "OverlayColors"},
                  {{11}, "ShadowViewProjection"},
                  {{12}, "ShadowSplits"},
                  {{13}, "ShadowAtlasScaleBias"},
                  {{14}, "ShadowSettings"},
                  {{15}, "ShadowBiasAndTexel"},
                  {{16}, "ShadowCameraForward"},
                  {{20}, "BaseColor"}}},
                {{2}, "DirectionalShadowAtlas", {}},
                {{3}, "DirectionalShadowSampler", {}}};
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

    Render::ShaderManifest EditorVulkanShadowShaderManifest() {
        return MakeEditorViewportShaderManifest(EditorViewportShaderProgram::Shadow, EditorVulkanViewportShaderTarget()).Value();
    }

    std::vector<Render::ShaderReflectionBindingName> EditorVulkanShadowReflectionNames() {
        return EditorViewportShaderReflectionNames(EditorViewportShaderProgram::Shadow);
    }

    Result<EditorVulkanShadowCook> LoadEditorVulkanShadowCook(std::span<const std::uint8_t> payload,
                                                              std::span<const std::uint8_t> metadata) {
        auto reflection = Render::DecodeReflectedShaderArtifact(EditorVulkanShadowShaderManifest(), EditorVulkanViewportShaderTarget(),
                                                                payload, metadata);
        if (reflection.HasError())
            return Result<EditorVulkanShadowCook>::Failure(reflection.ErrorValue());
        return Result<EditorVulkanShadowCook>::Success({{payload.begin(), payload.end()}, std::move(reflection).Value()});
    }

    /** @copydoc EditorVulkanMultiviewShaderManifest */
    Render::ShaderManifest EditorVulkanMultiviewShaderManifest() {
        using enum Horo::Render::ShaderValueType;
        using namespace Render;
        return {.schemaVersion = 1,
                .sourceIdentity = "assets/shaders/runtime/multiview_mesh.hlsl",
                .sourceRevision = 1,
                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                .bindings = {{{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex}},
                .parameters = {{{1}, {1}, Float32, 4, 4, 32}, {{2}, {1}, Float32, 4, 4, 1}, {{3}, {1}, Uint32, 1, 1, 32}},
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

    /** @copydoc EditorVulkanMultiviewMotionShaderManifest */
    Render::ShaderManifest EditorVulkanMultiviewMotionShaderManifest() {
        using namespace Render;
        using namespace Render::MultiviewShaderParameterIds;
        return {.schemaVersion = 1,
                .sourceIdentity = "assets/shaders/runtime/multiview_motion.hlsl",
                .sourceRevision = 1,
                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                .bindings = {{Buffer, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex}},
                .parameters = {{CurrentLocalToClip, Buffer, ShaderValueType::Float32, 4, 4, MultiviewExtractionLimits::HardMaximumViews},
                               {PreviousLocalToClip, Buffer, ShaderValueType::Float32, 4, 4, MultiviewExtractionLimits::HardMaximumViews}},
                .targets = {EditorVulkanViewportShaderTarget()}};
    }

    /** @copydoc EditorVulkanMultiviewMotionReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorVulkanMultiviewMotionReflectionNames() {
        using namespace Render::MultiviewShaderParameterIds;
        return {{Buffer,
                 "type.MultiviewMotionParameters",
                 {{CurrentLocalToClip, "CurrentLocalToClip"}, {PreviousLocalToClip, "PreviousLocalToClip"}}}};
    }

    /** @copydoc LoadEditorVulkanMultiviewMotionCook */
    Result<EditorVulkanMultiviewCook> LoadEditorVulkanMultiviewMotionCook(const std::span<const std::uint8_t> payload,
                                                                          const std::span<const std::uint8_t> metadata) {
        using namespace Render;
        using Outcome = Result<EditorVulkanMultiviewCook>;
        auto reflected = DecodeReflectedShaderArtifact(EditorVulkanMultiviewMotionShaderManifest(), EditorVulkanViewportShaderTarget(),
                                                       payload, metadata);
        if (reflected.HasError())
            return Outcome::Failure(reflected.ErrorValue());
        const auto &reflection = reflected.Value().reflection;
        const auto matches = [&reflection](const ShaderStage stage, const ShaderInterfaceDirection direction, const std::uint32_t location,
                                           const std::uint32_t columns) {
            const auto found = std::ranges::find_if(reflection.stageInterface, [=](const auto &attribute) {
                return attribute.stage == stage && attribute.direction == direction && attribute.location == location;
            });
            return found != reflection.stageInterface.end() && found->type == ShaderValueType::Float32 && found->rows == 1 &&
                   found->columns == columns && found->arrayCount == 1;
        };
        using enum ShaderInterfaceDirection;
        if (reflection.stageInterface.size() != 6 || !matches(ShaderStage::Vertex, Input, 0, 3) ||
            !matches(ShaderStage::Vertex, Output, 0, 4) || !matches(ShaderStage::Vertex, Output, 1, 4) ||
            !matches(ShaderStage::Fragment, Input, 0, 4) || !matches(ShaderStage::Fragment, Input, 1, 4) ||
            !matches(ShaderStage::Fragment, Output, 0, 4))
            return Outcome::Failure(MakeError(ShaderReflectionErrors::StageInterfaceMismatch));
        const auto layout = PrepareMaterialBindingLayout(reflection);
        if (layout.HasError())
            return Outcome::Failure(layout.ErrorValue());
        const auto bytes = MultiviewMotionParameterBytes(layout.Value(), MultiviewExtractionLimits::HardMaximumViews);
        if (bytes.HasError())
            return Outcome::Failure(bytes.ErrorValue());
        constexpr auto blockBytes = MultiviewExtractionLimits::HardMaximumViews * 2 * sizeof(Math::Mat4);
        if (bytes.Value() != blockBytes)
            return Outcome::Failure(MakeError(ShaderReflectionErrors::LayoutMismatch));
        return Outcome::Success({{payload.begin(), payload.end()}, std::move(reflected).Value()});
    }
}  // namespace Horo::Editor
