#include "EditorViewportRendererVulkan.h"
#include "editor/renderer/EditorRendererErrors.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Horo::Editor {
    /** @copydoc EditorViewportRendererVulkan::WriteParameter */
    Result<void> EditorViewportRendererVulkan::WriteParameter(const std::span<std::byte> bytes, const Render::ShaderParameterId parameter,
                                                              const std::span<const float> values) const {
        const auto entry = std::ranges::find(shader_.layout.parameters, parameter, &Render::ShaderReflectedParameter::id);
        if (entry == shader_.layout.parameters.end() || !entry->active || entry->arrayCount == 0 ||
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

    /** @copydoc EditorViewportRendererVulkan::WriteLighting */
    Result<void> EditorViewportRendererVulkan::WriteLighting(const std::span<std::byte> bytes, const Render::RenderSceneView &scene) const {
        std::array<float, Render::MaximumForwardLights * 4> positions{}, directions{}, colors{}, cones{};
        for (std::size_t index = 0; index < scene.lights.size(); ++index) {
            const auto &light = scene.lights[index];
            const std::array position{light.position.x, light.position.y, light.position.z, static_cast<float>(light.kind)};
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
            if (const auto written = WriteParameter(bytes, {static_cast<std::uint32_t>(index + 6)}, parameters[index]); written.HasError())
                return written;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::WriteDraw */
    Result<void> EditorViewportRendererVulkan::WriteDraw(const Render::FrameToken frame, const DrawSlot &slot,
                                                         const Render::RenderStaticMeshInstance &instance,
                                                         const Render::RenderSceneView &scene) {
        const auto extent = resources_.AllocatedExtent();
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
        const auto &presentation = instance.presentation;
        const std::array tint{presentation.tint.x, presentation.tint.y, presentation.tint.z, presentation.tintStrength};
        const std::array camera{scene.camera.position.x, scene.camera.position.y, scene.camera.position.z,
                                static_cast<float>(scene.lights.size())};
        std::array<std::byte, MaximumUniformBytes> storage{};
        const auto bytes = std::span{storage}.first(shader_.uniformBytes);
        const std::array<std::span<const float>, 5> parameters{clipFromLocal.values, normal.values, instance.localToWorld.values, tint,
                                                               camera};
        for (std::size_t index = 0; index < parameters.size(); ++index)
            if (const auto written = WriteParameter(bytes, {static_cast<std::uint32_t>(index + 1)}, parameters[index]); written.HasError())
                return written;
        if (const auto written = WriteLighting(bytes, scene); written.HasError())
            return written;
        return bridge_.Port()->WriteFrameBuffer(frame, slot.instance, bytes);
    }
}  // namespace Horo::Editor
