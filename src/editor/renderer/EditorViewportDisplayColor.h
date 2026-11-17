#pragma once
/** @file EditorViewportDisplayColor.h @brief Explicit legacy editor UI sRGB color interpretation into display-linear Rec709. */
#include "EditorRendererErrors.h"
#include "Horo/Math/SceneMath.h"

#include <cmath>

namespace Horo::Editor {
    /** @brief Projects canonical light chroma into display-linear Rec709 for editor geometry, without scene exposure or ACES tone mapping.
     * @param ap1 Explicit canonical light color. @return Finite signed display-linear color or typed failure.
     * @details Matrix is the inverse of the pinned CG4/OCIO2.5 Rec709-to-AP1 ingress matrix; the visualizer owns subsequent UI contrast
     * clamps. */
    [[nodiscard]] inline Result<Math::Vec3> LightOverlayDisplayLinearV1(Math::Vec3 ap1) {
        if (!Math::IsFinite(ap1))
            return Result<Math::Vec3>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        const Math::Vec3 display{1.705050913F * ap1.x - 0.621792072F * ap1.y - 0.083258871F * ap1.z,
                                 -0.130256412F * ap1.x + 1.140804730F * ap1.y - 0.010548319F * ap1.z,
                                 -0.024003355F * ap1.x - 0.128968977F * ap1.y + 1.152972333F * ap1.z};
        if (!Math::IsFinite(display))
            return Result<Math::Vec3>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        return Result<Math::Vec3>::Success(display);
    }

    /** @brief Decodes version-one authored editor UI color exactly once, preserving finite signed values without ACES/exposure.
     * @param encoded Explicit legacy sRGB/Rec709 UI color. @return Finite display-linear Rec709/D65 value or typed invalid input. */
    [[nodiscard]] inline Result<Math::Vec3> DecodeEditorOverlaySrgbV1(Math::Vec3 encoded) {
        if (!Math::IsFinite(encoded))
            return Result<Math::Vec3>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        const auto channel = [](float value) {
            return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
        };
        const Math::Vec3 decoded{channel(encoded.x), channel(encoded.y), channel(encoded.z)};
        if (!Math::IsFinite(decoded))
            return Result<Math::Vec3>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        return Result<Math::Vec3>::Success(decoded);
    }
}  // namespace Horo::Editor
