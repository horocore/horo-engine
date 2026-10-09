#pragma once

/**
 * @file ParticleSystemDescriptor.h
 * @brief Versioned, bounded particle-system source validation and cook admission.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/ValidationResult.h"
#include "Horo/Vfx/VfxIdentity.h"
#include "Horo/Vfx/VfxQualityPolicy.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Horo::Vfx {
    /** @brief Current persisted particle-system source schema version. */
    struct ParticleDescriptorSchemaVersion final {
        std::uint16_t major{1}; /**< Breaking schema generation. */
        std::uint16_t minor{};  /**< Backward-migratable schema revision. */
        constexpr auto operator<=>(const ParticleDescriptorSchemaVersion &) const noexcept = default;
    };

    inline constexpr ParticleDescriptorSchemaVersion CurrentParticleDescriptorSchemaVersion{1, 0};

    /** @brief Compatibility classification applied before source payload decoding. */
    enum class ParticleDescriptorCompatibility : std::uint8_t {
        Exact,
        MigrationRequired,
        Unsupported
    };

    /** @brief Compile-time ceilings that project input cannot raise. */
    struct ParticleDescriptorHardLimits final {
        static constexpr std::size_t SourceBytes = 2U * 1024U * 1024U;
        static constexpr std::size_t JsonDepth = 24;
        static constexpr std::uint32_t Particles = 1'000'000;
        static constexpr double SpawnRate = 1'000'000.0;
    };

    /** @brief Project-lowerable parser and semantic limits. */
    struct ParticleDescriptorLimits final {
        std::size_t maximumSourceBytes{ParticleDescriptorHardLimits::SourceBytes}; /**< Maximum accepted UTF-8 source bytes. */
        std::size_t maximumJsonDepth{ParticleDescriptorHardLimits::JsonDepth};     /**< Maximum parser nesting depth. */
        std::uint32_t maximumParticles{ParticleDescriptorHardLimits::Particles};   /**< Maximum admitted capacity. */
        double maximumSpawnRate{ParticleDescriptorHardLimits::SpawnRate};          /**< Maximum particles spawned per second. */
        constexpr auto operator<=>(const ParticleDescriptorLimits &) const noexcept = default;
    };

    /** @brief Backend-neutral authored emitter geometry. */
    enum class ParticleEmitterShape : std::uint8_t {
        Point,
        Sphere,
        Box,
        Cone,
        Count
    };

    /** @brief Backend-neutral particle render topology. */
    enum class ParticleRenderMode : std::uint8_t {
        Billboard,
        Mesh,
        Ribbon,
        Count
    };

    /** @brief Authored sort intent validated independently from renderer algorithms. */
    enum class ParticleSortMode : std::uint8_t {
        None,
        ByDistance,
        OldestFirst,
        Count
    };

    /** @brief Authored collision source; no native physics or renderer type crosses this boundary. */
    enum class ParticleCollisionMode : std::uint8_t {
        None,
        Planes,
        SceneDepth,
        PhysicsWorld,
        Count
    };

    /** @brief Authored response shared by every CPU collision source. */
    enum class ParticleCollisionResponse : std::uint8_t {
        Bounce,
        Die,
        Count
    };

    /** @brief Terminal particle condition required by infinite-lifetime emitters. */
    enum class ParticleKillCondition : std::uint8_t {
        None,
        Lifetime,
        Collision,
        ExplicitSignal,
        Count
    };

    /** @brief Closed lifetime representation; infinity is never encoded as a floating-point sentinel. */
    enum class ParticleLifetimeKind : std::uint8_t {
        Finite,
        Infinite,
        Count
    };

    /** @brief Inclusive scalar interval used by deterministic emitter initialization. */
    struct ParticleScalarRange final {
        double minimum{}; /**< Inclusive lower bound. */
        double maximum{}; /**< Inclusive upper bound. */
        constexpr auto operator<=>(const ParticleScalarRange &) const noexcept = default;
    };

    /** @brief Persisted mutable carrier accepted only through validation. */
    struct ParticleSystemDescriptorData final {
        ParticleDescriptorSchemaVersion version{CurrentParticleDescriptorSchemaVersion}; /**< Persisted schema version. */
        EmitterId emitter; /**< Stable emitter identity within its owning effect. */
        SimulationPreference simulationPreference{SimulationPreference::Automatic}; /**< Authored execution-domain policy. */
        std::uint32_t maximumParticles{};                                           /**< Fixed admitted storage capacity. */
        ParticleEmitterShape shape{ParticleEmitterShape::Point};                    /**< Authored spawn geometry. */
        ParticleScalarRange spawnRate;                                              /**< Particles spawned per second. */
        ParticleLifetimeKind lifetimeKind{ParticleLifetimeKind::Finite};            /**< Closed finite/infinite representation. */
        ParticleScalarRange lifetimeSeconds;                                        /**< Finite seconds or canonical zero range. */
        ParticleKillCondition killCondition{ParticleKillCondition::Lifetime};       /**< Required terminal condition. */
        ParticleScalarRange initialSpeed;                                           /**< Initial scalar speed range. */
        ParticleScalarRange initialSize;                                            /**< Initial positive size range. */
        ParticleScalarRange initialOpacity;                                         /**< Initial opacity inside [0, 1]. */
        Assets::AssetId material;                                                   /**< Stable referenced material identity. */
        ParticleRenderMode renderMode{ParticleRenderMode::Billboard};               /**< Backend-neutral output topology. */
        ParticleSortMode sortMode{ParticleSortMode::ByDistance};                    /**< Authored ordering policy. */
        ParticleCollisionMode collisionMode{ParticleCollisionMode::None};           /**< Authored collision source. */
        ParticleCollisionResponse collisionResponse{
            ParticleCollisionResponse::Bounce}; /**< Optional source field; legacy sources bounce. */
        constexpr bool operator==(const ParticleSystemDescriptorData &) const = default;
    };

    struct ParticleDescriptorValidation;

    /** @brief Immutable particle-system descriptor admitted for cook planning. */
    class ParticleSystemDescriptor final {
    public:
        /** @brief Returns the validated source data. @return Borrowed data owned by this descriptor. */
        [[nodiscard]] const ParticleSystemDescriptorData &Data() const noexcept;

    private:
        explicit ParticleSystemDescriptor(ParticleSystemDescriptorData data) noexcept;
        ParticleSystemDescriptorData data_;

        friend Result<ParticleDescriptorValidation> ValidateParticleSystemDescriptor(ParticleSystemDescriptorData, ErrorCodeRegistry,
                                                                                     const std::string &, const ParticleDescriptorLimits &);
    };

    /** @brief Full-pass import validation outcome with an admitted descriptor only when no error finding exists. */
    struct ParticleDescriptorValidation final {
        ValidationResult diagnostics;                       /**< Deterministically ordered complete findings. */
        std::optional<ParticleSystemDescriptor> descriptor; /**< Present only when no error finding exists. */

        /** @brief Returns whether a descriptor was admitted. @return True only when validation has no error findings. */
        [[nodiscard]] bool Accepted() const noexcept;
    };

    /**
     * @brief Classifies persisted schema compatibility without decoding payload fields.
     * @param version Persisted version to classify.
     * @return Exact for 1.0, MigrationRequired for an older same-major minor, otherwise Unsupported.
     */
    [[nodiscard]] constexpr ParticleDescriptorCompatibility ClassifyParticleDescriptorCompatibility(
        const ParticleDescriptorSchemaVersion version) noexcept {
        if (version == CurrentParticleDescriptorSchemaVersion)
            return ParticleDescriptorCompatibility::Exact;
        if (version.major == CurrentParticleDescriptorSchemaVersion.major && version.minor < CurrentParticleDescriptorSchemaVersion.minor)
            return ParticleDescriptorCompatibility::MigrationRequired;
        return ParticleDescriptorCompatibility::Unsupported;
    }

    /**
     * @brief Parses one strict canonical JSON source under finite byte and depth limits.
     * @param source Complete UTF-8 source bytes.
     * @param limits Active limits bounded by ParticleDescriptorHardLimits.
     * @return Decoded carrier or a typed malformed, duplicate, version, or limit failure.
     */
    [[nodiscard]] Result<ParticleSystemDescriptorData> ParseParticleSystemDescriptor(std::string_view source,
                                                                                     const ParticleDescriptorLimits &limits = {});

    /**
     * @brief Reports all semantic import findings through the registered multi-diagnostic contract.
     * @param data Decoded source carrier to validate and own on success.
     * @param registry Immutable registry containing every VFX validation descriptor.
     * @param sourceName Non-empty stable source context recorded on every finding.
     * @param limits Active semantic ceilings bounded by ParticleDescriptorHardLimits.
     * @return Full-pass findings and an admitted immutable descriptor, or an operation failure when diagnostics cannot be produced.
     */
    [[nodiscard]] Result<ParticleDescriptorValidation> ValidateParticleSystemDescriptor(ParticleSystemDescriptorData data,
                                                                                        ErrorCodeRegistry registry,
                                                                                        const std::string &sourceName = "particle-system",
                                                                                        const ParticleDescriptorLimits &limits = {});

    /** @brief Provider-neutral particle cook tier. */
    enum class ParticleCookTier : std::uint8_t {
        Compact,
        Standard,
        Large,
        Count
    };

    /** @brief Immutable finite ceilings selected for one particle-system cook. */
    struct ParticleCookProfile final {
        ParticleCookTier tier{ParticleCookTier::Compact}; /**< Selected product tier. */
        std::uint32_t maximumParticles{};                 /**< Maximum admitted capacity. */
        double maximumSpawnRate{};                        /**< Maximum admitted particles per second. */
        constexpr auto operator<=>(const ParticleCookProfile &) const noexcept = default;
    };

    /** @brief Exact material evidence captured from one immutable Asset Registry snapshot. */
    enum class ParticleMaterialAvailability : std::uint8_t {
        Available,
        Missing,
        TypeMismatch,
        Unloadable,
        Count
    };

    /** @brief Cook-scoped material lookup result; it owns no provider lease or native material handle. */
    struct ParticleMaterialEvidence final {
        Assets::AssetId material; /**< Exact identity resolved from the captured cook snapshot. */
        ParticleMaterialAvailability availability{ParticleMaterialAvailability::Missing}; /**< Typed lookup result. */
        constexpr auto operator<=>(const ParticleMaterialEvidence &) const noexcept = default;
    };

    /** @brief Deterministic cook admission summary with no publication or runtime ownership. */
    struct ParticleCookPlan final {
        ParticleCookProfile profile;      /**< Validated immutable tier limits. */
        EmitterId emitter;                /**< Stable emitter identity copied from the descriptor. */
        Assets::AssetId material;         /**< Stable material dependency identity. */
        std::uint32_t maximumParticles{}; /**< Fixed cooked capacity. */
        double maximumSpawnRate{};        /**< Fixed cooked spawn-rate ceiling. */
        constexpr auto operator<=>(const ParticleCookPlan &) const noexcept = default;
    };

    /** @brief Full-pass cook validation outcome with a plan only when every requirement is satisfied. */
    struct ParticleCookValidation final {
        ValidationResult diagnostics;         /**< Deterministically ordered complete cook findings. */
        std::optional<ParticleCookPlan> plan; /**< Present only when no error finding exists. */

        /** @brief Returns whether the candidate was admitted. @return True only when validation has no error findings. */
        [[nodiscard]] bool Accepted() const noexcept;
    };

    /** @brief Returns the canonical immutable profile for a known tier. @param tier Requested tier. @return Profile or typed malformed
     * failure. */
    [[nodiscard]] Result<ParticleCookProfile> GetParticleCookProfile(ParticleCookTier tier);

    /**
     * @brief Validates bounded cook limits and the exact material snapshot without publishing output.
     * @param descriptor Previously admitted immutable descriptor.
     * @param profile Explicit cook profile.
     * @param material Exact dependency evidence for the descriptor's material identity.
     * @param registry Immutable registry containing every VFX validation descriptor.
     * @param sourceName Non-empty stable source context recorded on every finding.
     * @return All cook findings and an admitted plan, or an operation failure when diagnostics cannot be produced.
     */
    [[nodiscard]] Result<ParticleCookValidation> BuildParticleSystemCookPlan(const ParticleSystemDescriptor &descriptor,
                                                                             const ParticleCookProfile &profile,
                                                                             const ParticleMaterialEvidence &material,
                                                                             ErrorCodeRegistry registry,
                                                                             const std::string &sourceName = "particle-system");
}  // namespace Horo::Vfx
