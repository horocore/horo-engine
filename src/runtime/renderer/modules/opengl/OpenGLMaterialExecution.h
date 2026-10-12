#pragma once

#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/RenderShaderResources.h"

namespace Horo::Render::Detail {
    /** @brief Bounded selected-context facts captured during owner preparation, never rediscovered by a frame-hot draw. */
    struct OpenGLMaterialContextFeatures {
        std::int32_t drawBuffers{0};
        std::int32_t clipDistances{0};
        bool fixedIndexRestart{false};

        [[nodiscard]] bool IsValid() const noexcept {
            return drawBuffers > 0 && drawBuffers <= 64 && clipDistances >= 0 && clipDistances <= 64;
        }

        [[nodiscard]] bool operator==(const OpenGLMaterialContextFeatures &) const noexcept = default;
    };

    /** @brief Fixed native binding recipe; exact generation pins and backing remain frontend-owned. */
    struct OpenGLMaterialSlot {
        MaterialResidentResourceKind kind{MaterialResidentResourceKind::Buffer};
        std::uint32_t binding{0};
        std::uint32_t object{0};
        std::uint32_t sampler{0};
        std::size_t offset{0};
        std::size_t byteCount{0};
    };

    /** @brief Synchronous immutable draw recipe over prevalidated program, mesh and binding instances. */
    struct OpenGLMaterialDrawCommand {
        std::uint32_t program{0};
        std::uint32_t vertexArray{0};
        std::uint32_t indexCount{0};
        RenderIndexFormat indexFormat{RenderIndexFormat::UInt32};
        RenderGraphicsPipelineState state;
        OpenGLMaterialContextFeatures features;
        std::span<const OpenGLMaterialSlot> slots;
    };

    /** @brief Selected-context native consumer dispatch, owning no borrowed request after the call. */
    struct OpenGLMaterialExecutionFunctions {
        bool (*isAvailable)() noexcept {nullptr};
        Result<OpenGLMaterialContextFeatures> (*queryFeatures)(){nullptr};
        Result<void> (*execute)(const RenderMaterialExecutionRequest &, std::span<const OpenGLMaterialDrawCommand>){nullptr};

        [[nodiscard]] bool IsValid() const noexcept {
            return isAvailable && queryFeatures && execute;
        }
    };

    /** @brief Returns complete GL4.1 draw/state-isolation dispatch. @return Private synchronous functions. */
    [[nodiscard]] OpenGLMaterialExecutionFunctions ProductionOpenGLMaterialExecutionFunctions() noexcept;
}  // namespace Horo::Render::Detail
