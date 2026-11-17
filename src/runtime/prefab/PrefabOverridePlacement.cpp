#include "PrefabValueOverridesInternal.h"

#include <algorithm>
#include <cmath>

namespace Horo::Prefab {
    namespace {
        /** @brief Allows only finite engine-precision roundoff, never the shear
         * discarded by Gram–Schmidt decomposition. */
        bool SameAffineComponent(float expected, float actual) noexcept {
            if (!std::isfinite(expected) || !std::isfinite(actual))
                return false;
            const double scale = std::max(std::fabs(static_cast<double>(expected)), std::fabs(static_cast<double>(actual)));
            const double tolerance = static_cast<double>(Math::DefaultEpsilon) * (1.0 + scale);
            return std::fabs(static_cast<double>(expected) - static_cast<double>(actual)) <= tolerance;
        }

        /** @brief Checks all sixteen original affine coefficients against the
         * reconstructed, representable TRS. */
        bool SameAffineMatrix(const Math::Mat4 &expected, const Math::Mat4 &actual) noexcept {
            for (std::size_t i = 0; i < expected.values.size(); ++i)
                if (!SameAffineComponent(expected.values[i], actual.values[i]))
                    return false;
            return true;
        }

    }  // namespace

    /** @copydoc ValueDetail::ComposePrefabPlacement */
    Result<Math::Transform> ValueDetail::ComposePrefabPlacement(const Math::Transform &placement, const Math::Transform &local) {
        const auto parent = placement.TryToMatrix();
        if (parent.HasError())
            return Result<Math::Transform>::Failure(parent.ErrorValue());
        const auto child = local.TryToMatrix();
        if (child.HasError())
            return Result<Math::Transform>::Failure(child.ErrorValue());
        const auto matrix = Math::Multiply(parent.Value(), child.Value());
        const auto composed = Math::TryDecomposeAffineTRS(matrix);
        if (composed.HasError())
            return composed;
        const auto reconstructed = composed.Value().TryToMatrix();
        if (reconstructed.HasError())
            return Result<Math::Transform>::Failure(reconstructed.ErrorValue());
        if (!SameAffineMatrix(matrix, reconstructed.Value()))
            return Result<Math::Transform>::Failure(MakeError(PrefabErrors::InvalidPlacement,
                                                              "Nested placement cannot represent the final affine "
                                                              "transform without shear."));
        return composed;
    }
}  // namespace Horo::Prefab
