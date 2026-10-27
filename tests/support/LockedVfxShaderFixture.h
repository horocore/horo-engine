/** @file
 * @brief Bounded authentic locked-tool text evidence shared by VFX cook and native smoke tests.
 */
#pragma once

#include "ShaderReflectionCook.h"

#include <array>

namespace Horo::Tests {
    /** @brief Actual same-operation reflection/native text; Metal text is not a compiled library. */
    struct LockedVfxShaderEvidence {
        Render::ShaderTargetRequirement target;
        Render::ShaderManifest manifest;
        std::array<Render::SpirvReflectionStage, 2> stages;
        std::array<std::string, 2> translations;
    };

    /**
     * @brief Loads the fixed, hash-checked captured VFX text fixture without invoking tools.
     * @param backend OpenGL or Metal text evidence to load.
     * @param masked Selects the actual masked fragment translation instead of the optimized ordinary fragment.
     * @return Owned evidence from the exact frozen shader and locked profile recorded beside the files.
     */
    LockedVfxShaderEvidence LoadLockedVfxShaderEvidence(Render::ShaderTargetBackend backend, bool masked);

    /**
     * @brief Packages actual captured GLSL through the existing reflection/cook authority for native fixture admission.
     * @param evidence Exact OpenGL fixture evidence; no authored native layout is supplied.
     * @return A bounded two-stage GLSL package with validated final-target reflection.
     */
    Render::CompiledShaderArtifact CookLockedVfxOpenGlFixture(const LockedVfxShaderEvidence &evidence);
}  // namespace Horo::Tests
