#include "Horo/Vfx/ParticleSimulationData.h"

#include <cmath>

namespace Horo::Vfx {
    namespace {
        /** @brief Checks vector components before any force arithmetic. */
        bool Finite(const Math::Vec3 value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        /** @brief Checks descriptor representation independently of force-specific arithmetic. */
        bool FiniteForce(const CpuParticleForceModule &force) noexcept {
            return force.kind < CpuParticleForceKind::Count && Finite(force.vector) && Finite(force.center) &&
                   std::isfinite(force.strength) && std::isfinite(force.falloff) && std::isfinite(force.frequency);
        }

        /** @brief Preserves the CPU force acceptance rules without choosing an execution domain. */
        bool ValidForce(const CpuParticleForceModule &force) noexcept {
            using enum CpuParticleForceKind;
            if (!FiniteForce(force) || force.falloff < 0.0F || force.frequency < 0.0F)
                return false;
            if (force.kind == Noise)
                return force.randomChannel >= 10 && std::isfinite(force.strength * force.frequency);
            if (force.kind == Gravity || force.kind == Wind)
                return Finite(force.vector * force.strength);
            return true;
        }

        /** @brief Applies the exact existing full-simulation unsigned mixing permutation. */
        std::uint64_t Mix(std::uint64_t value) noexcept {
            value += 0x9E3779B97F4A7C15ULL;
            value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
            value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
            return value ^ (value >> 31U);
        }

        /** @brief Combines one semantic key lane using defined unsigned wrap arithmetic. */
        void HashCombine(std::uint64_t &hash, const std::uint64_t value) noexcept {
            hash ^= Mix(value + 0x9E3779B97F4A7C15ULL + (hash << 6U) + (hash >> 2U));
        }

        /** @brief Combines identity dimensions in the pre-extraction reference order. */
        void CombineIdentity(std::uint64_t &hash, const ParticleRandomIdentity &identity) noexcept {
            HashCombine(hash, identity.scope);
            HashCombine(hash, identity.slot);
            HashCombine(hash, identity.generation);
        }

        /** @brief Produces the common seed/activation/emitter prefix for identity and random samples. */
        std::uint64_t IdentityHash(const ParticleRandomKey &key) noexcept {
            auto hash = Mix(key.effectSeed);
            CombineIdentity(hash, key.activation);
            CombineIdentity(hash, key.emitter);
            return hash;
        }
    }  // namespace

    /** @copydoc ValidateParticleForceModules */
    bool ValidateParticleForceModules(const std::span<const CpuParticleForceModule> forces) noexcept {
        if (forces.size() > ParticleSimulationDataLimits::ForceModules)
            return false;
        std::array<bool, 256> noiseChannelSeen{};
        for (const auto &force : forces) {
            if (!ValidForce(force))
                return false;
            if (force.kind == CpuParticleForceKind::Noise) {
                if (noiseChannelSeen[force.randomChannel])
                    return false;
                noiseChannelSeen[force.randomChannel] = true;
            }
        }
        return true;
    }

    /** @copydoc ComputeParticleIdentityBase */
    std::uint64_t ComputeParticleIdentityBase(const ParticleRandomKey &key) noexcept {
        return IdentityHash(key) & 0x0000FFFFFFFFFFFFULL;
    }

    /** @copydoc ComputeParticleRandomWord */
    std::uint64_t ComputeParticleRandomWord(const ParticleRandomKey &key, const ParticleRandomSample &sample) noexcept {
        auto hash = IdentityHash(key);
        HashCombine(hash, sample.particle);
        HashCombine(hash, sample.channel);
        HashCombine(hash, sample.sampleOrdinal);
        HashCombine(hash, sample.tick);
        HashCombine(hash, ParticleSimulationDataLimits::RandomAlgorithmVersion);
        return hash;
    }

    /** @copydoc ParticleRandomUnitFloat */
    float ParticleRandomUnitFloat(const std::uint64_t word) noexcept {
        constexpr float Scale = 1.0F / 16'777'216.0F;
        return static_cast<float>(word >> 40U) * Scale;
    }
}  // namespace Horo::Vfx
