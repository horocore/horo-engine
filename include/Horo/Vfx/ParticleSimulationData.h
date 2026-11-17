#pragma once
/** @file ParticleSimulationData.h
 * @brief Canonical force descriptors and versioned counter-based random data shared by CPU and renderer simulation.
 */
#include "Horo/Math/SceneMath.h"

#include <array>
#include <cstdint>
#include <span>

namespace Horo::Vfx {
    /** @brief Shared force and random-algorithm ceilings; product policy may only lower work limits. */
    struct ParticleSimulationDataLimits final {
        static constexpr std::uint32_t ForceModules = 64;          /**< Maximum ordered force descriptors. */
        static constexpr std::uint32_t RandomAlgorithmVersion = 1; /**< Canonical full-simulation counter algorithm. */
    };

    /** @brief Fixed logical order of one committed CPU simulation tick. */
    enum class CpuParticleStage : std::uint8_t {
        Spawn,
        Initialize,
        Forces,
        Integrate,
        Collide,
        Kill,
        Extract,
        Count,
    };

    inline constexpr std::array<CpuParticleStage, 7> CpuParticleStageOrder{CpuParticleStage::Spawn,   CpuParticleStage::Initialize,
                                                                           CpuParticleStage::Forces,  CpuParticleStage::Integrate,
                                                                           CpuParticleStage::Collide, CpuParticleStage::Kill,
                                                                           CpuParticleStage::Extract};

    /** @brief Force kernel selected by a compiled CPU simulation plan. */
    enum class CpuParticleForceKind : std::uint8_t {
        Gravity,
        Wind,
        Attraction,
        Noise,
        Count,
    };

    /**
     * @brief Immutable force parameters captured during simulation preparation.
     *
     * Gravity and Wind use `vector * strength`. Attraction points at `center`; `falloff` is
     * the distance attenuation coefficient. Noise uses the versioned per-particle counter stream;
     * `strength * frequency` scales its acceleration. Noise channels 1-9 are reserved for
     * initialization, and each noise module in a stack requires a distinct channel.
     */
    struct CpuParticleForceModule final {
        CpuParticleForceKind kind{CpuParticleForceKind::Gravity};
        Math::Vec3 vector{};
        Math::Vec3 center{};
        float strength{1.0F};
        float falloff{};
        float frequency{1.0F};
        std::uint8_t randomChannel{16};
    };

    /** @brief Value projection of one stable activation or emitter identity, never a native storage index. */
    struct ParticleRandomIdentity final {
        std::uint64_t scope{};
        std::uint32_t slot{};
        std::uint32_t generation{};
    };

    /** @brief Immutable semantic identity tuple shared by every particle stream in one emitter activation. */
    struct ParticleRandomKey final {
        std::uint64_t effectSeed{};
        ParticleRandomIdentity activation;
        ParticleRandomIdentity emitter;
    };

    /** @brief Counter tuple for one random sample; native/dense indices and completion order are never inputs. */
    struct ParticleRandomSample final {
        std::uint64_t particle{};
        std::uint32_t channel{};
        std::uint32_t sampleOrdinal{};
        std::uint64_t tick{};
    };

    /** @brief Validates bounded ordered force descriptors without allocation, registration or error construction.
     * @param forces Immutable force stack; noise channels must be distinct and at least ten.
     * @return True only for a finite stack within the shared ceiling, including finite constant/noise amplitude products.
     */
    [[nodiscard]] bool ValidateParticleForceModules(std::span<const CpuParticleForceModule> forces) noexcept;

    /** @brief Computes the existing 48-bit initial identity base using the canonical activation/emitter key order.
     * @param key Captured semantic identity values; identity admission remains the caller's responsibility.
     * @return Exact masked counter-hash bits. The CPU owner retains its existing zero-base/first-birth policy.
     */
    [[nodiscard]] std::uint64_t ComputeParticleIdentityBase(const ParticleRandomKey &key) noexcept;

    /** @brief Computes canonical full-simulation RNG version one with defined unsigned 64-bit wrap arithmetic.
     * @param key Captured effect/activation/emitter tuple, combined in declaration order.
     * @param sample Stable particle, channel, sample ordinal and logical tick, followed by algorithm version one.
     * @return Exact counter-hash bits; this pure mapping neither admits identities nor mutates a generator.
     * @note Legacy CpuParticleSpawnPipeline uses a distinct existing stream and is not this simulation/GPU reference.
     */
    [[nodiscard]] std::uint64_t ComputeParticleRandomWord(const ParticleRandomKey &key, const ParticleRandomSample &sample) noexcept;

    /** @brief Converts the top 24 random bits to the canonical exactly representable float interval [0,1).
     * @param word Exact counter-hash output.
     * @return Float(top24bits) / 2^24, without a standard-library distribution or mutable state.
     */
    [[nodiscard]] float ParticleRandomUnitFloat(std::uint64_t word) noexcept;
}  // namespace Horo::Vfx
