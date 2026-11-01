#include "EditorViewportShaderParameters.h"

#include "../EditorRendererErrors.h"
#include "EditorViewportMatrixComponents.h"
#include "EditorViewportShaderRecipe.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        Result<void> ValidateViewportReloadInterface(const Render::ShaderHotReloadProgramView &view,
                                                     const Render::ReflectedShaderArtifact &reflection, const std::size_t role) {
            const auto canonical =
                MakeEditorViewportShaderManifest(static_cast<EditorViewportShaderProgram>(role + 1), view.manifest->targets.front());
            if (canonical.HasError())
                return Result<void>::Failure(canonical.ErrorValue());
            const auto expected = Render::ComputeShaderInterfaceCompatibilityId(canonical.Value());
            if (expected.HasError())
                return Result<void>::Failure(expected.ErrorValue());
            if (expected.Value() != reflection.reflection.interfaceCompatibility)
                return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
            return Result<void>::Success();
        }
    }  // namespace

    /** @brief Requires exact canonical logical ABI and its same-boundary cooked native bytes. */
    Result<const Render::CompiledShaderArtifact *> ViewportReloadArtifact(const Render::ShaderHotReloadProgramView &view,
                                                                          const Render::ShaderTargetBackend backend,
                                                                          const std::size_t role) {
        using Outcome = Result<const Render::CompiledShaderArtifact *>;
        if (!view.manifest || !view.compiled || view.id.value != role + 1)
            return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::InvalidRequest));
        if (view.compiled->artifacts.size() != 1)
            return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::InvalidAdapterOutput));
        const auto &artifact = view.compiled->artifacts.front();
        if (artifact.backend != backend || !artifact.reflection || view.manifest->targets.size() != 1)
            return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::InvalidAdapterOutput));
        const auto interface = ValidateViewportReloadInterface(view, *artifact.reflection, role);
        if (interface.HasError())
            return Outcome::Failure(interface.ErrorValue());
        const auto valid = Render::ValidateReflectedShaderArtifact(artifact, *artifact.reflection);
        if (valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        return Outcome::Success(&artifact);
    }

    void EditorViewportShaderParameters::SetScene(const Render::RenderSceneView &scene,
                                                  const std::optional<Render::DirectionalShadowFrame> &shadowFrame,
                                                  const Render::ShaderTargetBackend backend) noexcept {
        const auto count = std::min(scene.lights.size(), std::size_t{16});
        camera = {scene.camera.position.x, scene.camera.position.y, scene.camera.position.z, static_cast<float>(count)};
        const auto forward = Math::Normalize(scene.camera.target - scene.camera.position);
        cameraForward = {forward.x, forward.y, forward.z, 1.0F};
        shadowSettings = {};
        shadowCoordinates = backend == Render::ShaderTargetBackend::Metal ? std::array<float, 4>{0.5F, -0.5F, 1.0F, 0.0F}
                                                                          : std::array<float, 4>{0.5F, 0.5F, 0.5F, 0.5F};
        if (shadowFrame) {
            const auto &frame = *shadowFrame;
            shadowSettings = {1, static_cast<float>(frame.lightIndex), static_cast<float>(frame.cascadeCount), 1.0F / frame.atlasWidth};
            cameraForward[3] = 1.0F / frame.atlasHeight;
            shadowBiasFilter = {frame.profile.depthBias, frame.profile.slopeBias, frame.profile.normalBias,
                                static_cast<float>(frame.profile.filterRadius)};
            for (std::size_t index = 0; index < frame.cascadeCount; ++index) {
                const auto &cascade = frame.cascades[index];
                std::copy(cascade.cameraRelativeViewProjection.values.begin(), cascade.cameraRelativeViewProjection.values.end(),
                          shadow.begin() + index * 16);
                const std::array<float, 4> split{cascade.splitFar, cascade.blendStart, cascade.splitNear, cascade.worldUnitsPerTexel};
                const auto &rect = cascade.atlasScaleBias;
                const float y = backend == Render::ShaderTargetBackend::OpenGL ? 1.0F - rect.w - rect.y : rect.w;
                const std::array<float, 4> atlas{rect.x, rect.y, rect.z, y};
                std::copy(split.begin(), split.end(), cascadeSplits.begin() + index * 4);
                std::copy(atlas.begin(), atlas.end(), atlasRects.begin() + index * 4);
            }
        }
        for (std::size_t index = 0; index < count; ++index) {
            const auto &light = scene.lights[index];
            const std::array<std::array<float, 4>, 4> values{
                {{light.position.x, light.position.y, light.position.z, static_cast<float>(light.kind)},
                 {light.direction.x, light.direction.y, light.direction.z, light.range},
                 {light.color.x, light.color.y, light.color.z, light.intensity},
                 {light.innerConeCosine, light.outerConeCosine, 0, 0}}};
            for (std::size_t kind = 0; kind < values.size(); ++kind)
                std::copy(values[kind].begin(), values[kind].end(), lights[kind].begin() + index * 4);
        }
    }

    Result<std::size_t> EditorViewportShaderParameters::PackLit(const Render::MaterialBindingLayout &layout,
                                                                const std::span<std::byte> destination) const {
        const auto mvpComponents = Detail::MatrixRowMajorComponents(mvp.values);
        const auto modelComponents = Detail::MatrixRowMajorComponents(model.values);
        const auto shadowComponents = Detail::MatrixRowMajorComponents(shadow);
        const std::array<Render::MaterialParameterValue, 15> values{{{{1}, std::span<const float>{mvpComponents}},
                                                                     {{2}, std::span<const float>{modelComponents}},
                                                                     {{3}, std::span<const float>{shadowComponents}},
                                                                     {{4}, std::span<const float>{camera}},
                                                                     {{5}, std::span<const float>{lights[0]}},
                                                                     {{6}, std::span<const float>{lights[1]}},
                                                                     {{7}, std::span<const float>{lights[2]}},
                                                                     {{8}, std::span<const float>{lights[3]}},
                                                                     {{9}, std::span<const float>{selection}},
                                                                     {{10}, std::span<const float>{shadowSettings}},
                                                                     {{11}, std::span<const float>{shadowCoordinates}},
                                                                     {{12}, std::span<const float>{cascadeSplits}},
                                                                     {{13}, std::span<const float>{atlasRects}},
                                                                     {{14}, std::span<const float>{cameraForward}},
                                                                     {{15}, std::span<const float>{shadowBiasFilter}}}};
        return Render::PackMaterialParameterBinding(layout, {1}, values, destination);
    }

    Math::Mat4 EditorViewportShadowMvp(const Render::DirectionalShadowFrame &frame, const std::size_t cascade,
                                       const Math::Mat4 &model) noexcept {
        auto localized = model;
        localized.values[12] -= frame.cameraOrigin.x;
        localized.values[13] -= frame.cameraOrigin.y;
        localized.values[14] -= frame.cameraOrigin.z;
        return Math::Multiply(frame.cascades[cascade].cameraRelativeViewProjection, localized);
    }

    Result<std::size_t> PackEditorViewportGrid(const Render::MaterialBindingLayout &layout, const Math::Mat4 &mvp, const Math::Vec3 color,
                                               const std::span<std::byte> destination) {
        const auto mvpComponents = Detail::MatrixRowMajorComponents(mvp.values);
        const std::array<float, 4> rgba{color.x, color.y, color.z, 1};
        const std::array<Render::MaterialParameterValue, 2> values{
            {{{1}, std::span<const float>{mvpComponents}}, {{2}, std::span<const float>{rgba}}}};
        return Render::PackMaterialParameterBinding(layout, {1}, values, destination);
    }

    Result<std::size_t> PackEditorViewportShadow(const Render::MaterialBindingLayout &layout, const Math::Mat4 &mvp,
                                                 const std::span<std::byte> destination) {
        const auto mvpComponents = Detail::MatrixRowMajorComponents(mvp.values);
        const std::array<Render::MaterialParameterValue, 1> values{{{{1}, std::span<const float>{mvpComponents}}}};
        return Render::PackMaterialParameterBinding(layout, {1}, values, destination);
    }
}  // namespace Horo::Editor
