#pragma once
/** @file MetalUniformShaderAbi.h @brief Actual native uniform member evidence for the supported material ABI. */

#include "Horo/Runtime/Render/ShaderReflection.h"

#import <Metal/Metal.h>
#include <algorithm>

namespace Horo::Render::Detail {
    /** @brief Metal float4x4 stores four 16-byte columns; other matrix representations remain unimplemented. */
    inline MTLDataType MetalUniformMatrixType(const ShaderReflectedParameter &parameter) noexcept {
        return parameter.type == ShaderValueType::Float32 && parameter.rows == 4 && parameter.columns == 4 && parameter.columnMajor &&
                       parameter.matrixStride == 16
                   ? MTLDataTypeFloat4x4
                   : MTLDataTypeNone;
    }

    inline bool MetalUniformShapeValid(const ShaderReflectedParameter &parameter) noexcept {
        return parameter.rows > 0 && parameter.rows <= 4 && parameter.columns > 0 && parameter.columns <= 4;
    }

    /** @brief Admits the existing scalar/vector shapes and the required four-column float matrix. */
    inline MTLDataType MetalUniformValueType(const ShaderReflectedParameter &parameter) noexcept {
        if (!MetalUniformShapeValid(parameter))
            return MTLDataTypeNone;
        if (parameter.rows > 1 && parameter.columns > 1)
            return MetalUniformMatrixType(parameter);
        if (parameter.matrixStride != 0)
            return MTLDataTypeNone;
        MTLDataType base = MTLDataTypeNone;
        switch (parameter.type) {
            case ShaderValueType::Float32:
                base = MTLDataTypeFloat;
                break;
            case ShaderValueType::Int32:
                base = MTLDataTypeInt;
                break;
            case ShaderValueType::Uint32:
                base = MTLDataTypeUInt;
                break;
            case ShaderValueType::Bool32:
                return MTLDataTypeNone;
        }
        return static_cast<MTLDataType>(base + std::max(parameter.rows, parameter.columns) - 1);
    }

    /** @brief Limits new array support to the actual float4 and float4x4 native ABI, without nested arrays. */
    inline bool SupportedMetalUniformParameter(const ShaderReflectedParameter &parameter) noexcept {
        const auto type = MetalUniformValueType(parameter);
        if (type == MTLDataTypeNone || parameter.arrayCount == 0)
            return false;
        if (parameter.arrayCount == 1)
            return parameter.arrayStride == 0;
        if (type == MTLDataTypeFloat4x4)
            return parameter.arrayStride == 64;
        return type == MTLDataTypeFloat4 && parameter.arrayStride == 16;
    }

    /** @brief Verifies the final scalar extent lies within the actual native block without overflow. */
    inline bool MetalUniformParameterFits(const ShaderReflectedParameter &parameter, const std::size_t blockBytes) noexcept {
        if (!SupportedMetalUniformParameter(parameter) || parameter.byteOffset > blockBytes)
            return false;
        const auto bytes = parameter.rows > 1 && parameter.columns > 1 ? 64U : 4U * std::max(parameter.rows, parameter.columns);
        const auto remaining = blockBytes - parameter.byteOffset;
        if (bytes > remaining)
            return false;
        return parameter.arrayCount == 1 || parameter.arrayCount - 1 <= (remaining - bytes) / parameter.arrayStride;
    }

    /** @brief Compares actual native array dimensions and rejects nested or structured element representations. */
    inline bool MatchesMetalUniformArray(MTLArrayType *array, const ShaderReflectedParameter &parameter, const MTLDataType type) {
        return array != nil && array.arrayLength == parameter.arrayCount && array.stride == parameter.arrayStride &&
               array.elementType == type && array.elementStructType == nil && array.elementArrayType == nil;
    }

    /** @brief Compares exact actual Metal member types, offsets and native array dimensions with cooked packing. */
    inline bool HasMetalUniformParameter(MTLStructType *structure, const ShaderReflectedParameter &parameter) {
        if (!SupportedMetalUniformParameter(parameter))
            return false;
        const auto type = MetalUniformValueType(parameter);
        for (MTLStructMember *member in structure.members) {
            if (member.offset != parameter.byteOffset)
                continue;
            if (parameter.arrayCount == 1)
                return member.dataType == type;
            MTLArrayType *array = member.dataType == MTLDataTypeArray ? member.arrayType : nil;
            return MatchesMetalUniformArray(array, parameter, type);
        }
        return false;
    }
}  // namespace Horo::Render::Detail
