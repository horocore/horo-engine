#include "Horo/Runtime/Render/SceneColorEncoding.h"

#include "RenderErrorDescriptor.h"

#include <array>
#include <cmath>
#include <limits>

namespace Horo::Render {
    namespace {
        // OpenColorIO2.5.0 CGconfig4.0.0/ACES2, LinearRec709D65→ACEScg CPU basis vectors, version1 ingress.
        constexpr std::array<std::array<float, 3>, 3> Rec709ToAcesCg{{{0.6130974292755127F, 0.3395231366157532F, 0.04737945273518562F},
                                                                      {0.07019372284412384F, 0.9163538813591003F, 0.013452398590743542F},
                                                                      {0.0206155925989151F, 0.10956977307796478F, 0.8698146343231201F}}};

        /** @brief Decodes the declared sRGB EOTF, preserving its linear extension below zero. */
        double DecodeSrgb(double value) noexcept {
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        }

        /** @brief Computes a pinned AP1 basis conversion without float32 intermediate overflow. */
        std::array<double, 3> ConvertBasis(const std::array<double, 3> &source) noexcept {
            std::array<double, 3> result{};
            for (std::size_t row = 0; row < result.size(); ++row)
                for (std::size_t column = 0; column < source.size(); ++column)
                    result[row] += static_cast<double>(Rec709ToAcesCg[row][column]) * source[column];
            return result;
        }

        /** @brief Rejects unrepresentable results without clamping signed working RGB. */
        bool Representable(const std::array<double, 3> &values) noexcept {
            for (double value : values)
                if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                    return false;
            return true;
        }
    }  // namespace

    /** @copydoc ValidateSceneColorSourcePolicy */
    Result<void> ValidateSceneColorSourcePolicy(SceneColorSourcePolicy policy) {
        if (policy.version != 1 || policy.encoding == SceneColorEncoding::Unspecified ||
            static_cast<unsigned>(policy.encoding) > static_cast<unsigned>(SceneColorEncoding::SrgbRec709D65))
            return Result<void>::Failure(MakeError(SceneColorErrors::InvalidPolicy));
        return Result<void>::Success();
    }

    /** @copydoc ConvertSceneColorToAcesCg */
    Result<Math::Vec3> ConvertSceneColorToAcesCg(Math::Vec3 source, SceneColorSourcePolicy policy) {
        using Return = Result<Math::Vec3>;
        if (auto valid = ValidateSceneColorSourcePolicy(policy); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        if (!Math::IsFinite(source))
            return Return::Failure(MakeError(SceneColorErrors::NonFiniteInput));
        if (policy.encoding == SceneColorEncoding::LinearAcesCg)
            return Return::Success(source);
        std::array<double, 3> values{source.x, source.y, source.z};
        if (policy.encoding == SceneColorEncoding::SrgbRec709D65)
            for (double &value : values)
                value = DecodeSrgb(value);
        const auto converted = ConvertBasis(values);
        if (!Representable(converted))
            return Return::Failure(MakeError(SceneColorErrors::OutOfRange));
        return Return::Success({static_cast<float>(converted[0]), static_cast<float>(converted[1]), static_cast<float>(converted[2])});
    }

    namespace SceneColorErrors {
        namespace {
            const ErrorDomainId Domain{"render.scene_color"};
        }

        const ErrorCodeDescriptor InvalidPolicy =
            Detail::MakeErrorDescriptor(Domain, "render.scene_color.policy_invalid", ErrorSeverity::Error,
                                        "Scene source color policy is missing or unsupported.",
                                        "Select an explicit supported versioned source interpretation before scene lighting.");
        const ErrorCodeDescriptor NonFiniteInput =
            Detail::MakeErrorDescriptor(Domain, "render.scene_color.input_nonfinite", ErrorSeverity::Error,
                                        "Scene source color contains NaN or infinity.",
                                        "Supply finite source RGB; finite negative working components remain supported.");
        const ErrorCodeDescriptor OutOfRange =
            Detail::MakeErrorDescriptor(Domain, "render.scene_color.output_out_of_range", ErrorSeverity::Error,
                                        "Scene source conversion exceeds finite float32 range.",
                                        "Supply representable source RGB; conversion never silently clamps or changes source policy.");
    }  // namespace SceneColorErrors
}  // namespace Horo::Render
