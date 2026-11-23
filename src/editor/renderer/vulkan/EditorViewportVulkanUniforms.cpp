#include "EditorViewportRendererVulkan.h"
#include "EditorViewportShaderParameters.h"
#include "editor/renderer/EditorRendererErrors.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace Horo::Editor {
    /** @copydoc EditorViewportRendererVulkan::ProgramResources::WriteParameter */
    Result<void> EditorViewportRendererVulkan::ProgramResources::WriteParameter(const Render::MaterialBindingLayout &layout,
                                                                                const std::span<std::byte> bytes,
                                                                                const Render::ShaderParameterId parameter,
                                                                                const std::span<const float> values) {
        const auto entry = std::ranges::find(layout.parameters, parameter, &Render::ShaderReflectedParameter::id);
        if (entry == layout.parameters.end() || !entry->active || entry->arrayCount == 0 ||
            entry->type != Render::ShaderValueType::Float32 ||
            values.size() != std::size_t{entry->rows} * entry->columns * entry->arrayCount || !std::ranges::all_of(values, [](float value) {
            return std::isfinite(value);
        }))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
        for (std::size_t element = 0; element < entry->arrayCount; ++element)
            for (std::size_t row = 0; row < entry->rows; ++row)
                for (std::size_t column = 0; column < entry->columns; ++column) {
                    const bool matrix = entry->rows > 1 && entry->columns > 1;
                    const auto offset = entry->byteOffset + element * entry->arrayStride +
                                        (matrix ? (entry->columnMajor ? column : row) * entry->matrixStride +
                                                      (entry->columnMajor ? row : column) * sizeof(float)
                                                : (row * entry->columns + column) * sizeof(float));
                    if (offset > bytes.size() || sizeof(float) > bytes.size() - offset)
                        return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
                    // Horo matrices are column-major; final reflection independently fixes the native matrix/array ABI.
                    const auto source =
                        element * entry->rows * entry->columns + (matrix ? column * entry->rows + row : row * entry->columns + column);
                    const auto representation = std::bit_cast<std::array<std::byte, sizeof(float)>>(values[source]);
                    std::ranges::copy(representation, bytes.subspan(offset, sizeof(float)).begin());
                }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::ProgramResources::WriteLighting */
    Result<void> EditorViewportRendererVulkan::ProgramResources::WriteLighting(const Render::MaterialBindingLayout &layout,
                                                                               const std::span<std::byte> bytes,
                                                                               const Render::RenderSceneView &scene) {
        if (scene.lights.size() > Render::MaximumForwardLights)
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::Capacity));
        std::array<float, Render::MaximumForwardLights * 4> positions{}, directions{}, colors{}, cones{};
        for (std::size_t index = 0; index < scene.lights.size(); ++index) {
            const auto &light = scene.lights[index];
            const auto relative = light.position - scene.camera.position;
            const std::array position{relative.x, relative.y, relative.z, static_cast<float>(light.kind)};
            const std::array direction{light.direction.x, light.direction.y, light.direction.z, light.range};
            const std::array color{light.color.x, light.color.y, light.color.z, light.intensity};
            const std::array cone{light.innerConeCosine, light.outerConeCosine, 0.0F, 0.0F};
            std::ranges::copy(position, positions.begin() + index * 4);
            std::ranges::copy(direction, directions.begin() + index * 4);
            std::ranges::copy(color, colors.begin() + index * 4);
            std::ranges::copy(cone, cones.begin() + index * 4);
        }
        const std::array<std::span<const float>, 4> parameters{positions, directions, colors, cones};
        for (std::size_t index = 0; index < parameters.size(); ++index)
            if (const auto written = WriteParameter(layout, bytes, {static_cast<std::uint32_t>(index + 6)}, parameters[index]);
                written.HasError())
                return written;
        const auto baseColor = EditorViewportFixedMaterialBaseColor();
        return WriteParameter(layout, bytes, {20}, baseColor);
    }

    /** @copydoc EditorViewportRendererVulkan::TemporalGuidePrograms::PrepareSource */
    Result<void> EditorViewportRendererVulkan::TemporalGuidePrograms::PrepareSource(const EditorViewportMotionSource source) {
        source_ = {};
        if (source.objects.size() > sourceIds_.size() || (!source.sceneDomain && !source.objects.empty()))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        std::ranges::copy(source.objects, sourceIds_.begin());
        source_ = {source.sceneDomain, std::span{sourceIds_}.first(source.objects.size())};
        return Result<void>::Success();
    }

    /** @brief Compares the projection parameters used by both guide passes. */
    [[nodiscard]] static bool SameMotionProjection(const Render::RenderProjectionDescriptor &left,
                                                   const Render::RenderProjectionDescriptor &right) noexcept {
        if (left.kind != right.kind || left.nearPlane != right.nearPlane || left.farPlane != right.farPlane)
            return false;
        return left.kind == Render::RenderProjectionKind::Perspective ? left.verticalFovRadians == right.verticalFovRadians
                                                                      : left.orthographicHeight == right.orthographicHeight;
    }

    /** @copydoc EditorViewportRendererVulkan::TemporalGuidePrograms::BeginMotion */
    Result<void> EditorViewportRendererVulkan::TemporalGuidePrograms::BeginMotion(const Render::FrameToken frame,
                                                                                  const Render::StaticMeshPassDescriptor &pass,
                                                                                  const Math::Mat4 &viewProjection) {
        using namespace Render;
        if (!source_.sceneDomain)
            return Result<void>::Success();
        const auto images = owner.resources_.TemporalImages();
        if (!images || pending_)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        const auto samples = BuildEditorViewportMotionSamples({.instances = pass.scene.instances, .motion = source_}, samples_);
        if (samples.HasError())
            return Result<void>::Failure(samples.ErrorValue());

        const auto color = owner.resources_.ColorTexture();
        const auto surfaceGeneration = (std::uint64_t{color.slot} << 32) | color.generation;
        // This owner admits one immutable desktop guide recipe and an unjittered, fixed-extent raster.
        constexpr std::uint64_t desktopGuideContract = 1;
        const TemporalHistoryCompatibility compatibility{
            .view = owner.LightingViewIdentity(),
            .provider = {desktopGuideContract},
            .mode = {desktopGuideContract},
            .renderExtent = pass.extent,
            .targetExtent = pass.extent,
            .providerGeneration = desktopGuideContract,
            .modeGeneration = desktopGuideContract,
            .surfaceGeneration = surfaceGeneration,
            .rasterGeneration = desktopGuideContract,
            .colorGeneration = desktopGuideContract,
            .exposureGeneration = desktopGuideContract,
            .inputSchemaGeneration = desktopGuideContract,
            .deviceGeneration = owner.frontend_.Capabilities().support.deviceIncarnation,
            .projectionGeneration = desktopGuideContract,
            .jitterGeneration = desktopGuideContract,
            .sceneOriginGeneration = source_.sceneDomain,
            .motionGeneration = desktopGuideContract,
            .recipeGeneration = desktopGuideContract,
        };
        projection_ = pass.scene.camera.projection;
        auto attempted = history_.BeginFrame({.compatibility = compatibility,
                                              .frameId = frame.value,
                                              .predecessorFrameId = lastAcceptedFrame_,
                                              .camera = {viewProjection},
                                              .disocclusion = {images->linearDepth.texture, images->motion.texture},
                                              .invalidation = {.cameraCut = !SameMotionProjection(projection_, acceptedProjection_)},
                                              .objects = samples.Value()});
        if (attempted.HasError())
            return Result<void>::Failure(attempted.ErrorValue());
        pending_ = std::move(attempted).Value();
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::TemporalGuidePrograms::Recorded */
    void EditorViewportRendererVulkan::TemporalGuidePrograms::Recorded(const Render::RenderTimelinePoint signal) noexcept {
        if (pending_)
            signal_ = signal;
    }

    /** @copydoc EditorViewportRendererVulkan::TemporalGuidePrograms::Finish */
    void EditorViewportRendererVulkan::TemporalGuidePrograms::Finish(const bool presented) noexcept {
        source_ = {};
        if (!pending_) {
            if (presented)
                lastAcceptedFrame_ = 0;
            return;
        }
        const auto signal = std::exchange(signal_, {});
        const auto *port = owner.bridge_.Port();
        if (signal.IsValid() && port) {
            const auto accepted = port->WasSubmissionAccepted(signal, pending_->compatibility.deviceGeneration);
            if (accepted.HasValue() && accepted.Value() && history_.Publish(*pending_).HasValue()) {
                lastAcceptedFrame_ = pending_->frameId;
                acceptedProjection_ = projection_;
                pending_.reset();
                return;
            }
        }
        static_cast<void>(history_.Abandon(*pending_));
        pending_.reset();
    }

    /** @copydoc EditorViewportRendererVulkan::TemporalGuidePrograms::MotionForDraw */
    Render::RenderMotionObjectPair EditorViewportRendererVulkan::TemporalGuidePrograms::MotionForDraw(
        const std::size_t index, const Math::Mat4 &localToWorld) const noexcept {
        if (pending_ && index < source_.objects.size()) {
            const auto id = source_.objects[index];
            const auto found = std::ranges::lower_bound(pending_->objects, id, {}, &Render::RenderMotionObjectPair::object);
            if (found != pending_->objects.end() && found->object == id)
                return *found;
        }
        return {.currentLocalToWorld = localToWorld};
    }

    /** @copydoc EditorViewportRendererVulkan::TemporalGuidePrograms::Upload */
    Result<void> EditorViewportRendererVulkan::TemporalGuidePrograms::Upload(const Render::FrameToken frame, const std::uint32_t slot,
                                                                             const Render::StaticMeshPassDescriptor &pass) {
        const auto &instances = pass.scene.instances;
        if (slot >= draws.size() || instances.size() > draws[slot].size())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        const auto &camera = pass.scene.camera;
        const auto view = Math::TryLookAt(camera.position, camera.target, camera.up);
        if (view.HasError())
            return Result<void>::Failure(view.ErrorValue());
        const auto aspect = static_cast<float>(pass.extent.width) / static_cast<float>(pass.extent.height);
        const auto projection = BuildRenderMvp(camera, Math::Mat4::Identity(), aspect, Math::ClipDepthRange::ZeroToOne);
        if (projection.HasError())
            return Result<void>::Failure(projection.ErrorValue());
        if (const auto tracked = BeginMotion(frame, pass, projection.Value()); tracked.HasError())
            return tracked;
        const auto cameraPair =
            pending_ ? pending_->camera : Render::RenderMotionCameraPair{.currentUnjitteredViewProjection = projection.Value()};
        for (std::size_t index = 0; index < instances.size(); ++index)
            if (const auto uploaded =
                    UploadDraw(frame, draws[slot][index], view.Value(), cameraPair, MotionForDraw(index, instances[index].localToWorld));
                uploaded.HasError())
                return uploaded;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::TemporalGuidePrograms::UploadDraw */
    Result<void> EditorViewportRendererVulkan::TemporalGuidePrograms::UploadDraw(const Render::FrameToken frame, const GuideDraw &draw,
                                                                                 const Math::Mat4 &view,
                                                                                 const Render::RenderMotionCameraPair &camera,
                                                                                 const Render::RenderMotionObjectPair &object) const {
        const auto &localToWorld = object.currentLocalToWorld;
        auto raster = Math::Multiply(camera.currentUnjitteredViewProjection, localToWorld);
        // Match the ordinary Vulkan raster's top-left orientation.
        for (std::size_t column = 0; column < 4; ++column)
            raster.values[column * 4 + 1] *= -1;
        const auto localToView = Math::Multiply(view, localToWorld);
        std::array<std::byte, MaximumUniformBytes> depthBytes{};
        const auto depthBlock = std::span{depthBytes}.first(linearDepth.uniformBytes);
        if (const auto packed = PackEditorVulkanTemporalDepth(linearDepth.layout, raster, localToView, depthBlock); packed.HasError())
            return Result<void>::Failure(packed.ErrorValue());
        std::array<std::byte, MaximumUniformBytes> motionBytes{};
        const auto motionBlock = std::span{motionBytes}.first(motion.uniformBytes);
        if (const auto packed = PackEditorVulkanTemporalMotion(motion.layout, raster, camera, object, motionBlock); packed.HasError())
            return Result<void>::Failure(packed.ErrorValue());
        auto *port = owner.bridge_.Port();
        if (const auto uploaded = port->WriteFrameBuffer(frame, draw.linearDepth.instance, depthBlock); uploaded.HasError())
            return uploaded;
        return port->WriteFrameBuffer(frame, draw.motion.instance, motionBlock);
    }

    /** @copydoc EditorViewportRendererVulkan::ProgramResources::WriteDraw */
    Result<void> EditorViewportRendererVulkan::ProgramResources::WriteDraw(
        const Render::Detail::VulkanEditorGraphicsBridge &bridge, const ShaderResources &program, const EditorViewportExtent extent,
        const Render::FrameToken frame, const DrawSlot &slot, const Render::RenderStaticMeshInstance &instance,
        const Render::RenderSceneView &scene, const Render::DirectionalShadowFrame *shadow) {
        auto mvp = BuildRenderMvp(scene.camera, instance.localToWorld, static_cast<float>(extent.width) / extent.height,
                                  Math::ClipDepthRange::ZeroToOne);
        if (mvp.HasError())
            return Result<void>::Failure(mvp.ErrorValue());
        Math::Mat4 clipFromLocal = std::move(mvp).Value();
        const auto inverse = Math::TryInverseAffine(instance.localToWorld);
        if (inverse.HasError())
            return Result<void>::Failure(inverse.ErrorValue());
        Math::Mat4 normal;
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t column = 0; column < 4; ++column)
                normal.values[column * 4 + row] = inverse.Value().values[row * 4 + column];
        // Vulkan's positive-height viewport has top-left framebuffer origin; flip only the clip-space Y component.
        for (std::size_t column = 0; column < 4; ++column)
            clipFromLocal.values[column * 4 + 1] *= -1;
        // DXC lowers the HLSL viewport MVP to a transposed SPIR-V matrix. Pack that matrix before reflecting its native strides.
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t column = row + 1; column < 4; ++column)
                std::swap(clipFromLocal.values[column * 4 + row], clipFromLocal.values[row * 4 + column]);
        const auto &presentation = instance.presentation;
        const std::array tint{presentation.tint.x, presentation.tint.y, presentation.tint.z, presentation.tintStrength};
        const std::array camera{0.0F, 0.0F, 0.0F, static_cast<float>(scene.lights.size())};
        auto relativeModel = instance.localToWorld;
        relativeModel.values[12] -= scene.camera.position.x;
        relativeModel.values[13] -= scene.camera.position.y;
        relativeModel.values[14] -= scene.camera.position.z;
        std::array<std::byte, MaximumUniformBytes> storage{};
        const auto bytes = std::span{storage}.first(program.uniformBytes);
        const std::array<std::span<const float>, 5> parameters{clipFromLocal.values, normal.values, relativeModel.values, tint, camera};
        for (std::size_t index = 0; index < parameters.size(); ++index)
            if (const auto written = WriteParameter(program.layout, bytes, {static_cast<std::uint32_t>(index + 1)}, parameters[index]);
                written.HasError())
                return written;
        if (const auto written = WriteLighting(program.layout, bytes, scene); written.HasError())
            return written;
        if (auto written = WriteShadowParameters(program, bytes, scene, shadow); written.HasError())
            return written;
        return bridge.Port()->WriteFrameBuffer(frame, slot.instance, bytes);
    }

    Result<void> EditorViewportRendererVulkan::ProgramResources::WriteShadowParameters(const ShaderResources &program,
                                                                                       std::span<std::byte> bytes,
                                                                                       const Render::RenderSceneView &scene,
                                                                                       const Render::DirectionalShadowFrame *shadow) {
        std::array<float, 64> matrices{};
        std::array<float, 16> splits{};
        std::array<float, 16> rectangles{};
        std::array<float, 4> settings{};
        std::array<float, 4> bias{};
        if (shadow) {
            for (std::uint32_t index = 0; index < shadow->cascadeCount; ++index) {
                const auto &cascade = shadow->cascades[index];
                std::ranges::copy(cascade.cameraRelativeViewProjection.values, matrices.begin() + index * 16);
                const std::array split{cascade.splitNear, cascade.splitFar, cascade.blendStart, cascade.worldUnitsPerTexel};
                const std::array rectangle{cascade.atlasScaleBias.x, cascade.atlasScaleBias.y, cascade.atlasScaleBias.z,
                                           cascade.atlasScaleBias.w};
                std::ranges::copy(split, splits.begin() + index * 4);
                std::ranges::copy(rectangle, rectangles.begin() + index * 4);
            }
            settings = {static_cast<float>(shadow->cascadeCount), static_cast<float>(shadow->lightIndex),
                        static_cast<float>(shadow->profile.filterRadius), shadow->profile.normalBias};
            bias = {shadow->profile.depthBias, shadow->profile.slopeBias, 1.0F / shadow->atlasWidth, 1.0F / shadow->atlasHeight};
        }
        const auto forward = Math::Normalize(scene.camera.target - scene.camera.position);
        const std::array direction{forward.x, forward.y, forward.z, 0.0F};
        const std::array<std::span<const float>, 6> parameters{matrices, splits, rectangles, settings, bias, direction};
        for (std::size_t index = 0; index < parameters.size(); ++index)
            if (auto written = WriteParameter(program.layout, bytes, {static_cast<std::uint32_t>(11 + index)}, parameters[index]);
                written.HasError())
                return written;
        return Result<void>::Success();
    }

}  // namespace Horo::Editor
