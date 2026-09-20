#pragma once

/**
 * @file PhysicsMaterialAsset.h
 * @brief Immutable backend-neutral physical-material asset values and combine policy.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace Horo::Physics {
    /** @brief Version of the persisted physical-material asset descriptor contract. */
    inline constexpr std::uint32_t PhysicsMaterialAssetSchemaVersion = 1;

    /** @brief Maximum CanonicalV1 friction coefficient admitted by the backend-neutral contract. */
    inline constexpr float MaximumPhysicsMaterialFriction = 10.0F;

    /** @brief Minimum CanonicalV1 friction coefficient admitted by the backend-neutral contract. */
    inline constexpr float MinimumPhysicsMaterialFriction = 0.0F;

    /** @brief Minimum CanonicalV1 restitution coefficient. */
    inline constexpr float MinimumPhysicsMaterialRestitution = 0.0F;

    /** @brief Maximum CanonicalV1 restitution coefficient. */
    inline constexpr float MaximumPhysicsMaterialRestitution = 1.0F;

    /**
     * @brief Stable typed identity of one reusable Physics material asset.
     *
     * The wrapped identity is persistent asset metadata. It does not resolve an asset, retain a
     * registry snapshot, select a solver or own runtime material state.
     */
    class PhysicsMaterialAssetId final {
    public:
        /** @brief Constructs an invalid identity. */
        PhysicsMaterialAssetId() = default;

        /**
         * @brief Parses a canonical persistent asset identity.
         * @param value Canonical lowercase UUID text.
         * @return Typed identity or PhysicsErrors::MaterialDescriptorInvalid.
         */
        [[nodiscard]] static Result<PhysicsMaterialAssetId> Parse(std::string_view value);

        /**
         * @brief Wraps an existing persistent asset identity without resolving it.
         * @param asset Persistent asset identity.
         * @return Typed Physics material identity containing the supplied value.
         */
        [[nodiscard]] static constexpr PhysicsMaterialAssetId FromAssetId(const Assets::AssetId asset) noexcept {
            return PhysicsMaterialAssetId{asset};
        }

        /** @brief Checks representation only. @return True when the wrapped asset identity is non-zero. */
        [[nodiscard]] bool IsValid() const noexcept {
            return asset_.IsValid();
        }

        /** @brief Returns the wrapped persistent identity. @return Borrowed identity owned by this value. */
        [[nodiscard]] const Assets::AssetId &Asset() const noexcept {
            return asset_;
        }

        /** @brief Formats the wrapped identity. @return Canonical lowercase UUID text. */
        [[nodiscard]] std::string ToString() const {
            return asset_.ToString();
        }

        [[nodiscard]] constexpr auto operator<=>(const PhysicsMaterialAssetId &) const noexcept = default;

    private:
        explicit constexpr PhysicsMaterialAssetId(const Assets::AssetId asset) noexcept : asset_(asset) {}

        Assets::AssetId asset_{};
    };

    /** @brief Canonical combine policy applied independently to one contact scalar. */
    enum class PhysicsMaterialCombineMode : std::uint8_t {
        Average = 0,
        Minimum = 1,
        Multiply = 2,
        Maximum = 3,
    };

    /**
     * @brief Complete authored physical behavior for one reusable material asset.
     *
     * This descriptor intentionally contains no gameplay surface, acoustic, renderer or native
     * solver semantics. Those domains keep their own stable references and can address the same
     * asset independently.
     */
    struct PhysicsMaterialAssetDescriptor final {
        std::uint32_t schemaVersion{PhysicsMaterialAssetSchemaVersion}; /**< Exact persisted schema version. */
        PhysicsMaterialAssetId id;                                      /**< Stable persistent asset identity. */
        std::uint64_t sourceRevision{};                                 /**< Non-zero revision of this authored value. */
        float friction{0.5F};                                           /**< Dimensionless non-negative contact coefficient. */
        float restitution{};                                            /**< Dimensionless bounce coefficient in [0, 1]. */
        float densityKilogramsPerCubicMeter{1'000.0F};                  /**< Mass density in canonical SI units. */
        PhysicsMaterialCombineMode frictionCombine{PhysicsMaterialCombineMode::Average};    /**< Friction policy. */
        PhysicsMaterialCombineMode restitutionCombine{PhysicsMaterialCombineMode::Average}; /**< Restitution policy. */

        [[nodiscard]] constexpr auto operator<=>(const PhysicsMaterialAssetDescriptor &) const noexcept = default;
    };

    /** @brief Contact coefficients produced by combining two validated physical-material assets. */
    struct PhysicsMaterialContactProperties final {
        float friction{};    /**< Combined dimensionless friction coefficient. */
        float restitution{}; /**< Combined dimensionless restitution coefficient. */

        [[nodiscard]] constexpr auto operator<=>(const PhysicsMaterialContactProperties &) const noexcept = default;
    };

    /**
     * @brief Validates a physical-material descriptor without resolving assets or touching runtime state.
     * @param descriptor Owned authored material value.
     * @return Success, MaterialDescriptorInvalid for identity/schema/value failures, or
     * MaterialCombineUnsupported for an unknown combine-mode representation.
     * @pre Control/authoring use; no solver or world callback is active.
     * @post The descriptor remains unchanged and no asset or runtime lifetime is acquired.
     */
    [[nodiscard]] Result<void> ValidatePhysicsMaterialAssetDescriptor(const PhysicsMaterialAssetDescriptor &descriptor);

    /**
     * @brief Immutable value-owned physical-material asset snapshot.
     *
     * Copies own independent semantic data and may be inspected or destroyed on any thread. The
     * type has no shutdown phase, callback, job, native handle or ambient registry ownership.
     */
    class PhysicsMaterialAsset final {
    public:
        /** @brief Constructs an invalid empty value. */
        PhysicsMaterialAsset() = default;

        /**
         * @brief Validates and captures one immutable material asset value.
         * @param descriptor Authored value copied into the returned asset on success.
         * @return Owned immutable asset or a stable Physics material validation error.
         * @post Failure publishes no partial asset and does not retain the descriptor.
         */
        [[nodiscard]] static Result<PhysicsMaterialAsset> Create(const PhysicsMaterialAssetDescriptor &descriptor);

        /** @brief Checks whether this value contains a validated asset. @return True for a valid snapshot. */
        [[nodiscard]] bool IsValid() const noexcept {
            return descriptor_.id.IsValid() && descriptor_.sourceRevision != 0;
        }

        /** @brief Returns the stable asset identity. @pre IsValid is true. */
        [[nodiscard]] const PhysicsMaterialAssetId &Id() const noexcept {
            return descriptor_.id;
        }

        /** @brief Returns the captured source revision. @pre IsValid is true. */
        [[nodiscard]] std::uint64_t SourceRevision() const noexcept {
            return descriptor_.sourceRevision;
        }

        /** @brief Returns the captured friction coefficient. @pre IsValid is true. */
        [[nodiscard]] float Friction() const noexcept {
            return descriptor_.friction;
        }

        /** @brief Returns the captured restitution coefficient. @pre IsValid is true. */
        [[nodiscard]] float Restitution() const noexcept {
            return descriptor_.restitution;
        }

        /** @brief Returns the captured density in kilograms per cubic meter. @pre IsValid is true. */
        [[nodiscard]] float DensityKilogramsPerCubicMeter() const noexcept {
            return descriptor_.densityKilogramsPerCubicMeter;
        }

        /** @brief Returns the independent friction combine policy. @pre IsValid is true. */
        [[nodiscard]] PhysicsMaterialCombineMode FrictionCombine() const noexcept {
            return descriptor_.frictionCombine;
        }

        /** @brief Returns the independent restitution combine policy. @pre IsValid is true. */
        [[nodiscard]] PhysicsMaterialCombineMode RestitutionCombine() const noexcept {
            return descriptor_.restitutionCombine;
        }

        /** @brief Returns the owned immutable descriptor snapshot. @pre IsValid is true. */
        [[nodiscard]] const PhysicsMaterialAssetDescriptor &Descriptor() const noexcept {
            return descriptor_;
        }

        [[nodiscard]] constexpr auto operator<=>(const PhysicsMaterialAsset &) const noexcept = default;

    private:
        explicit constexpr PhysicsMaterialAsset(const PhysicsMaterialAssetDescriptor &descriptor) noexcept : descriptor_(descriptor) {}

        PhysicsMaterialAssetDescriptor descriptor_{};
    };

    /**
     * @brief Combines contact coefficients using the deterministic Horo policy vocabulary.
     * @param left First immutable physical-material asset.
     * @param right Second immutable physical-material asset.
     * @return Combined friction and restitution, or MaterialDescriptorInvalid for an empty asset.
     * @details When modes differ, the higher explicit policy precedence is selected independently
     * for friction and restitution: Average, Minimum, Multiply, then Maximum. Density is not a
     * contact coefficient and is intentionally not combined.
     */
    [[nodiscard]] Result<PhysicsMaterialContactProperties> CombinePhysicsMaterialAssets(const PhysicsMaterialAsset &left,
                                                                                        const PhysicsMaterialAsset &right);
}  // namespace Horo::Physics
