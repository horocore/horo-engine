#include "Horo/Runtime/Render/VfxShaderParameters.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace Horo::Render {
    /** @copydoc MakeVfxShaderManifest */
    ShaderManifest MakeVfxShaderManifest(const ShaderTargetRequirement &target) {
        using enum Horo::Render::ShaderValueType;
        using namespace VfxShaderParameterIds;
        using enum ShaderResourceKind;
        using enum ShaderResourceAccess;
        ShaderManifest manifest{.schemaVersion = 1,
                                .sourceIdentity = "horo/runtime/vfx_instanced.hlsl",
                                .sourceRevision =
                                    target.backend == ShaderTargetBackend::OpenGL || target.backend == ShaderTargetBackend::Metal ? 3U : 2U,
                                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}}};
        manifest.bindings = {{Instances, UniformBuffer, ReadOnly, 1, ShaderStageVisibility::Vertex},
                             {Material, UniformBuffer, ReadOnly, 1, ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment},
                             {BaseColor, SampledTexture, ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {BaseColorSampler, Sampler, ReadOnly, 1, ShaderStageVisibility::Fragment}};
        manifest.parameters = {{ClipFromLocal, Instances, Float32, 4, 4, MaximumVfxDrawInstances},
                               {InstanceColor, Instances, Float32, 1, 4, MaximumVfxDrawInstances},
                               {Tint, Material, Float32, 1, 4, 1},
                               {AlphaCutoff, Material, Float32, 1, 1, 1}};
        manifest.targets = {target};
        return manifest;
    }

    namespace {
        /** @brief Checks semantic shape only; the canonical material writer validates native packing. */
        bool AdmitsInstanceSchema(const MaterialBindingLayout &layout) {
            const auto resource = std::ranges::find(layout.resources, VfxShaderParameterIds::Instances, &ShaderReflectedBinding::id);
            if (resource == layout.resources.end() || !resource->active || resource->kind != ShaderResourceKind::UniformBuffer ||
                resource->access != ShaderResourceAccess::ReadOnly || resource->arrayCount != 1 ||
                resource->stages != ShaderStageVisibility::Vertex)
                return false;
            bool matrix = false;
            bool color = false;
            for (const auto &parameter : layout.parameters) {
                if (!parameter.active || parameter.binding != resource->id)
                    continue;
                if (parameter.type != ShaderValueType::Float32 || parameter.arrayCount != MaximumVfxDrawInstances)
                    return false;
                if (parameter.id == VfxShaderParameterIds::ClipFromLocal) {
                    if (matrix || parameter.rows != 4 || parameter.columns != 4)
                        return false;
                    matrix = true;
                } else if (parameter.id != VfxShaderParameterIds::InstanceColor || color || parameter.rows != 1 || parameter.columns != 4) {
                    return false;
                } else {
                    color = true;
                }
            }
            return matrix && color;
        }

        using MatrixValues = std::array<float, MaximumVfxDrawInstances * 16>;
        using ColorValues = std::array<float, MaximumVfxDrawInstances * 4>;
        static_assert(sizeof(float) == 4);
        constexpr std::size_t MatrixBytes = MaximumVfxDrawInstances * 64;
        constexpr std::size_t ColorBytes = MaximumVfxDrawInstances * 16;

        /** @brief Describes only two proven disjoint contiguous physical arrays inside the already admitted extent. */
        struct RegularPacking final {
            std::size_t matrixOffset{};
            std::size_t colorOffset{};
            bool columnMajor{};
        };

        /** @brief Locates a semantic field only after the public schema admission has proved its uniqueness. */
        const ShaderReflectedParameter *InstanceField(const MaterialBindingLayout &layout, ShaderParameterId id) {
            const auto field = std::ranges::find_if(layout.parameters, [id](const auto &parameter) {
                return parameter.active && parameter.binding == VfxShaderParameterIds::Instances && parameter.id == id;
            });
            return field == layout.parameters.end() ? nullptr : &*field;
        }

        /** @brief Bounds a complete physical field without overflowing offsets and excludes irregular byte alignment. */
        bool FitsRegularField(std::size_t offset, std::size_t bytes, std::size_t extent) {
            return offset % sizeof(float) == 0 && offset <= extent && bytes <= extent - offset;
        }

        /** @brief Proves full-array separation even for draws that supply only one live instance. */
        bool SeparateRegularFields(std::size_t matrix, std::size_t color) {
            return matrix <= color ? color - matrix >= MatrixBytes : matrix - color >= ColorBytes;
        }

        /** @brief Selects a strict optimization subset; all other physical layouts still use the canonical generic writer. */
        std::optional<RegularPacking> RegularLayout(const MaterialBindingLayout &layout, std::size_t extent) {
            const auto *matrix = InstanceField(layout, VfxShaderParameterIds::ClipFromLocal);
            const auto *color = InstanceField(layout, VfxShaderParameterIds::InstanceColor);
            if (!matrix || !color)
                return {};
            if (matrix->arrayStride != 64 || matrix->matrixStride != 16 || color->arrayStride != 16 || color->matrixStride != 0)
                return {};
            if (!FitsRegularField(matrix->byteOffset, MatrixBytes, extent) || !FitsRegularField(color->byteOffset, ColorBytes, extent) ||
                !SeparateRegularFields(matrix->byteOffset, color->byteOffset))
                return {};
            return RegularPacking{matrix->byteOffset, color->byteOffset, matrix->columnMajor};
        }

        /** @brief Compares actual byte spans using differences, without potentially overflowing end-address arithmetic. */
        bool OverlapsOutput(std::span<const std::byte> input, std::span<std::byte> output) {
            if (input.empty() || output.empty())
                return false;
            const auto source = reinterpret_cast<std::uintptr_t>(input.data());
            const auto destination = reinterpret_cast<std::uintptr_t>(output.data());
            return source <= destination ? destination - source < input.size() : source - destination < output.size();
        }

        /** @brief Sends descriptor/output aliases through the unchanged generic validation and error-ordering path. */
        bool MetadataAliases(const MaterialBindingLayout &layout, std::span<std::byte> prefix) {
            return OverlapsOutput(std::as_bytes(std::span{layout.parameters}), prefix) ||
                   OverlapsOutput(std::as_bytes(std::span{layout.resources}), prefix) ||
                   OverlapsOutput(std::as_bytes(std::span{layout.targetBindings}), prefix);
        }

        /** @brief Writes copied logical matrix values in final reflected order without arithmetic on their float payloads. */
        void WriteRegularMatrices(const RegularPacking &packing, const MatrixValues &logical, std::span<std::byte> prefix) {
            auto *destination = prefix.data() + packing.matrixOffset;
            if (!packing.columnMajor) {
                std::memcpy(destination, logical.data(), MatrixBytes);
                return;
            }
            for (std::size_t instance = 0; instance < MaximumVfxDrawInstances; ++instance) {
                std::array<float, 16> native;
                for (std::size_t row = 0; row < 4; ++row)
                    for (std::size_t column = 0; column < 4; ++column)
                        native[column * 4 + row] = logical[instance * 16 + row * 4 + column];
                std::memcpy(destination + instance * 64, native.data(), 64);
            }
        }

        /** @brief Writes only after every schema, range, overlap and input check; no fallible operation follows destination mutation. */
        bool PackRegular(const MaterialBindingLayout &layout, const MatrixValues &matrices, const ColorValues &colors,
                         std::span<std::byte> destination, std::size_t extent) {
            const auto packing = RegularLayout(layout, extent);
            if (!packing || destination.size() < extent)
                return false;
            const auto prefix = destination.first(extent);
            if (MetadataAliases(layout, prefix))
                return false;
            std::ranges::fill(prefix, std::byte{0});
            WriteRegularMatrices(*packing, matrices, prefix);
            std::memcpy(prefix.data() + packing->colorOffset, colors.data(), ColorBytes);
            return true;
        }
    }  // namespace

    /** @copydoc VfxShaderParameterBytes */
    Result<std::size_t> VfxShaderParameterBytes(const MaterialBindingLayout &layout) {
        if (!AdmitsInstanceSchema(layout))
            return Result<std::size_t>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        return ComputeMaterialParameterBufferByteSize(layout, VfxShaderParameterIds::Instances);
    }

    /** @copydoc PackVfxShaderParameters */
    Result<std::size_t> PackVfxShaderParameters(const MaterialBindingLayout &layout, const std::span<const VfxPackedDrawInstance> instances,
                                                const std::span<std::byte> destination) {
        const auto extent = VfxShaderParameterBytes(layout);
        if (extent.HasError())
            return Result<std::size_t>::Failure(extent.ErrorValue());
        if (instances.empty() || instances.size() > MaximumVfxDrawInstances)
            return Result<std::size_t>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        std::array<float, MaximumVfxDrawInstances * 16> matrices{};
        std::array<float, MaximumVfxDrawInstances * 4> colors{};
        for (std::size_t instance = 0; instance < instances.size(); ++instance) {
            const auto &input = instances[instance];
            if (!std::ranges::all_of(input.clipFromLocal,
                                     [](float value) {
                return std::isfinite(value);
            }) ||
                !std::ranges::all_of(input.linearColor, [](float value) {
                return std::isfinite(value);
            }))
                return Result<std::size_t>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            for (std::size_t row = 0; row < 4; ++row)
                for (std::size_t column = 0; column < 4; ++column)
                    matrices[instance * 16 + row * 4 + column] = input.clipFromLocal[column * 4 + row];
            std::copy(input.linearColor.begin(), input.linearColor.end(), colors.begin() + instance * 4);
        }
        const std::array values{MaterialParameterValue{VfxShaderParameterIds::ClipFromLocal, std::span<const float>{matrices}},
                                MaterialParameterValue{VfxShaderParameterIds::InstanceColor, std::span<const float>{colors}}};
        if (PackRegular(layout, matrices, colors, destination, extent.Value()))
            return Result<std::size_t>::Success(extent.Value());
        return PackMaterialParameterBinding(layout, VfxShaderParameterIds::Instances, values, destination);
    }
}  // namespace Horo::Render
