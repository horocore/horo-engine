#include "EditorViewportShaderRecipe.h"

#include "Horo/Runtime/Render/ShaderManifestErrors.h"

namespace Horo::Editor {
    namespace {
        using namespace Render;

        /** @brief Logical scalar/vector/matrix shapes only; actual offsets and strides come from target reflection. */
        std::vector<ShaderParameter> LitParameters() {
            return {{{1}, {1}, ShaderValueType::Float32, 4, 4, 1},  {{2}, {1}, ShaderValueType::Float32, 4, 4, 1},
                    {{3}, {1}, ShaderValueType::Float32, 4, 4, 4},  {{4}, {1}, ShaderValueType::Float32, 1, 4, 1},
                    {{5}, {1}, ShaderValueType::Float32, 1, 4, 16}, {{6}, {1}, ShaderValueType::Float32, 1, 4, 16},
                    {{7}, {1}, ShaderValueType::Float32, 1, 4, 16}, {{8}, {1}, ShaderValueType::Float32, 1, 4, 16},
                    {{9}, {1}, ShaderValueType::Float32, 1, 4, 1},  {{10}, {1}, ShaderValueType::Float32, 1, 4, 1},
                    {{11}, {1}, ShaderValueType::Float32, 1, 4, 1}, {{12}, {1}, ShaderValueType::Float32, 1, 4, 4},
                    {{13}, {1}, ShaderValueType::Float32, 1, 4, 4}, {{14}, {1}, ShaderValueType::Float32, 1, 4, 1},
                    {{15}, {1}, ShaderValueType::Float32, 1, 4, 1}, {{20}, {1}, ShaderValueType::Float32, 1, 4, 1},
                    {{21}, {1}, ShaderValueType::Float32, 1, 4, 1}};
        }

        void DescribeLit(ShaderManifest &manifest) {
            manifest.sourceIdentity = "engine/editor/viewport_lit.hlsl";
            manifest.sourceRevision = 4;
            manifest
                .bindings = {{{1},
                              ShaderResourceKind::UniformBuffer,
                              ShaderResourceAccess::ReadOnly,
                              1,
                              ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment},
                             {{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment}};
            manifest.parameters = LitParameters();
        }

        void DescribeGrid(ShaderManifest &manifest) {
            manifest.sourceIdentity = "engine/editor/viewport_grid.hlsl";
            manifest.sourceRevision = 2;
            manifest.bindings = {{{1},
                                  ShaderResourceKind::UniformBuffer,
                                  ShaderResourceAccess::ReadOnly,
                                  1,
                                  ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment}};
            manifest.parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 1}, {{2}, {1}, ShaderValueType::Float32, 1, 4, 1}};
        }

        void DescribeShadow(ShaderManifest &manifest) {
            manifest.sourceIdentity = "engine/editor/viewport_shadow.hlsl";
            manifest.bindings = {
                {{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex}};
            manifest.parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 1}};
        }
    }  // namespace

    /** @copydoc EditorViewportShaderSource */
    std::string_view EditorViewportShaderSource(const EditorViewportShaderProgram program) noexcept {
        switch (program) {
            case EditorViewportShaderProgram::Lit:
                return "shaders/editor/viewport_lit.hlsl";
            case EditorViewportShaderProgram::Grid:
                return "shaders/editor/viewport_grid.hlsl";
            case EditorViewportShaderProgram::Shadow:
                return "shaders/editor/viewport_shadow.hlsl";
        }
        return {};
    }

    /** @copydoc MakeEditorViewportShaderManifest */
    Result<Render::ShaderManifest> MakeEditorViewportShaderManifest(const EditorViewportShaderProgram program,
                                                                    const Render::ShaderTargetRequirement &target) {
        if (target.backend != ShaderTargetBackend::OpenGL && target.backend != ShaderTargetBackend::Metal &&
            !(target.backend == ShaderTargetBackend::Vulkan && program == EditorViewportShaderProgram::Shadow))
            return Result<ShaderManifest>::Failure(MakeError(ShaderManifestErrors::UnsupportedTarget));
        ShaderManifest manifest{.schemaVersion = 1,
                                .sourceRevision = 1,
                                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                                .targets = {target}};
        switch (program) {
            case EditorViewportShaderProgram::Lit:
                DescribeLit(manifest);
                break;
            case EditorViewportShaderProgram::Grid:
                DescribeGrid(manifest);
                break;
            case EditorViewportShaderProgram::Shadow:
                DescribeShadow(manifest);
                break;
            default:
                return Result<ShaderManifest>::Failure(MakeError(ShaderManifestErrors::UnsupportedTarget));
        }
        if (const auto validated = ValidateShaderManifest(manifest); validated.HasError())
            return Result<ShaderManifest>::Failure(validated.ErrorValue());
        return Result<ShaderManifest>::Success(std::move(manifest));
    }

    /** @copydoc EditorViewportShaderReflectionNames */
    std::vector<Render::ShaderReflectionBindingName> EditorViewportShaderReflectionNames(const EditorViewportShaderProgram program) {
        switch (program) {
            case EditorViewportShaderProgram::Lit:
                return {{{1},
                         "type.ViewportParameters",
                         {{{1}, "Mvp"},
                          {{2}, "ModelToWorld"},
                          {{3}, "ShadowViewProjection"},
                          {{4}, "CameraAndLightCount"},
                          {{5}, "LightPositionKind"},
                          {{6}, "LightDirectionRange"},
                          {{7}, "LightColorIntensity"},
                          {{8}, "LightCone"},
                          {{9}, "SelectionColorAndStrength"},
                          {{10}, "ShadowSettings"},
                          {{11}, "ShadowCoordinates"},
                          {{12}, "CascadeSplits"},
                          {{13}, "AtlasRects"},
                          {{14}, "CameraForward"},
                          {{15}, "ShadowBiasFilter"},
                          {{20}, "BaseColor"},
                          {{21}, "SceneColorPolicy"}}},
                        {{2}, "ShadowMap", {}},
                        {{3}, "ShadowSampler", {}}};
            case EditorViewportShaderProgram::Grid:
                return {{{1}, "type.GridParameters", {{{1}, "Mvp"}, {{2}, "Color"}}}};
            case EditorViewportShaderProgram::Shadow:
                return {{{1}, "type.ShadowParameters", {{{1}, "Mvp"}}}};
        }
        return {};
    }
}  // namespace Horo::Editor
