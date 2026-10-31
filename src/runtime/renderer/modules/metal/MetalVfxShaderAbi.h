/** @file
 * @brief Closed VFX instance-array validation against actual Metal native reflection.
 */
#pragma once
#include "Horo/Runtime/Render/VfxShaderParameters.h"

#import <Metal/Metal.h>

namespace Horo::Render::Detail {
    /** @brief Admits only the canonical VFX instance schema before inspecting its actual native array ABI. */
    inline bool IsMetalVfxInstances(const ShaderBindingId binding, const NormalizedShaderReflection &reflection) {
        if (binding != VfxShaderParameterIds::Instances || reflection.backend != ShaderTargetBackend::Metal)
            return false;
        const auto identity =
            ComputeShaderInterfaceCompatibilityId(MakeVfxShaderManifest({.backend = ShaderTargetBackend::Metal,
                                                                         .payloadFormat = ShaderPayloadFormat::MetalLibrary24,
                                                                         .descriptorVersion = 1,
                                                                         .interfaceSchemaVersion = 1,
                                                                         .maximumBindings = 4}));
        if (identity.HasError() || identity.Value() != reflection.interfaceCompatibility)
            return false;
        const auto layout = PrepareMaterialBindingLayout(reflection);
        return layout.HasValue() && VfxShaderParameterBytes(layout.Value()).HasValue();
    }

    /** @brief Compares the closed VFX arrays with real Metal reflection, including native stride and element type. */
    inline bool HasMetalVfxArray(MTLStructType *structure, const ShaderReflectedParameter &parameter) {
        const bool matrix = parameter.id == VfxShaderParameterIds::ClipFromLocal;
        if (!matrix && parameter.id != VfxShaderParameterIds::InstanceColor)
            return false;
        if (parameter.type != ShaderValueType::Float32 || parameter.arrayCount != MaximumVfxDrawInstances ||
            parameter.rows != (matrix ? 4 : 1) || parameter.columns != 4 || parameter.byteOffset != (matrix ? 0U : 8192U) ||
            parameter.arrayStride != (matrix ? 64U : 16U) || parameter.matrixStride != (matrix ? 16U : 0U) ||
            (matrix && !parameter.columnMajor))
            return false;
        const auto expected = matrix ? MTLDataTypeFloat4x4 : MTLDataTypeFloat4;
        for (MTLStructMember *member in structure.members) {
            if (member.offset != parameter.byteOffset || member.dataType != MTLDataTypeArray)
                continue;
            MTLArrayType *const array = member.arrayType;
            // Metal's float4x4 consists of four column float4 values (16 bytes each).
            // Its actual array stride is independently checked rather than inferred from cooked metadata.
            return array != nil && array.arrayLength == parameter.arrayCount && array.stride == parameter.arrayStride &&
                   array.elementType == expected && array.elementStructType == nil && array.elementArrayType == nil;
        }
        return false;
    }

}  // namespace Horo::Render::Detail
