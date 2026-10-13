#pragma once

#include "Horo/Runtime/Render/RenderShaderResources.h"
#include "OpenGLShaderRealization.h"

namespace Horo::Render::Detail {
    /** @brief Maps an admitted reflected scalar/vector/matrix shape to its exact private native GL4.1 type.
     * @param type Portable scalar kind. @param rows Row count in the bounded 1..4 envelope.
     * @param columns Column count in the bounded 1..4 envelope.
     * @return Native type or zero for invalid/unsupported shapes. Vectors admit either single-row or single-column representation.
     * @details Pure owner-private reflection mapping; no context access or native mutation. Matrix orientation is preserved. */
    [[nodiscard]] std::uint32_t OpenGLReflectedValueType(ShaderValueType type, std::uint8_t rows, std::uint8_t columns) noexcept;

    /** @brief Bounded material admission queries over an already validated immutable program; none change binding state. */
    struct OpenGLMaterialFunctions {
        Result<std::size_t> (*uniformBlockSize)(std::uint32_t, const std::string &){nullptr};
        Result<bool> (*textureComparison)(std::uint32_t, const std::string &){nullptr};
        Result<std::size_t> (*uniformOffsetAlignment)(){nullptr};

        [[nodiscard]] bool IsValid() const noexcept {
            return uniformBlockSize && textureComparison && uniformOffsetAlignment;
        }
    };

    /** @brief Private final-program preparation dispatch; validation may allocate an owned typed diagnostic. */
    struct OpenGLPipelineFunctions {
        bool (*isAvailable)() noexcept {nullptr};
        Result<void> (*validateAndMap)(std::uint32_t, const RenderGraphicsPipelineDescriptor &){nullptr};
        /** Selected-context facts, not adapter discovery; may allocate owned identity/error metadata. */
        Result<RenderPipelineDeviceIdentity> (*queryDeviceIdentity)(){nullptr};

        /** @brief Reports complete dispatch without invoking a native command. @return True for complete dispatch. */
        [[nodiscard]] bool IsValid() const noexcept {
            return isAvailable && validateAndMap;
        }
    };

    /** @brief Links and checks the actual final program before transferring publication ownership.
     * @param vertex Exact admitted vertex instance.
     * @param fragment Exact admitted fragment instance.
     * @param descriptor Bounded, normalized cooked reflection and immutable state.
     * @param shaders Current owner-context compile/link dispatch.
     * @param pipelines Current owner-context final-program validation dispatch.
     * @return Owned unpublished program or a typed failure; exceptions also destroy only the candidate.
     * @details No current program, texture, buffer, sampler or fixed rendering state is changed. Mapping only mutates
     * the unpublished candidate. The frontend must admit memory and pin module generations before calling. */
    [[nodiscard]] Result<std::uint32_t> CreateOpenGLGraphicsProgram(std::uint32_t vertex, std::uint32_t fragment,
                                                                    const RenderGraphicsPipelineDescriptor &descriptor,
                                                                    const OpenGLShaderFunctions &shaders,
                                                                    const OpenGLPipelineFunctions &pipelines);

    /** @brief Returns GL4.1 final-program reflection wrappers. @return Private owner-context preparation dispatch. */
    [[nodiscard]] OpenGLPipelineFunctions ProductionOpenGLPipelineFunctions() noexcept;
    /** @brief Returns selected-context material admission queries. @return Private GL4.1 dispatch. */
    [[nodiscard]] OpenGLMaterialFunctions ProductionOpenGLMaterialFunctions() noexcept;
}  // namespace Horo::Render::Detail
