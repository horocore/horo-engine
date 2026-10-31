/** @file @brief Existing SDL/Core smoke host shared by material and cooked VFX qualification cases. */
#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"

namespace Horo::Render::OpenGLMaterialSmoke {
    /** @brief Distinguishes hardware evidence from software-capable contract coverage. */
    enum class MaterialEvidence {
        HardwareQualification,
        SoftwareCapableCoverage
    };

    /**
     * @brief Runs a bounded test using the existing real SDL/OpenGL frontend composition.
     * @param evidence Required evidence lane; hardware rejects known software identities.
     * @param verify Synchronous test body, borrowing the frontend only for this call.
     */
    void WithNativeFrontend(MaterialEvidence evidence, void (*verify)(RenderFrontend &));
}  // namespace Horo::Render::OpenGLMaterialSmoke
