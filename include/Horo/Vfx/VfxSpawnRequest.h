#pragma once

/** @file VfxSpawnRequest.h
 * @brief Copied, bounded semantic-free effect commands and allocation-free admission evidence.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Math/WorldCoordinate64.h"
#include "Horo/Vfx/VfxIdentity.h"
#include "Horo/Vfx/VfxQualityPolicy.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace Horo::Vfx {
    /** @brief Compiled ceilings; product preparation may select smaller finite bounds. */
    struct VfxSpawnHardLimits final {
        static constexpr std::uint32_t Parameters = 32;
        static constexpr std::uint32_t Pending = 256;
        static constexpr std::uint32_t Results = 256;
        static constexpr std::uint32_t Effects = 64;
        static constexpr std::uint32_t Layers = 8;
        static constexpr std::uint32_t Producers = 64;
    };

    /** @brief One schema-declared scalar GameplayInput channel, independent of private particle streams. */
    struct VfxParameterValue final {
        std::uint16_t channel{};
        float value{};
        constexpr auto operator<=>(const VfxParameterValue &) const noexcept = default;
    };

    /** @brief Fixed copied scalar schema payload; channels must be strictly increasing. */
    struct VfxParameterBlock final {
        std::uint64_t schema{}; /**< Nonzero schema identity when count is nonzero; zero for the empty schema. */
        std::array<VfxParameterValue, VfxSpawnHardLimits::Parameters> values{};
        std::uint32_t count{};

        /** @brief Borrows the bounded active prefix. @return Empty when the carrier count is malformed. */
        [[nodiscard]] std::span<const VfxParameterValue> Values() const noexcept;
    };

    /** @brief World placement; orientation is a finite unit quaternion and axis scales are positive. */
    struct VfxSpawnTransform final {
        Math::WorldCoordinate64 position;
        Math::Quaternion orientation;
        Math::Vec3 scale{1.0F, 1.0F, 1.0F};
    };

    /** @brief Application-issued source occurrence. Producer sequence increases without wrapping in one Scene incarnation. */
    struct VfxSpawnOccurrence final {
        std::uint64_t producer{};
        std::uint64_t sequence{};
        constexpr auto operator<=>(const VfxSpawnOccurrence &) const noexcept = default;
    };

    /** @brief Fully resolved queue-owned command; no gameplay meanings, provider borrows, callbacks or native handles. */
    struct VfxSpawnRequest final {
        Assets::AssetId effectAsset;
        VfxSpawnTransform transform;
        std::optional<Math::Vec3> impactNormal; /**< Optional unit world-space normal; absence is distinct from an invented direction. */
        float scale{1.0F};
        VfxParameterBlock parameters;
        VfxIdentityScope scene;
        VfxIdentityScope owner;
        std::uint64_t ownerGeneration{};
        std::uint64_t tick{}; /**< One-based owner cutoff tick; zero is reserved before World activation. */
        VfxSpawnOccurrence occurrence;
        std::uint64_t bindingGeneration{};
        std::uint64_t destination{};
        std::uint32_t layerOrdinal{};
        VfxRequirementClass requirement{VfxRequirementClass::Cosmetic};
    };

    /** @brief Closed hot-path outcomes; typed rejection never constructs heap-backed diagnostic strings. */
    enum class VfxSpawnStatus : std::uint8_t {
        Accepted,
        Spawned,
        Duplicate,
        UnknownEvent,
        UnknownTag,
        MissingAsset,
        AssetTypeMismatch,
        AssetUnavailable,
        InvalidRequest,
        InvalidTransform,
        InvalidImpactNormal,
        InvalidParameters,
        ParameterSchemaMismatch,
        PermissionDenied,
        StaleScene,
        StaleOwner,
        StaleBinding,
        OutOfOrder,
        EventQueueFull,
        ResultQueueFull,
        EffectCapacityExceeded,
        ResourceBudgetExceeded,
        UnsupportedRecipe,
        Cancelled,
        SimulationFailed,
        ThreadViolation,
        Reentrant,
        ShutDown,
        Empty
    };

    /** @brief Correlated copied admission/completion evidence, populated without invoking a producer. */
    struct VfxSpawnOutcome final {
        VfxSpawnStatus status{VfxSpawnStatus::Empty};
        VfxSpawnOccurrence occurrence;
        std::uint64_t bindingGeneration{};
        std::uint64_t destination{};
        std::uint32_t layerOrdinal{};
        Assets::AssetId effectAsset;
        EffectSystemId instance;
        const ErrorCodeDescriptor *kernelFailure{}; /**< Optional process-lifetime static kernel descriptor; never an owned diagnostic. */
    };

    /** @brief Validates only copied request representation, never authority or current residency.
     * @param request Untrusted carrier; no field is retained or mutated.
     * @return Accepted for valid representation, otherwise the exact invalid field class.
     */
    [[nodiscard]] VfxSpawnStatus ValidateVfxSpawnRequest(const VfxSpawnRequest &request) noexcept;

    /** @brief Copies command correlation into a nonallocating outcome.
     * @param request Source command. @param status Admission or terminal status. @param instance Issued effect, if any.
     * @return Copied bounded evidence; it grants no spawn authority.
     */
    [[nodiscard]] VfxSpawnOutcome MakeVfxSpawnOutcome(const VfxSpawnRequest &request, VfxSpawnStatus status,
                                                      EffectSystemId instance = {}) noexcept;
}  // namespace Horo::Vfx
