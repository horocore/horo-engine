#include "Horo/Runtime/Render/MultiviewShaderParameters.h"

#include "Horo/Runtime/Render/MultiviewExtractionErrors.h"

#include <algorithm>
#include <array>

namespace Horo::Render {
    namespace {
        /** @brief Checks only shader semantics; generic reflected packing remains owned by MaterialBinding. */
        [[nodiscard]] bool Admits(const MaterialBindingLayout &layout, const std::size_t viewCount) {
            if (viewCount == 0 || viewCount > MultiviewExtractionLimits::HardMaximumViews || layout.resources.size() != 1 ||
                layout.parameters.size() != 3)
                return false;
            const auto &buffer = layout.resources.front();
            if (!buffer.active || buffer.id != MultiviewShaderParameterIds::Buffer || buffer.kind != ShaderResourceKind::UniformBuffer ||
                buffer.access != ShaderResourceAccess::ReadOnly || buffer.arrayCount != 1)
                return false;
            const std::array ids{MultiviewShaderParameterIds::WorldToClip, MultiviewShaderParameterIds::LocalToWorld,
                                 MultiviewShaderParameterIds::ViewLayers};
            for (std::size_t index = 0; index < ids.size(); ++index) {
                const auto field = std::ranges::find(layout.parameters, ids[index], &ShaderReflectedParameter::id);
                if (field == layout.parameters.end() || !field->active || field->binding != buffer.id ||
                    field->arrayCount < (index == 1 ? 1 : viewCount) || field->arrayCount > MultiviewExtractionLimits::HardMaximumViews)
                    return false;
                if (index == 2) {
                    if (field->type != ShaderValueType::Uint32 || field->rows != 1 || field->columns != 1)
                        return false;
                } else if (field->type != ShaderValueType::Float32 || field->rows != 4 || field->columns != 4 ||
                           (index == 1 && field->arrayCount != 1)) {
                    return false;
                }
            }
            return true;
        }

        /** @brief Copies canonical mathematical row/column access into the packer's array-major row-major input. */
        void MatrixValues(const Math::Mat4 &matrix, const std::span<float, 16> output) {
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    output[static_cast<std::size_t>(row * 4 + column)] = matrix.At(row, column);
        }
    }  // namespace

    /** @copydoc MultiviewShaderParameterBytes */
    Result<std::size_t> MultiviewShaderParameterBytes(const MaterialBindingLayout &layout, const std::size_t viewCount) {
        if (!Admits(layout, viewCount))
            return Result<std::size_t>::Failure(MakeError(MultiviewExtractionErrors::InvalidInput));
        return ComputeMaterialParameterBufferByteSize(layout, MultiviewShaderParameterIds::Buffer);
    }

    /** @copydoc PackMultiviewShaderParameters */
    Result<MaterialParameterBinding> PackMultiviewShaderParameters(const MaterialBindingLayout &layout,
                                                                   const std::span<const MultiviewSceneView> views,
                                                                   const Math::Mat4 &localToWorld,
                                                                   const std::span<const std::uint32_t> layers) {
        if (const auto bytes = MultiviewShaderParameterBytes(layout, views.size()); bytes.HasError())
            return Result<MaterialParameterBinding>::Failure(bytes.ErrorValue());
        if (layers.size() != views.size() || !Math::IsFinite(localToWorld))
            return Result<MaterialParameterBinding>::Failure(MakeError(MultiviewExtractionErrors::InvalidInput));
        std::array<float, MultiviewExtractionLimits::HardMaximumViews * 16> matrices{};
        std::array<float, 16> model{};
        std::array<std::uint32_t, MultiviewExtractionLimits::HardMaximumViews> routing{};
        for (std::size_t index = 0; index < views.size(); ++index) {
            if (views[index].index != index || !Math::IsFinite(views[index].worldToClip))
                return Result<MaterialParameterBinding>::Failure(MakeError(MultiviewExtractionErrors::InvalidInput));
            MatrixValues(views[index].worldToClip, std::span<float, 16>{matrices.data() + index * 16, 16});
            routing[index] = layers[index];
        }
        MatrixValues(localToWorld, model);
        const auto viewField =
            std::ranges::find(layout.parameters, MultiviewShaderParameterIds::WorldToClip, &ShaderReflectedParameter::id);
        const auto layerField =
            std::ranges::find(layout.parameters, MultiviewShaderParameterIds::ViewLayers, &ShaderReflectedParameter::id);
        const std::array values{MaterialParameterValue{MultiviewShaderParameterIds::WorldToClip,
                                                       std::span<const float>{matrices}.first(viewField->arrayCount * 16)},
                                MaterialParameterValue{MultiviewShaderParameterIds::LocalToWorld, std::span<const float>{model}},
                                MaterialParameterValue{MultiviewShaderParameterIds::ViewLayers,
                                                       std::span<const std::uint32_t>{routing}.first(layerField->arrayCount)}};
        return PackMaterialParameterBinding(layout, MultiviewShaderParameterIds::Buffer, values);
    }
}  // namespace Horo::Render
