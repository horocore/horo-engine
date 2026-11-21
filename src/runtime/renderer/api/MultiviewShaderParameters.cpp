#include "Horo/Runtime/Render/MultiviewShaderParameters.h"

#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "Horo/Runtime/Render/MultiviewExtractionErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace Horo::Render {
    namespace {
        /** @brief Checks only shader semantics; generic reflected packing remains owned by MaterialBinding. */
        [[nodiscard]] bool Admits(const MaterialBindingLayout &layout, const std::size_t viewCount, std::size_t parameterCount = 3) {
            if (viewCount == 0 || viewCount > MultiviewExtractionLimits::HardMaximumViews || layout.resources.size() != 1 ||
                layout.parameters.size() != parameterCount)
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

        /** @brief Matches a verified motion shader block member without inventing ABI offsets or matrix order. */
        const ShaderReflectedParameter *MotionField(const MaterialBindingLayout &layout, const ShaderParameterId id,
                                                    const std::size_t viewCount) {
            const auto found = std::ranges::find(layout.parameters, id, &ShaderReflectedParameter::id);
            if (found == layout.parameters.end() || !found->active || found->binding != MultiviewShaderParameterIds::Buffer ||
                found->type != ShaderValueType::Float32 || found->rows != 4 || found->columns != 4 || found->arrayCount < viewCount ||
                found->arrayCount > MultiviewExtractionLimits::HardMaximumViews)
                return nullptr;
            return std::to_address(found);
        }

        /** @brief Copies canonical mathematical row/column access into the packer's array-major row-major input. */
        void MatrixValues(const Math::Mat4 &matrix, const std::span<float, 16> output) {
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    output[static_cast<std::size_t>(row * 4 + column)] = matrix.At(row, column);
        }

        /** @brief Owned scratch shared by the baseline and optional environment programs. */
        struct ProjectionValues final {
            std::array<float, MultiviewExtractionLimits::HardMaximumViews * 16> cameras{};
            std::array<float, 16> model{};
            std::array<std::uint32_t, MultiviewExtractionLimits::HardMaximumViews> routing{};

            bool Prepare(std::span<const MultiviewSceneView> views, const Math::Mat4 &localToWorld, std::span<const std::uint32_t> layers) {
                if (layers.size() != views.size() || !Math::IsFinite(localToWorld))
                    return false;
                for (std::size_t index = 0; index < views.size(); ++index) {
                    if (views[index].index != index || !Math::IsFinite(views[index].worldToClip))
                        return false;
                    MatrixValues(views[index].worldToClip, std::span<float, 16>{cameras.data() + index * 16, 16});
                    routing[index] = layers[index];
                }
                MatrixValues(localToWorld, model);
                return true;
            }

            [[nodiscard]] std::array<MaterialParameterValue, 3> Bind(const MaterialBindingLayout &layout) const noexcept {
                using namespace MultiviewShaderParameterIds;
                const auto views = std::ranges::find(layout.parameters, WorldToClip, &ShaderReflectedParameter::id);
                const auto layers = std::ranges::find(layout.parameters, ViewLayers, &ShaderReflectedParameter::id);
                return {MaterialParameterValue{WorldToClip, std::span<const float>{cameras}.first(views->arrayCount * 16)},
                        MaterialParameterValue{LocalToWorld, std::span<const float>{model}},
                        MaterialParameterValue{ViewLayers, std::span<const std::uint32_t>{routing}.first(layers->arrayCount)}};
            }
        };

        /** @brief Checks one unique additional environment field after the shared projection ABI was admitted. */
        [[nodiscard]] bool EnvironmentField(const MaterialBindingLayout &layout, ShaderParameterId id, std::uint8_t rows) {
            const auto field = std::ranges::find(layout.parameters, id, &ShaderReflectedParameter::id);
            return field != layout.parameters.end() && field->active && field->binding == MultiviewShaderParameterIds::Buffer &&
                   field->arrayCount == 1 && field->type == ShaderValueType::Float32 && field->rows == rows && field->columns == 4;
        }

        /** @brief Bounds signed scene-linear contribution to finite values representable by the renderer's FP16 working space. */
        [[nodiscard]] bool BoundedColor(const Math::Vec3 color) noexcept {
            constexpr float limit = 65504.0F;
            return Math::IsFinite(color) && std::abs(color.x) <= limit && std::abs(color.y) <= limit && std::abs(color.z) <= limit;
        }
    }  // namespace

    /** @copydoc MultiviewEnvironmentLighting::IsValid */
    bool MultiviewEnvironmentLighting::IsValid() const noexcept {
        return (!ambient || BoundedColor(*ambient)) &&
               (!directional || (directional->kind == RenderLightKind::Directional && directional->IsValid() &&
                                 directional->stableIdentity != 0 && BoundedColor(directional->color * directional->intensity)));
    }

    /** @copydoc MultiviewShaderParameterBytes */
    Result<std::size_t> MultiviewShaderParameterBytes(const MaterialBindingLayout &layout, const std::size_t viewCount) {
        if (!Admits(layout, viewCount))
            return Result<std::size_t>::Failure(MakeError(MultiviewExtractionErrors::InvalidInput));
        return ComputeMaterialParameterBufferByteSize(layout, MultiviewShaderParameterIds::Buffer);
    }

    /** @copydoc MultiviewEnvironmentParameterBytes */
    Result<std::size_t> MultiviewEnvironmentParameterBytes(const MaterialBindingLayout &layout, const std::size_t viewCount) {
        using namespace MultiviewShaderParameterIds;
        if (!Admits(layout, viewCount, 7) || !EnvironmentField(layout, NormalToWorld, 4) ||
            !EnvironmentField(layout, EnvironmentAmbient, 1) || !EnvironmentField(layout, EnvironmentDirectional, 1) ||
            !EnvironmentField(layout, EnvironmentDirection, 1))
            return Result<std::size_t>::Failure(MakeError(MultiviewExtractionErrors::InvalidInput));
        return ComputeMaterialParameterBufferByteSize(layout, Buffer);
    }

    /** @copydoc MultiviewMotionParameterBytes */
    Result<std::size_t> MultiviewMotionParameterBytes(const MaterialBindingLayout &layout, const std::size_t viewCount) {
        using namespace MultiviewShaderParameterIds;
        if (viewCount == 0 || layout.parameters.size() != 2)
            return Result<std::size_t>::Failure(MakeError(MultiviewExtractionErrors::InvalidInput));
        const auto buffer = std::ranges::find(layout.resources, Buffer, &ShaderReflectedBinding::id);
        if (buffer == layout.resources.end() || !buffer->active || buffer->kind != ShaderResourceKind::UniformBuffer ||
            buffer->access != ShaderResourceAccess::ReadOnly || buffer->arrayCount != 1 ||
            !MotionField(layout, CurrentLocalToClip, viewCount) || !MotionField(layout, PreviousLocalToClip, viewCount))
            return Result<std::size_t>::Failure(MakeError(MultiviewExtractionErrors::InvalidInput));
        return ComputeMaterialParameterBufferByteSize(layout, Buffer);
    }

    /** @copydoc PackMultiviewMotionShaderParameters */
    Result<std::size_t> PackMultiviewMotionShaderParameters(const MaterialBindingLayout &layout, const MultiviewMotionHistory &history,
                                                            const MultiviewMotionFrame &frame, const std::uint32_t instanceIndex,
                                                            const std::span<std::byte> destination) {
        using namespace MultiviewShaderParameterIds;
        const auto draw = history.ReadDraw(frame, instanceIndex);
        if (!draw)
            return Result<std::size_t>::Failure(MakeError(MultiviewExtractionErrors::StalePublication));
        if (const auto size = MultiviewMotionParameterBytes(layout, draw->viewCount); size.HasError())
            return Result<std::size_t>::Failure(size.ErrorValue());
        std::array<float, MultiviewExtractionLimits::HardMaximumViews * 16> current{}, previous{};
        for (std::size_t eye = 0; eye < draw->viewCount; ++eye) {
            MatrixValues(draw->currentLocalToClip[eye], std::span<float, 16>{current.data() + eye * 16, 16});
            MatrixValues(draw->previousLocalToClip[eye], std::span<float, 16>{previous.data() + eye * 16, 16});
        }
        const auto currentCount = MotionField(layout, CurrentLocalToClip, draw->viewCount)->arrayCount * 16;
        const auto previousCount = MotionField(layout, PreviousLocalToClip, draw->viewCount)->arrayCount * 16;
        const std::array values{MaterialParameterValue{CurrentLocalToClip, std::span<const float>{current}.first(currentCount)},
                                MaterialParameterValue{PreviousLocalToClip, std::span<const float>{previous}.first(previousCount)}};
        return PackMaterialParameterBinding(layout, Buffer, values, destination);
    }

    /** @copydoc PackMultiviewShaderParameters */
    Result<std::size_t> PackMultiviewShaderParameters(const MaterialBindingLayout &layout, const std::span<const MultiviewSceneView> views,
                                                      const Math::Mat4 &localToWorld, const std::span<const std::uint32_t> layers,
                                                      const std::span<std::byte> destination) {
        if (const auto bytes = MultiviewShaderParameterBytes(layout, views.size()); bytes.HasError())
            return Result<std::size_t>::Failure(bytes.ErrorValue());
        ProjectionValues values;
        if (!values.Prepare(views, localToWorld, layers))
            return Result<std::size_t>::Failure(MakeError(MultiviewExtractionErrors::InvalidInput));
        return PackMaterialParameterBinding(layout, MultiviewShaderParameterIds::Buffer, values.Bind(layout), destination);
    }

    /** @copydoc PackMultiviewEnvironmentParameters */
    Result<std::size_t> PackMultiviewEnvironmentParameters(const MaterialBindingLayout &layout, std::span<const MultiviewSceneView> views,
                                                           const Math::Mat4 &localToWorld, std::span<const std::uint32_t> layers,
                                                           const MultiviewEnvironmentLighting &lighting, std::span<std::byte> destination) {
        using namespace MultiviewShaderParameterIds;
        using Outcome = Result<std::size_t>;
        if (const auto bytes = MultiviewEnvironmentParameterBytes(layout, views.size()); bytes.HasError())
            return Outcome::Failure(bytes.ErrorValue());
        ProjectionValues values;
        if (!lighting.IsValid() || !values.Prepare(views, localToWorld, layers))
            return Outcome::Failure(MakeError(MultiviewExtractionErrors::InvalidInput));
        std::array<float, 16> normalMatrix{};
        MatrixValues(Math::Mat4::Identity(), normalMatrix);
        if (lighting.directional) {
            const auto inverse = Math::TryInverseAffine(localToWorld);
            if (inverse.HasError())
                return Outcome::Failure(inverse.ErrorValue());
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    normalMatrix[static_cast<std::size_t>(row * 4 + column)] = inverse.Value().At(column, row);
        }
        const auto ambient = lighting.ambient.value_or(Math::Vec3{});
        const auto color = lighting.directional ? lighting.directional->color * lighting.directional->intensity : Math::Vec3{};
        const auto direction = lighting.directional ? lighting.directional->direction : Math::Vec3{};
        const std::array ambientValues{ambient.x, ambient.y, ambient.z, lighting.ambient || lighting.directional ? 1.0F : 0.0F};
        const std::array colorValues{color.x, color.y, color.z, 0.0F};
        const std::array directionValues{direction.x, direction.y, direction.z, 0.0F};
        const auto projection = values.Bind(layout);
        const std::array inputs{projection[0],
                                projection[1],
                                projection[2],
                                MaterialParameterValue{NormalToWorld, std::span<const float>{normalMatrix}},
                                MaterialParameterValue{EnvironmentAmbient, std::span<const float>{ambientValues}},
                                MaterialParameterValue{EnvironmentDirectional, std::span<const float>{colorValues}},
                                MaterialParameterValue{EnvironmentDirection, std::span<const float>{directionValues}}};
        return PackMaterialParameterBinding(layout, Buffer, inputs, destination);
    }

    /** @copydoc PackMultiviewShaderParameters */
    Result<MaterialParameterBinding> PackMultiviewShaderParameters(const MaterialBindingLayout &layout,
                                                                   const std::span<const MultiviewSceneView> views,
                                                                   const Math::Mat4 &localToWorld,
                                                                   const std::span<const std::uint32_t> layers) {
        const auto size = MultiviewShaderParameterBytes(layout, views.size());
        if (size.HasError())
            return Result<MaterialParameterBinding>::Failure(size.ErrorValue());
        try {
            MaterialParameterBinding result{std::vector<std::byte>(size.Value())};
            if (const auto written = PackMultiviewShaderParameters(layout, views, localToWorld, layers, std::span{result.bytes});
                written.HasError())
                return Result<MaterialParameterBinding>::Failure(written.ErrorValue());
            return Result<MaterialParameterBinding>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<MaterialParameterBinding>::Failure(MakeError(MaterialBindingErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
