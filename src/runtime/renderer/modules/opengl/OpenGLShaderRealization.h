#pragma once

#include "Horo/Runtime/Render/RenderResourceDescriptors.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"

#include <cstdint>
#include <span>

namespace Horo::Render::Detail {
    /** @brief Owner-context sampler commands; inputs are borrowed synchronously and callbacks cannot throw. */
    struct OpenGLSamplerFunctions {
        bool (*isAvailable)() noexcept {nullptr};
        void (*generate)(std::uint32_t &) noexcept {nullptr};
        void (*destroy)(std::uint32_t) noexcept {nullptr};
        void (*parameterInteger)(std::uint32_t, std::uint32_t, std::int32_t) noexcept {nullptr};
        void (*parameterFloat)(std::uint32_t, std::uint32_t, float) noexcept {nullptr};
        std::uint32_t (*error)() noexcept {nullptr};

        /** @brief Checks complete dispatch before invoking a native callback. @return True for complete callbacks. */
        [[nodiscard]] bool IsValid() const noexcept {
            return isAvailable && generate && destroy && parameterInteger && parameterFloat && error;
        }
    };

    /** @brief Realizes exact core sampler policy without changing a texture-unit binding.
     * @param descriptor Admitted finite policy; anisotropy above one is explicitly unsupported.
     * @param functions Owner-context sampler dispatch.
     * @return Owned unpublished sampler or a typed policy/native failure; candidate cleanup survives diagnostic allocation failure. */
    [[nodiscard]] Result<std::uint32_t> CreateOpenGLSampler(const RenderSamplerDescriptor &descriptor,
                                                            const OpenGLSamplerFunctions &functions);
    /** @brief Supplies dynamic GLAD wrappers after owner-context loading. @return Nonthrowing sampler dispatch. */
    [[nodiscard]] OpenGLSamplerFunctions ProductionOpenGLSamplerFunctions() noexcept;

    /** @brief Context-owner native shader dispatch; every callback is nonthrowing and synchronously borrows its inputs. */
    struct OpenGLShaderFunctions {
        bool (*isAvailable)() noexcept {nullptr};
        std::uint32_t (*createShader)(std::uint32_t) noexcept {nullptr};
        void (*shaderSource)(std::uint32_t, std::span<const std::uint8_t>) noexcept {nullptr};
        void (*compileShader)(std::uint32_t) noexcept {nullptr};
        void (*shaderParameter)(std::uint32_t, std::uint32_t, std::int32_t &) noexcept {nullptr};
        std::size_t (*shaderLog)(std::uint32_t, std::span<char>) noexcept {nullptr};
        void (*deleteShader)(std::uint32_t) noexcept {nullptr};
        std::uint32_t (*createProgram)() noexcept {nullptr};
        void (*attachShader)(std::uint32_t, std::uint32_t) noexcept {nullptr};
        void (*linkProgram)(std::uint32_t) noexcept {nullptr};
        void (*programParameter)(std::uint32_t, std::uint32_t, std::int32_t &) noexcept {nullptr};
        std::size_t (*programLog)(std::uint32_t, std::span<char>) noexcept {nullptr};
        void (*deleteProgram)(std::uint32_t) noexcept {nullptr};
        std::uint32_t (*error)() noexcept {nullptr};

        /** @brief Reports complete readiness without calling unavailable native entry points. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Compiles one cooked vertex/fragment stage at a context-owner preparation safe point.
     * @param stage Borrowed selected stage from an admitted Glsl410 artifact, valid only during this call.
     * @param functions Complete current-context dispatch.
     * @return Native shader identity transferred to the caller, or a typed unsupported/compile/driver failure.
     * @details The caller admits memory first and owns publication/destruction. Failure releases the candidate even if
     * owned diagnostic allocation throws. No source cooking, backend fallback, waiting or retained input occurs here.
     */
    [[nodiscard]] Result<std::uint32_t> CompileOpenGLCookedStage(const ShaderArtifactStageView &stage,
                                                                 const OpenGLShaderFunctions &functions);

    /**
     * @brief Links two admitted native stages into a candidate graphics program during preparation.
     * @param vertex Exact live vertex shader instance.
     * @param fragment Exact live fragment shader instance.
     * @param functions Complete current-context dispatch.
     * @return Candidate native program, or a typed link/driver failure with bounded original diagnostics.
     * @details Caller owns final linked-reflection validation, publication and deletion. Failure deletes only the candidate
     * program; shader ownership remains with the frontend registry. The operation never binds or replaces the active program.
     */
    [[nodiscard]] Result<std::uint32_t> LinkOpenGLCookedGraphicsProgram(std::uint32_t vertex, std::uint32_t fragment,
                                                                        const OpenGLShaderFunctions &functions);

    /** @brief Returns dispatch wrappers for the owning context's loaded GLAD entry points. */
    [[nodiscard]] OpenGLShaderFunctions ProductionOpenGLShaderFunctions() noexcept;
}  // namespace Horo::Render::Detail
