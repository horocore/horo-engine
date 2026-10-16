#include "Horo/Physics/PhysicsMaterialAsset.h"

#include "Horo/Physics/PhysicsBodyDescriptor.h"

#include <algorithm>
#include <bit>
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

    namespace {
        static_assert(PhysicsMaterialAssetSchemaVersion == 1);
        constexpr std::array MaterialHeader{std::byte{'H'}, std::byte{'O'}, std::byte{'M'}, std::byte{'A'}, std::byte{'T'}, std::byte{'E'},
                                            std::byte{'R'}, std::byte{'L'}, std::byte{1},   std::byte{0},   std::byte{0},   std::byte{0},
                                            std::byte{1},   std::byte{0},   std::byte{0},   std::byte{0}};

        /** @brief Writes a fixed-width unsigned field independent of native endianness and padding. */
        void MaterialWord(PhysicsMaterialPayload &bytes, const std::size_t offset, const std::uint64_t value, const std::size_t width) {
            for (std::size_t index = 0; index < width; ++index)
                bytes[offset + index] = static_cast<std::byte>(value >> (8U * index));
        }

        /** @brief Reads a fixed-width unsigned field after the complete payload size was checked. */
        std::uint64_t MaterialWord(const std::span<const std::byte> bytes, const std::size_t offset, const std::size_t width) {
            std::uint64_t value{};
            for (std::size_t index = 0; index < width; ++index)
                value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (8U * index);
            return value;
        }
    }  // namespace

    /** @copydoc EncodePhysicsMaterialAsset */
    Result<PhysicsMaterialPayload> EncodePhysicsMaterialAsset(const PhysicsMaterialAsset &material) {
        if (!material.IsValid())
            return Result<PhysicsMaterialPayload>::Failure(MakeError(PhysicsErrors::MaterialDescriptorInvalid));
        PhysicsMaterialPayload bytes{};
        std::ranges::copy(MaterialHeader, bytes.begin());
        const auto &descriptor = material.Descriptor();
        const auto &identity = descriptor.id.Asset().Bytes();
        for (std::size_t index = 0; index < identity.size(); ++index)
            bytes[16 + index] = static_cast<std::byte>(identity[index]);
        MaterialWord(bytes, 32, descriptor.sourceRevision, 8);
        MaterialWord(bytes, 40, std::bit_cast<std::uint32_t>(descriptor.friction == 0 ? 0.0F : descriptor.friction), 4);
        MaterialWord(bytes, 44, std::bit_cast<std::uint32_t>(descriptor.restitution == 0 ? 0.0F : descriptor.restitution), 4);
        MaterialWord(bytes, 48, std::bit_cast<std::uint32_t>(descriptor.densityKilogramsPerCubicMeter), 4);
        bytes[52] = static_cast<std::byte>(descriptor.frictionCombine);
        bytes[53] = static_cast<std::byte>(descriptor.restitutionCombine);
        return Result<PhysicsMaterialPayload>::Success(bytes);
    }

    /** @copydoc DecodePhysicsMaterialAsset */
    Result<PhysicsMaterialAsset> DecodePhysicsMaterialAsset(const std::span<const std::byte> payload) {
        if (payload.size() != PhysicsMaterialPayloadSize || !std::ranges::equal(payload.first(MaterialHeader.size()), MaterialHeader))
            return Result<PhysicsMaterialAsset>::Failure(
                MakeError(PhysicsErrors::MaterialDescriptorInvalid,
                          "Physics material payload requires the exact canonical schema-1 header and fixed size."));
        std::array<std::uint8_t, 16> identity{};
        for (std::size_t index = 0; index < identity.size(); ++index)
            identity[index] = std::to_integer<std::uint8_t>(payload[16 + index]);
        PhysicsMaterialAssetDescriptor
            descriptor{.id = PhysicsMaterialAssetId::FromAssetId(Assets::AssetId::FromBytes(identity)),
                       .sourceRevision = MaterialWord(payload, 32, 8),
                       .friction = std::bit_cast<float>(static_cast<std::uint32_t>(MaterialWord(payload, 40, 4))),
                       .restitution = std::bit_cast<float>(static_cast<std::uint32_t>(MaterialWord(payload, 44, 4))),
                       .densityKilogramsPerCubicMeter = std::bit_cast<float>(static_cast<std::uint32_t>(MaterialWord(payload, 48, 4))),
                       .frictionCombine = static_cast<PhysicsMaterialCombineMode>(std::to_integer<std::uint8_t>(payload[52])),
                       .restitutionCombine = static_cast<PhysicsMaterialCombineMode>(std::to_integer<std::uint8_t>(payload[53]))};
        auto material = PhysicsMaterialAsset::Create(descriptor);
        if (material.HasError())
            return material;
        const auto canonical = EncodePhysicsMaterialAsset(material.Value());
        if (canonical.HasError() || !std::ranges::equal(canonical.Value(), payload))
            return Result<PhysicsMaterialAsset>::Failure(
                MakeError(PhysicsErrors::MaterialDescriptorInvalid,
                          "Physics material payload contains reserved or noncanonical field bytes."));
        return material;
    }
}  // namespace Horo::Physics
