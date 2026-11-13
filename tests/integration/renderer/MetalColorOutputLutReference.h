#pragma once

/** @file MetalColorOutputLutReference.h
 * @brief Portable independent OCIO CPU observations; never a runtime interpolation implementation or native verdict.
 */
#include "Horo/Runtime/Render/ColorPipeline.h"

#include <array>
#include <string_view>

namespace Horo::Render::Test::LutOracle {
    /** @brief Original independent reference identity, pinned to the external generation receipt. */
    inline constexpr std::string_view ReferenceDigest = "sha256:b2d2b910f6adc3e63da20227b5dc47c68b9074e5df0df4334cbd821cd426ebb9";
    inline constexpr std::string_view RequestDigest = "sha256:5836171e2e2facfa886dc62d8ac5ae165cf5f2c460c363e1ef907a0c758f9127";
    inline constexpr Math::Vec3 DomainMinimum{-1, -1, -1}, DomainMaximum{3, 3, 3};
    /** @brief Exact nonidentity cube vertices in the public red-fastest representation. */
    inline constexpr std::array<float, 24> CubeRgb{0.015625F, 0.03125F, 0.0625F, 2.0F, 0.125F, 0.25F, 0.25F, 1.0F,
                                                   0.0625F,   0.5F,     0.25F,   2.0F, 0.125F, 0.5F,  1.0F,  1.0F,
                                                   0.0625F,   0.5F,     0.0625F, 2.0F, 0.125F, 4.0F,  1.0F,  2.0F};

    /** @brief One real OCIO LUT3DTransform then pinned analytic output observation at an exact FP16 input. */
    struct Case final {
        std::string_view name;
        ColorLutInterpolation interpolation;
        Math::Vec3 input;
        std::array<float, 3> displayLinear;
    };

    // Generated independently by OCIO CPU OPTIMIZATION_NONE, explicit setValue(r,g,b), then the pinned processor.
    // Raw evidence: 394-lut-oracle-c34-reference.json and 394-lut-oracle-c34-receipt.json. No local toolchain path is retained.
    inline constexpr std::array<Case, 20> Cases{{
        {"min",
         ColorLutInterpolation::Trilinear,
         {-1.0F, -1.0F, -1.0F},
         {0.0002889600582420826F, 0.0074230404570698738F, 0.015450194478034973F}},
        {"max", ColorLutInterpolation::Trilinear, {3.0F, 3.0F, 3.0F}, {0.99999988079071045F, 0.46572071313858032F, 0.62974178791046143F}},
        {"corner-green",
         ColorLutInterpolation::Trilinear,
         {-1.0F, 3.0F, -1.0F},
         {0.010918110609054565F, 0.53031986951828003F, 0.03181023895740509F}},
        {"corner-magenta",
         ColorLutInterpolation::Trilinear,
         {3.0F, -1.0F, 3.0F},
         {0.79931658506393433F, 0.0049188961274921894F, 0.28880110383033752F}},
        {"unequal-xyz",
         ColorLutInterpolation::Trilinear,
         {-0.5F, 0.5F, 2.0F},
         {0.072191700339317322F, 0.44661292433738708F, 0.30570587515830994F}},
        {"unequal-zxy",
         ColorLutInterpolation::Trilinear,
         {2.0F, -0.5F, 0.5F},
         {0.93904358148574829F, 0.079821117222309113F, 0.28738892078399658F}},
        {"tie-xy",
         ColorLutInterpolation::Trilinear,
         {1.0F, 1.0F, 0.0F},
         {0.48787164688110352F, 0.27058413624763489F, 0.35575556755065918F}},
        {"tie-xz",
         ColorLutInterpolation::Trilinear,
         {1.0F, 0.0F, 1.0F},
         {0.55936884880065918F, 0.22190894186496735F, 0.33037608861923218F}},
        {"tie-yz",
         ColorLutInterpolation::Trilinear,
         {0.0F, 1.0F, 1.0F},
         {0.27452513575553894F, 0.38604974746704102F, 0.29653027653694153F}},
        {"tie-xyz",
         ColorLutInterpolation::Trilinear,
         {1.0F, 1.0F, 1.0F},
         {0.51724100112915039F, 0.33251085877418518F, 0.38401859998703003F}},
        {"min",
         ColorLutInterpolation::Tetrahedral,
         {-1.0F, -1.0F, -1.0F},
         {0.0002889600582420826F, 0.0074230404570698738F, 0.015450194478034973F}},
        {"max", ColorLutInterpolation::Tetrahedral, {3.0F, 3.0F, 3.0F}, {0.99999988079071045F, 0.46572071313858032F, 0.62974178791046143F}},
        {"corner-green",
         ColorLutInterpolation::Tetrahedral,
         {-1.0F, 3.0F, -1.0F},
         {0.010918110609054565F, 0.53031986951828003F, 0.03181023895740509F}},
        {"corner-magenta",
         ColorLutInterpolation::Tetrahedral,
         {3.0F, -1.0F, 3.0F},
         {0.79931658506393433F, 0.0049188961274921894F, 0.28880110383033752F}},
        {"unequal-xyz",
         ColorLutInterpolation::Tetrahedral,
         {-0.5F, 0.5F, 2.0F},
         {0.26737931370735168F, 0.41274344921112061F, 0.34944227337837219F}},
        {"unequal-zxy",
         ColorLutInterpolation::Tetrahedral,
         {2.0F, -0.5F, 0.5F},
         {0.99999988079071045F, 0.086187325417995453F, 0.26890015602111816F}},
        {"tie-xy",
         ColorLutInterpolation::Tetrahedral,
         {1.0F, 1.0F, 0.0F},
         {0.74825477600097656F, 0.16507638990879059F, 0.52553915977478027F}},
        {"tie-xz",
         ColorLutInterpolation::Tetrahedral,
         {1.0F, 0.0F, 1.0F},
         {0.88742077350616455F, 0.12963059544563293F, 0.3600810170173645F}},
        {"tie-yz",
         ColorLutInterpolation::Tetrahedral,
         {0.0F, 1.0F, 1.0F},
         {0.51261258125305176F, 0.38271710276603699F, 0.32211011648178101F}},
        {"tie-xyz",
         ColorLutInterpolation::Tetrahedral,
         {1.0F, 1.0F, 1.0F},
         {0.99999988079071045F, 0.26646393537521362F, 0.47228845953941345F}},
    }};

    /** @brief Adjacent representable FP16 samples outside the declared domain; no clamped CPU output is invented. */
    inline constexpr std::array<Math::Vec3, 2> Outside{{{-1.0009765625F, 1.0F, 1.0F}, {3.001953125F, 1.0F, 1.0F}}};
}  // namespace Horo::Render::Test::LutOracle
