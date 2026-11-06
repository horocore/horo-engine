#include "EditorViewportMatrixComponents.h"
#include "EditorViewportShaderParameters.h"

namespace Horo::Editor {
    namespace {
        bool SameLightTable(const Render::RenderSceneView &scene, const Render::ClusteredLightGrid &grid) noexcept {
            return scene.lightSceneDomain != 0 && scene.lightSnapshotRevision != 0 &&
                   scene.lightSceneDomain == grid.descriptor.identity.sceneDomain &&
                   scene.lightSnapshotRevision == grid.descriptor.identity.lightTableGeneration &&
                   scene.lights.size() <= Render::MaximumSceneLights;
        }

        bool SameVector(const Math::Vec3 a, const Math::Vec3 b) noexcept {
            return a.x == b.x && a.y == b.y && a.z == b.z;
        }

        bool SameProjection(const Render::RenderProjectionDescriptor &a, const Render::RenderProjectionDescriptor &b) noexcept {
            return a.kind == b.kind && a.verticalFovRadians == b.verticalFovRadians && a.orthographicHeight == b.orthographicHeight &&
                   a.nearPlane == b.nearPlane && a.farPlane == b.farPlane;
        }

        bool SameCamera(const Render::RenderCameraView &a, const Render::RenderCameraView &b) noexcept {
            return a.IsValid() && SameVector(a.position, b.position) && SameVector(a.target, b.target) && SameVector(a.up, b.up) &&
                   SameProjection(a.projection, b.projection);
        }

        bool ValidClusterUniformInput(const Render::RenderSceneView &scene, const Render::ClusteredLightGrid &grid,
                                      const std::uint32_t referencesPerCluster) noexcept {
            return grid.IsValid() && SameLightTable(scene, grid) && SameCamera(scene.camera, grid.descriptor.camera) &&
                   referencesPerCluster > 0 && referencesPerCluster <= Render::MaximumSceneLights &&
                   std::uint64_t{grid.clusterCount} * referencesPerCluster <= Render::LightCullingBudget::HardMaximumReferences &&
                   grid.descriptor.depthRange == Math::ClipDepthRange::ZeroToOne;
        }

        std::array<float, 4> IdentityLanes(const std::uint64_t identity) noexcept {
            return {static_cast<float>(identity & 0xffffU), static_cast<float>((identity >> 16U) & 0xffffU),
                    static_cast<float>((identity >> 32U) & 0xffffU), static_cast<float>(identity >> 48U)};
        }

        Result<std::array<float, 4>> ShadowIdentityLanes(const Render::RenderSceneView &scene,
                                                         const std::optional<Render::DirectionalShadowFrame> &shadowFrame) {
            using Outcome = Result<std::array<float, 4>>;
            if (!shadowFrame)
                return Outcome::Success({});
            if (!shadowFrame->IsValid(scene) || !SameVector(shadowFrame->cameraOrigin, scene.camera.position))
                return Outcome::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
            if (shadowFrame->lightIndex >= scene.lights.size() || scene.lights[shadowFrame->lightIndex].stableIdentity == 0)
                return Outcome::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
            return Outcome::Success(IdentityLanes(scene.lights[shadowFrame->lightIndex].stableIdentity));
        }
    }  // namespace

    Result<void> EditorViewportClusteredShaderParameters::SetScene(const Render::RenderSceneView &scene,
                                                                   const Render::ClusteredLightGrid &grid,
                                                                   const std::optional<Render::DirectionalShadowFrame> &shadowFrame,
                                                                   const std::uint32_t referencesPerCluster) {
        if (!ValidClusterUniformInput(scene, grid, referencesPerCluster))
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        const auto identity = ShadowIdentityLanes(scene, shadowFrame);
        if (identity.HasError())
            return Result<void>::Failure(identity.ErrorValue());
        auto cameraOnly = scene;
        cameraOnly.lights = {};
        common.SetScene(cameraOnly, shadowFrame, Render::ShaderTargetBackend::Metal);
        common.camera[3] = static_cast<float>(scene.lights.size());
        const auto &descriptor = grid.descriptor;
        dimensions = {static_cast<float>(descriptor.x), static_cast<float>(descriptor.y), static_cast<float>(descriptor.z),
                      static_cast<float>(scene.lights.size())};
        depth = {descriptor.camera.projection.nearPlane, descriptor.camera.projection.farPlane,
                 descriptor.depthPartition == Render::LightClusterDepthPartition::Logarithmic ? 1.0F : 0.0F,
                 descriptor.camera.projection.kind == Render::RenderProjectionKind::Orthographic ? 1.0F : 0.0F};
        viewport = {static_cast<float>(descriptor.extent.width), static_cast<float>(descriptor.extent.height),
                    static_cast<float>(referencesPerCluster), static_cast<float>(grid.clusterCount)};
        shadowIdentity = identity.Value();
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportClusteredShaderParameters::PackLit */
    Result<std::size_t> EditorViewportClusteredShaderParameters::PackLit(const Render::MaterialBindingLayout &layout,
                                                                         const std::span<std::byte> destination) const {
        const auto mvpComponents = Detail::MatrixRowMajorComponents(common.mvp.values);
        const auto modelComponents = Detail::MatrixRowMajorComponents(common.model.values);
        const auto shadowComponents = Detail::MatrixRowMajorComponents(common.shadow);
        const std::array<float, 4> sceneColorPolicy{common.unexposedAcesCg ? 1.0F : 0.0F, 0, 0, 0};
        const std::array<Render::MaterialParameterValue, 17> values{{{{1}, std::span<const float>{mvpComponents}},
                                                                     {{2}, std::span<const float>{modelComponents}},
                                                                     {{3}, std::span<const float>{shadowComponents}},
                                                                     {{4}, std::span<const float>{common.camera}},
                                                                     {{9}, std::span<const float>{common.selection}},
                                                                     {{10}, std::span<const float>{common.shadowSettings}},
                                                                     {{11}, std::span<const float>{common.shadowCoordinates}},
                                                                     {{12}, std::span<const float>{common.cascadeSplits}},
                                                                     {{13}, std::span<const float>{common.atlasRects}},
                                                                     {{14}, std::span<const float>{common.cameraForward}},
                                                                     {{15}, std::span<const float>{common.shadowBiasFilter}},
                                                                     {{16}, std::span<const float>{dimensions}},
                                                                     {{17}, std::span<const float>{depth}},
                                                                     {{18}, std::span<const float>{viewport}},
                                                                     {{19}, std::span<const float>{shadowIdentity}},
                                                                     {{20}, std::span<const float>{common.baseColor}},
                                                                     {{21}, std::span<const float>{sceneColorPolicy}}}};
        return Render::PackMaterialParameterBinding(layout, {1}, values, destination);
    }
}  // namespace Horo::Editor
