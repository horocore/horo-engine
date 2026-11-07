#include "VfxGpuSimulationInternal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace Horo::Render::VfxGpuDetail {
    namespace {
        using Force = Vfx::CpuParticleForceModule;

        /** @brief Reads an admitted whole-word address; cold callers validate the complete span before entering loops. */
        std::uint32_t Word(const VfxGpuSimulationPreparation &p, std::uint32_t stream, std::uint32_t slot) noexcept {
            return p.initialWords[std::size_t{stream} * p.descriptor.layout.streamStrideWords + slot];
        }

        /** @brief Reads canonical float bits without native structure or pointer casts. */
        float Value(const VfxGpuSimulationPreparation &p, std::uint32_t stream, std::uint32_t slot) noexcept {
            return std::bit_cast<float>(Word(p, stream, slot));
        }

        /** @brief Rejects raw enum/channel truncation and reserved-word ambiguity before constructing the shared carrier. */
        bool DecodeForce(std::span<const std::uint32_t> w, Force &force) noexcept {
            if (w[0] >= static_cast<std::uint32_t>(Vfx::CpuParticleForceKind::Count) || w[1] > 255 || w[11])
                return false;
            force = {static_cast<Vfx::CpuParticleForceKind>(w[0]),
                     {std::bit_cast<float>(w[4]), std::bit_cast<float>(w[5]), std::bit_cast<float>(w[6])},
                     {std::bit_cast<float>(w[8]), std::bit_cast<float>(w[9]), std::bit_cast<float>(w[10])},
                     std::bit_cast<float>(w[2]),
                     std::bit_cast<float>(w[3]),
                     std::bit_cast<float>(w[7]),
                     static_cast<std::uint8_t>(w[1])};
            return true;
        }

        struct Bounds {
            double position{}, velocity{}, size{}, rotation{}, angular{}, age{};
        };

        /** @brief Updates one scalar bound using exact finite source floats. */
        void Bound(double &current, float value) noexcept {
            current = std::max(current, std::abs(double{value}));
        }

        /** @brief Checks finite semantic attributes and positive size/lifetime before recording seed bounds. */
        bool SeedValues(const VfxGpuSimulationPreparation &p, std::uint32_t slot, Bounds &bounds) noexcept {
            for (std::uint32_t stream = 0; stream < 12; ++stream)
                if (!std::isfinite(Value(p, stream, slot)))
                    return false;
            for (std::uint32_t stream = 18; stream < p.descriptor.layout.streamCount; ++stream)
                if (!std::isfinite(Value(p, stream, slot)))
                    return false;
            if (Value(p, 6, slot) <= 0 || Value(p, 7, slot) <= 0 || Value(p, 10, slot) < 0 || Value(p, 11, slot) <= 0)
                return false;
            for (std::uint32_t stream = 0; stream < 3; ++stream) {
                Bound(bounds.position, Value(p, stream, slot));
                Bound(bounds.velocity, Value(p, stream + 3, slot));
            }
            Bound(bounds.size, Value(p, 6, slot));
            Bound(bounds.size, Value(p, 7, slot));
            Bound(bounds.rotation, Value(p, 8, slot));
            Bound(bounds.angular, Value(p, 9, slot));
            Bound(bounds.age, Value(p, 10, slot));
            return true;
        }

        /** @brief Enforces canonical zero for every unused or padding lane, not only its Alive word. */
        bool ZeroSlot(const VfxGpuSimulationPreparation &p, std::uint32_t slot) noexcept {
            for (std::uint32_t stream = 0; stream < p.descriptor.layout.streamCount; ++stream)
                if (Word(p, stream, slot))
                    return false;
            return true;
        }

        /** @brief Validates occupied stable identities independently of recyclable native lane numbers. */
        bool SeedIdentity(const VfxGpuSimulationPreparation &p, std::uint32_t slot, std::uint64_t &previous) noexcept {
            const auto id = std::uint64_t{Word(p, 14, slot)} | (std::uint64_t{Word(p, 15, slot)} << 32);
            if (!id || id <= previous || Word(p, 16, slot) != 1 || Word(p, 17, slot) == 0)
                return false;
            previous = id;
            return true;
        }

        /** @brief Scans bounded actual seed data and canonical padding once at cold admission. */
        bool SeedBounds(const VfxGpuSimulationPreparation &p, Bounds &bounds) noexcept {
            const auto &d = p.descriptor;
            if (!d.layout.IsValid() || p.initialWords.size() != d.layout.wordCount || !d.initialParticleCount ||
                d.initialParticleCount > d.layout.capacity)
                return false;
            std::uint64_t previous{};
            for (std::uint32_t slot = 0; slot < d.initialParticleCount; ++slot)
                if (!SeedIdentity(p, slot, previous) || !SeedValues(p, slot, bounds))
                    return false;
            for (std::uint32_t slot = d.initialParticleCount; slot < d.layout.streamStrideWords; ++slot)
                if (!ZeroSlot(p, slot))
                    return false;
            return true;
        }

        struct ForceBounds {
            double acceleration{}, center{}, falloff{};
        };

        /** @brief Conservative per-component magnitude, preserving all four real force kinds. */
        double AccelerationBound(const Force &f) noexcept {
            if (f.kind == Vfx::CpuParticleForceKind::Attraction)
                return 2 * std::abs(double{f.strength});
            if (f.kind == Vfx::CpuParticleForceKind::Noise)
                return 2 * std::abs(double{f.strength} * f.frequency);
            return 2 * std::max({std::abs(double{f.vector.x} * f.strength), std::abs(double{f.vector.y} * f.strength),
                                 std::abs(double{f.vector.z} * f.strength)});
        }

        /** @brief Bounds ordered finite additions with a deliberately generous float32 roundoff margin. */
        ForceBounds ModuleBounds(std::span<const Force> forces) noexcept {
            ForceBounds result;
            for (const auto &force : forces) {
                result.acceleration += AccelerationBound(force);
                if (force.kind != Vfx::CpuParticleForceKind::Attraction)
                    continue;
                result.center = std::max(
                    {result.center, std::abs(double{force.center.x}), std::abs(double{force.center.y}), std::abs(double{force.center.z})});
                result.falloff = std::max(result.falloff, double{force.falloff});
            }
            result.acceleration *= 2;
            return result;
        }

        /** @brief Proves intermediates, including attraction subtraction/dot/sqrt/attenuation, not just final positions. */
        bool SafeEnvelope(const VfxGpuArithmeticEnvelope &e, const Bounds &seed, const ForceBounds &forces) noexcept {
            constexpr double Safe = double{std::numeric_limits<float>::max()} / 128;
            const auto delta = 4 * (e.maximumPositionMagnitude + forces.center);
            const auto lengthSquared = 3 * delta * delta;
            if (lengthSquared > Safe || forces.falloff * std::sqrt(lengthSquared) > Safe)
                return false;
            const auto duration = 2 * static_cast<double>(e.maximumSteps) * e.maximumDeltaSeconds;
            return e.maximumAccelerationMagnitude <= Safe && e.maximumVelocityMagnitude <= Safe && e.maximumRotationMagnitude <= Safe &&
                   seed.age + duration <= Safe && e.maximumSizeMagnitude <= Safe;
        }
    }  // namespace

    bool DecodeForceWords(const VfxGpuSimulationPreparation &p,
                          std::array<Force, Vfx::ParticleSimulationDataLimits::ForceModules> &forces) noexcept {
        const auto count = p.descriptor.forceCount;
        if (count > forces.size() || p.forceWords.size() != std::size_t{count} * 12)
            return false;
        for (std::size_t index = 0; index < count; ++index)
            if (!DecodeForce(p.forceWords.subspan(index * 12, 12), forces[index]))
                return false;
        return Vfx::ValidateParticleForceModules(std::span{forces}.first(count));
    }
}  // namespace Horo::Render::VfxGpuDetail

