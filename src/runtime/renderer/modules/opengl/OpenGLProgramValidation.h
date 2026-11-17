#pragma once
/** @file OpenGLProgramValidation.h @brief Non-installed selected-OpenGL candidate validation seam. */
#include "Horo/Runtime/Render/ShaderReflection.h"

namespace Horo::Render::Detail {
    /** @brief Checks exact final-driver attributes, outputs, uniform packing and sampler units on an unpublished program.
     * @param program Owner-context native candidate. @param manifest Validated canonical logical interface.
     * @param target Exact selected target. @param reflection Same-boundary admitted cooked reflection.
     * @return Original driver/interface failure or complete validation; no frontend identities are fabricated. */
    [[nodiscard]] Result<void> ValidateOpenGLProgramInterface(std::uint32_t program, const ShaderManifest &manifest,
                                                              const ShaderTargetRequirement &target,
                                                              const NormalizedShaderReflection &reflection);

}  // namespace Horo::Render::Detail
