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
        if (entry == shader_.layout.parameters.end() || !entry->active || entry->arrayCount != 1 ||
            entry->type != Render::ShaderValueType::Float32 || values.size() != std::size_t{entry->rows} * entry->columns ||
            !std::ranges::all_of(values, [](float value) {
            return std::isfinite(value);
        }))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
        for (std::size_t row = 0; row < entry->rows; ++row)
            for (std::size_t column = 0; column < entry->columns; ++column) {
                const bool matrix = entry->rows > 1 && entry->columns > 1;
                const auto offset = entry->byteOffset + (matrix ? (entry->columnMajor ? column : row) * entry->matrixStride +
                                                                      (entry->columnMajor ? row : column) * sizeof(float)
                                                                : (row * entry->columns + column) * sizeof(float));
                if (offset > bytes.size() || sizeof(float) > bytes.size() - offset)
                    return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
                // Horo matrices are column-major; final reflection independently fixes the native matrix ABI.
                const auto value = values[matrix ? column * entry->rows + row : row * entry->columns + column];
                const auto representation = std::bit_cast<std::array<std::byte, sizeof(float)>>(value);
                std::ranges::copy(representation, bytes.subspan(offset, sizeof(float)).begin());
            }
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
        std::array direction{0.0F, 0.0F, -1.0F, 0.0F};
        std::array color{0.0F, 0.0F, 0.0F, 0.0F};
        if (const auto light = std::ranges::find(scene.lights, Render::RenderLightKind::Directional, &Render::RenderLight::kind);
            light != scene.lights.end()) {
            direction = {light->direction.x, light->direction.y, light->direction.z, 0};
            color = {light->color.x * light->intensity, light->color.y * light->intensity, light->color.z * light->intensity, 0};
        }
        std::array<std::byte, MaximumUniformBytes> storage{};
        const auto bytes = std::span{storage}.first(shader_.uniformBytes);
        const std::array<std::span<const float>, 5> parameters{clipFromLocal.values, normal.values, tint, direction, color};
        for (std::size_t index = 0; index < parameters.size(); ++index)
            if (const auto written = WriteParameter(bytes, {static_cast<std::uint32_t>(index + 1)}, parameters[index]); written.HasError())
                return written;
        return bridge_.Port()->WriteFrameBuffer(frame, slot.instance, bytes);
    }
}  // namespace Horo::Editor
