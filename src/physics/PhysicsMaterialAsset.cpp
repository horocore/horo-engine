#include "Horo/Physics/PhysicsMaterialAsset.h"

#include "Horo/Physics/PhysicsBodyDescriptor.h"

#include <cmath>
#include <utility>

namespace Horo::Physics {
    namespace {
        /** @brief Checks whether a persisted combine-mode representation is in the qualified vocabulary. */
        [[nodiscard]] bool IsSupportedCombineMode(const PhysicsMaterialCombineMode mode) noexcept {
            using enum PhysicsMaterialCombineMode;
            switch (mode) {
                case Average:
                case Minimum:
                case Multiply:
                case Maximum:
                    return true;
            }
            return false;
        }

        /** @brief Creates a stable descriptor error with operation-specific context. */
        [[nodiscard]] Error InvalidMaterial(std::string message) {
            return MakeError(PhysicsErrors::MaterialDescriptorInvalid, std::move(message));
        }

        /** @brief Validates one finite scalar against its closed authored range. */
        [[nodiscard]] Result<void> ValidateMaterialScalar(const float value, const float minimum, const float maximum,
                                                          std::string message) {
            if (!std::isfinite(value) || value < minimum || value > maximum)
                return Result<void>::Failure(InvalidMaterial(std::move(message)));
            return Result<void>::Success();
        }

        /** @brief Validates the persistent identity and exact schema metadata. */
        [[nodiscard]] Result<void> ValidateMaterialMetadata(const PhysicsMaterialAssetDescriptor &descriptor) {
            if (descriptor.schemaVersion != PhysicsMaterialAssetSchemaVersion)
                return Result<void>::Failure(InvalidMaterial("The Physics material asset schema version is unsupported."));
            if (!descriptor.id.IsValid())
                return Result<void>::Failure(InvalidMaterial("The Physics material asset requires a non-zero stable asset identity."));
            if (descriptor.sourceRevision == 0)
                return Result<void>::Failure(InvalidMaterial("The Physics material asset source revision must be non-zero."));
            return Result<void>::Success();
        }

        /** @brief Selects the deterministic higher-precedence policy for two valid mode values. */
        [[nodiscard]] PhysicsMaterialCombineMode SelectCombineMode(const PhysicsMaterialCombineMode left,
                                                                   const PhysicsMaterialCombineMode right) noexcept {
            return static_cast<std::uint8_t>(left) >= static_cast<std::uint8_t>(right) ? left : right;
        }

        /** @brief Applies one validated combine policy to two finite scalar coefficients. */
        [[nodiscard]] float CombineScalar(const float left, const float right, const PhysicsMaterialCombineMode mode) noexcept {
            using enum PhysicsMaterialCombineMode;
            switch (mode) {
                case Average:
                    return (left + right) * 0.5F;
                case Minimum:
                    return std::fmin(left, right);
                case Multiply:
                    return left * right;
                case Maximum:
                    return std::fmax(left, right);
            }
            return 0.0F;
        }
    }  // namespace

    /** @copydoc PhysicsMaterialAssetId::Parse */
    Result<PhysicsMaterialAssetId> PhysicsMaterialAssetId::Parse(const std::string_view value) {
        const auto parsed = Assets::AssetId::Parse(value);
        if (parsed.HasError() || !parsed.Value().IsValid())
            return Result<PhysicsMaterialAssetId>::Failure(
                InvalidMaterial("The Physics material asset identity must be a non-zero canonical asset UUID."));
        return Result<PhysicsMaterialAssetId>::Success(PhysicsMaterialAssetId::FromAssetId(parsed.Value()));
    }

    /** @copydoc ValidatePhysicsMaterialAssetDescriptor */
    Result<void> ValidatePhysicsMaterialAssetDescriptor(const PhysicsMaterialAssetDescriptor &descriptor) {
        if (const auto metadata = ValidateMaterialMetadata(descriptor); metadata.HasError())
            return metadata;
        if (const auto friction =
                ValidateMaterialScalar(descriptor.friction, MinimumPhysicsMaterialFriction, MaximumPhysicsMaterialFriction,
                                       "Physics material friction must be finite and within the CanonicalV1 bounds.");
            friction.HasError())
            return friction;
        if (const auto restitution =
                ValidateMaterialScalar(descriptor.restitution, MinimumPhysicsMaterialRestitution, MaximumPhysicsMaterialRestitution,
                                       "Physics material restitution must be finite and within the CanonicalV1 bounds.");
            restitution.HasError())
            return restitution;
        if (const auto density =
                ValidateMaterialScalar(descriptor.densityKilogramsPerCubicMeter, MinimumPhysicsDensity, MaximumPhysicsDensity,
                                       "Physics material density must be finite and within the CanonicalV1 SI bounds.");
            density.HasError())
            return density;
        if (!IsSupportedCombineMode(descriptor.frictionCombine) || !IsSupportedCombineMode(descriptor.restitutionCombine))
            return Result<void>::Failure(MakeError(PhysicsErrors::MaterialCombineUnsupported));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsMaterialAsset::Create */
    Result<PhysicsMaterialAsset> PhysicsMaterialAsset::Create(const PhysicsMaterialAssetDescriptor &descriptor) {
        if (const auto valid = ValidatePhysicsMaterialAssetDescriptor(descriptor); valid.HasError())
            return Result<PhysicsMaterialAsset>::Failure(valid.ErrorValue());
        return Result<PhysicsMaterialAsset>::Success(PhysicsMaterialAsset{descriptor});
    }

    /** @copydoc CombinePhysicsMaterialAssets */
    Result<PhysicsMaterialContactProperties> CombinePhysicsMaterialAssets(const PhysicsMaterialAsset &left,
                                                                          const PhysicsMaterialAsset &right) {
        if (!left.IsValid() || !right.IsValid())
            return Result<PhysicsMaterialContactProperties>::Failure(
                InvalidMaterial("Physics material combination requires two validated asset snapshots."));

        const auto frictionMode = SelectCombineMode(left.FrictionCombine(), right.FrictionCombine());
        const auto restitutionMode = SelectCombineMode(left.RestitutionCombine(), right.RestitutionCombine());
        return Result<PhysicsMaterialContactProperties>::Success(
            {.friction = CombineScalar(left.Friction(), right.Friction(), frictionMode),
             .restitution = CombineScalar(left.Restitution(), right.Restitution(), restitutionMode)});
    }
}  // namespace Horo::Physics
