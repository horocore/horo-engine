#include "Horo/Runtime/Render/VfxShaderParameters.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

#include <algorithm>
#include <cmath>

namespace Horo::Render {
    /** @copydoc MakeVfxShaderManifest */
    ShaderManifest MakeVfxShaderManifest(const ShaderTargetRequirement &target) {
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
        manifest.parameters = {{ClipFromLocal, Instances, ShaderValueType::Float32, 4, 4, MaximumVfxDrawInstances},
                               {InstanceColor, Instances, ShaderValueType::Float32, 1, 4, MaximumVfxDrawInstances},
                               {Tint, Material, ShaderValueType::Float32, 1, 4, 1},
                               {AlphaCutoff, Material, ShaderValueType::Float32, 1, 1, 1}};
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
        if (const auto extent = VfxShaderParameterBytes(layout); extent.HasError())
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
        return PackMaterialParameterBinding(layout, VfxShaderParameterIds::Instances, values, destination);
    }
}  // namespace Horo::Render