namespace Horo::Render {
    /** @copydoc ComputeVfxGpuArithmeticEnvelope */
    Result<VfxGpuArithmeticEnvelope> ComputeVfxGpuArithmeticEnvelope(const VfxGpuSimulationPreparation &p) {
        using namespace VfxGpuDetail;
        const auto &limits = p.descriptor.arithmetic;
        if (!limits.maximumSteps || limits.maximumSteps > 1'000'000 || !std::isfinite(limits.maximumDeltaSeconds) ||
            limits.maximumDeltaSeconds <= 0 || limits.maximumDeltaSeconds > 60)
            return Result<VfxGpuArithmeticEnvelope>::Failure(MakeError(VfxGpuSimulationErrors::InvalidPreparation));
        Bounds seed;
        std::array<Vfx::CpuParticleForceModule, Vfx::ParticleSimulationDataLimits::ForceModules> modules;
        if (!SeedBounds(p, seed) || !DecodeForceWords(p, modules))
            return Result<VfxGpuArithmeticEnvelope>::Failure(MakeError(VfxGpuSimulationErrors::InvalidPreparation));
        const auto forces = ModuleBounds(std::span{modules}.first(p.descriptor.forceCount));
        const double time = 2 * static_cast<double>(limits.maximumSteps) * limits.maximumDeltaSeconds;
        VfxGpuArithmeticEnvelope result{limits.maximumSteps,
                                        limits.maximumDeltaSeconds,
                                        2 * (seed.position + seed.velocity * time + forces.acceleration * time * time),
                                        2 * (seed.velocity + forces.acceleration * time),
                                        forces.acceleration,
                                        2 * (seed.rotation + seed.angular * time),
                                        seed.size};
        if (!SafeEnvelope(result, seed, forces))
            return Result<VfxGpuArithmeticEnvelope>::Failure(MakeError(VfxGpuSimulationErrors::ArithmeticUnsafe));
        return Result<VfxGpuArithmeticEnvelope>::Success(result);
    }
}  // namespace Horo::Render
