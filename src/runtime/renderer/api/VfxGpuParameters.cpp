#include "Horo/Runtime/Render/VfxGpuKernel.h"
#include "VfxGpuSimulationInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace Horo::Render {
    namespace {
        /** @brief Rejects overlapping descriptor/output storage before any canonical word write. */
        bool Aliases(const void *source, std::size_t bytes, std::span<std::uint32_t> destination) noexcept {
            const auto a = reinterpret_cast<std::uintptr_t>(source);
            const auto b = reinterpret_cast<std::uintptr_t>(destination.data());
            return a <= b ? b - a < bytes : a - b < destination.size_bytes();
        }

        /** @brief Writes a portable low/high pair into a fixed stack-owned word block. */
        template <std::size_t N> void Wide(std::array<std::uint32_t, N> &words, std::size_t at, std::uint64_t value) noexcept {
            words[at] = static_cast<std::uint32_t>(value);
            words[at + 1] = static_cast<std::uint32_t>(value >> 32);
        }

        /** @brief Packs only shader-consumed semantic random lanes, never a native/dense index. */
        void PackRandom(std::array<std::uint32_t, VfxGpuDispatchWords::Count> &words, const Vfx::ParticleRandomKey &key) noexcept {
            using namespace VfxGpuDispatchWords;
            Wide(words, EffectSeedLow, key.effectSeed);
            Wide(words, ActivationScopeLow, key.activation.scope);
            words[ActivationSlot] = key.activation.slot;
            words[ActivationGeneration] = key.activation.generation;
            Wide(words, EmitterScopeLow, key.emitter.scope);
            words[EmitterSlot] = key.emitter.slot;
            words[EmitterGeneration] = key.emitter.generation;
        }

        /** @brief Checks all camera/origin components before bounded draw arithmetic. */
        bool Finite(Math::Vec3 value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        /** @brief Bounds one vector by its largest absolute component. */
        double Magnitude(Math::Vec3 value) noexcept {
            return std::max({std::abs(double{value.x}), std::abs(double{value.y}), std::abs(double{value.z})});
        }

        /** @brief Proves billboard and matrix intermediates fit float32 with margin; native viewport admission remains separate. */
        bool SafeView(const VfxGpuSimulationDescriptor &d, const VfxGpuDrawView &v) noexcept {
            if (!v.extent.IsValid() || !Finite(v.cameraRight) || !Finite(v.cameraUp) || !Finite(v.originDelta))
                return false;
            if (!std::ranges::all_of(v.viewProjection.values, [](float x) {
                return std::isfinite(x);
            }))
                return false;
            double matrix{};
            for (const auto x : v.viewProjection.values)
                matrix = std::max(matrix, std::abs(double{x}));
            const auto basis = std::max(Magnitude(v.cameraRight), Magnitude(v.cameraUp));
            const auto world =
                d.arithmetic.maximumPositionMagnitude + Magnitude(v.originDelta) + 4 * basis * d.arithmetic.maximumSizeMagnitude;
            constexpr double Safe = double{std::numeric_limits<float>::max()} / 128;
            return world <= Safe && 4 * matrix * (world + 1) <= Safe;
        }

        /** @brief Writes a float vector and explicit fourth lane without native Vec3 representation. */
        void Vector(std::array<std::uint32_t, VfxGpuDrawWords::Count> &words, std::size_t at, Math::Vec3 value, float fourth) noexcept {
            words[at] = std::bit_cast<std::uint32_t>(value.x);
            words[at + 1] = std::bit_cast<std::uint32_t>(value.y);
            words[at + 2] = std::bit_cast<std::uint32_t>(value.z);
            words[at + 3] = std::bit_cast<std::uint32_t>(fourth);
        }
    }  // namespace

    /** @copydoc PackVfxGpuDispatchParameters */
    bool PackVfxGpuDispatchParameters(const VfxGpuSimulationDescriptor &d, const GpuVfxSimulationWork &w,
                                      std::span<std::uint32_t> output) noexcept {
        using namespace VfxGpuDispatchWords;
        if (output.size() < Count || !ValidateVfxGpuSimulationWork(d, w))
            return false;
        const auto prefix = output.first(Count);
        if (Aliases(&d, sizeof(d), prefix) || Aliases(&w, sizeof(w), prefix))
            return false;
        std::array<std::uint32_t, Count> words{};
        words[Capacity] = d.layout.capacity;
        words[StreamStride] = d.layout.streamStrideWords;
        words[StreamCount] = d.layout.streamCount;
        words[ForceCount] = d.forceCount;
        words[Delta] = std::bit_cast<std::uint32_t>(w.deltaSeconds);
        Wide(words, TickLow, w.step.tick);
        words[ObservationCount] = w.observe ? d.readback.maximumRecords : 0;
        PackRandom(words, d.random);
        Wide(words, SourceGenerationLow, w.step.sourceGeneration);
        Wide(words, TargetGenerationLow, w.step.targetGeneration);
        words[LayoutVersion] = GpuParticleLayout::Version;
        words[RandomVersion] = Vfx::ParticleSimulationDataLimits::RandomAlgorithmVersion;
        std::ranges::copy(words, prefix.begin());
        return true;
    }

    /** @copydoc PackVfxGpuDrawParameters */
    bool PackVfxGpuDrawParameters(const VfxGpuSimulationDescriptor &d, const VfxGpuDrawView &v, std::span<std::uint32_t> output) noexcept {
        using namespace VfxGpuDrawWords;
        if (output.size() < Count || !VfxGpuDetail::ValidDescriptor(d) || !SafeView(d, v))
            return false;
        const auto prefix = output.first(Count);
        if (Aliases(&d, sizeof(d), prefix) || Aliases(&v, sizeof(v), prefix))
            return false;
        std::array<std::uint32_t, Count> words;
        // Raw column-native bytes match the canonical row_major HLSL + mul(rowVector,matrix) route.
        // Unlike MaterialParameterValue these are already final bytes, not row-major scalar input components.
        for (std::size_t i = 0; i < 16; ++i)
            words[i] = std::bit_cast<std::uint32_t>(v.viewProjection.values[i]);
        Vector(words, CameraRight, v.cameraRight, static_cast<float>(v.extent.width));
        Vector(words, CameraUp, v.cameraUp, static_cast<float>(v.extent.height));
        Vector(words, OriginDelta, v.originDelta, 0);
        words[Layout] = d.layout.capacity;
        words[Layout + 1] = d.layout.streamStrideWords;
        words[Layout + 2] = d.layout.streamCount;
        words[Layout + 3] = GpuParticleLayout::Version;
        std::ranges::copy(words, prefix.begin());
        return true;
    }
}  // namespace Horo::Render
