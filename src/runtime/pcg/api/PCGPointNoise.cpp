#include "Horo/PCG/PCGErrors.h"
#include "PCGPointTransformationInternal.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Horo::PCG::detail {
    namespace {
        using Coordinate = std::array<double, 3>;
        using Cell = std::array<std::int64_t, 3>;

        /** @brief Fixed unsigned mixing; signed lattice coordinates convert modulo 2^64. */
        std::uint64_t Mix(std::uint64_t value) {
            value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
            return value ^ (value >> 31);
        }

        /** @brief Selects one of twelve fixed diagonal gradients without a mutable permutation table. */
        double Gradient(const Cell &cell, const Coordinate &offset, std::uint64_t seed) {
            constexpr std::array<std::array<int, 3>, 12> gradients{{{1, 1, 0},
                                                                    {-1, 1, 0},
                                                                    {1, -1, 0},
                                                                    {-1, -1, 0},
                                                                    {1, 0, 1},
                                                                    {-1, 0, 1},
                                                                    {1, 0, -1},
                                                                    {-1, 0, -1},
                                                                    {0, 1, 1},
                                                                    {0, -1, 1},
                                                                    {0, 1, -1},
                                                                    {0, -1, -1}}};
            auto hash = seed;
            for (auto axis : cell)
                hash = Mix(hash ^ Mix(static_cast<std::uint64_t>(axis) + 0x9e3779b97f4a7c15ULL));
            const auto &g = gradients[hash % gradients.size()];
            return g[0] * offset[0] + g[1] * offset[1] + g[2] * offset[2];
        }

        /** @brief Quintic fade has zero first and second derivatives at lattice boundaries. */
        double Fade(double t) {
            return t * t * t * (t * (t * 6 - 15) + 10);
        }

        /** @brief Three-dimensional Perlin interpolation over eight fixed lattice corners. */
        double Perlin(const Coordinate &p, std::uint64_t seed) {
            Cell base{};
            Coordinate fraction{};
            Coordinate fade{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                base[axis] = static_cast<std::int64_t>(std::floor(p[axis]));
                fraction[axis] = p[axis] - static_cast<double>(base[axis]);
                fade[axis] = Fade(fraction[axis]);
            }
            std::array<double, 8> corners{};
            for (std::size_t i = 0; i < 8; ++i) {
                Cell cell = base;
                Coordinate offset = fraction;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const auto step = static_cast<std::int64_t>((i >> axis) & 1U);
                    cell[axis] += step;
                    offset[axis] -= static_cast<double>(step);
                }
                corners[i] = Gradient(cell, offset, seed);
            }
            const double low = std::lerp(std::lerp(corners[0], corners[1], fade[0]), std::lerp(corners[2], corners[3], fade[0]), fade[1]);
            const double high = std::lerp(std::lerp(corners[4], corners[5], fade[0]), std::lerp(corners[6], corners[7], fade[0]), fade[1]);
            return std::lerp(low, high, fade[2]);
        }

        /** @brief Three-dimensional Simplex contribution, zero beyond the compact spherical support. */
        double Contribution(const Cell &cell, const Coordinate &offset, std::uint64_t seed) {
            double attenuation = 0.6;
            for (auto value : offset)
                attenuation -= value * value;
            if (attenuation <= 0)
                return 0;
            attenuation *= attenuation;
            return attenuation * attenuation * Gradient(cell, offset, seed);
        }

        /** @brief Selects only valid axes with deterministic x/y/z precedence at every coordinate tie. */
        std::array<std::size_t, 3> SimplexOrder(const Coordinate &offset) {
            if (offset[0] >= offset[1]) {
                if (offset[1] >= offset[2])
                    return {0, 1, 2};
                if (offset[0] >= offset[2])
                    return {0, 2, 1};
                return {2, 0, 1};
            }
            if (offset[0] >= offset[2])
                return {1, 0, 2};
            if (offset[1] >= offset[2])
                return {1, 2, 0};
            return {2, 1, 0};
        }

        /** @brief Four-corner Simplex skew/unskew with stable x/y/z ordering at coordinate ties. */
        double Simplex(const Coordinate &p, std::uint64_t seed) {
            const double skew = (p[0] + p[1] + p[2]) / 3;
            Cell base{};
            for (std::size_t axis = 0; axis < 3; ++axis)
                base[axis] = static_cast<std::int64_t>(std::floor(p[axis] + skew));
            const double unskew = static_cast<double>(base[0] + base[1] + base[2]) / 6;
            Coordinate offset{};
            for (std::size_t axis = 0; axis < 3; ++axis)
                offset[axis] = p[axis] - static_cast<double>(base[axis]) + unskew;
            const auto order = SimplexOrder(offset);
            Cell step{};
            double sum{};
            for (std::size_t corner = 0; corner < 4; ++corner) {
                if (corner != 0)
                    ++step[order[corner - 1]];
                Cell cell = base;
                Coordinate delta = offset;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    cell[axis] += step[axis];
                    delta[axis] -= static_cast<double>(step[axis]);
                    delta[axis] += static_cast<double>(corner) / 6;
                }
                sum += Contribution(cell, delta, seed);
            }
            return 32 * sum;
        }

        /** @brief Bounds every octave coordinate before floor or signed integer conversion. */
        bool Domain(const Coordinate &p) {
            return std::ranges::all_of(p, [](double value) {
                return std::isfinite(value) && std::abs(value) <= 1048576;
            });
        }
    }  // namespace

    /** @copydoc PointNoise */
    Result<std::array<double, 4>> PointNoise(const PCGPointNoiseSettings &s, PCGCpuNodeKind kind, Math::Vec3 position,
                                             std::uint64_t fieldSeed, CancellationToken cancellation) {
        if ((kind != PCGCpuNodeKind::PerlinNoise && kind != PCGCpuNodeKind::SimplexNoise) || s.type < PCGAttributeType::Scalar ||
            s.type > PCGAttributeType::Vector4)
            return Result<std::array<double, 4>>::Failure(MakeError(PCGErrors::CpuEvaluationUnsupported));
        std::array<double, 4> result{};
        const std::size_t channels = static_cast<std::size_t>(s.type) - static_cast<std::size_t>(PCGAttributeType::Scalar) + 1;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            double frequency = s.frequency;
            double weight = 1;
            double total{};
            double denominator{};
            const auto seed = Mix(fieldSeed ^ s.salt ^ Mix(channel + 0x9e3779b97f4a7c15ULL));
            for (std::uint32_t octave = 0; octave < s.octaves; ++octave) {
                if (cancellation.IsCancellationRequested())
                    return Result<std::array<double, 4>>::Failure(MakeError(PCGErrors::CpuEvaluationClosed));
                const Coordinate p{position.x * frequency, position.y * frequency, position.z * frequency};
                if (!Domain(p))
                    return Result<std::array<double, 4>>::Failure(MakeError(PCGErrors::PointDataInvalid));
                total += weight * (kind == PCGCpuNodeKind::PerlinNoise ? Perlin(p, seed) : Simplex(p, seed));
                denominator += weight;
                frequency *= s.lacunarity;
                weight *= s.persistence;
            }
            const double normalized = std::clamp(total / denominator, -1.0, 1.0);
            result[channel] = (s.normalization == PCGPointNoiseNormalization::Signed ? normalized : (normalized + 1) * 0.5) * s.amplitude;
            if (!std::isfinite(result[channel]))
                return Result<std::array<double, 4>>::Failure(MakeError(PCGErrors::PointDataInvalid));
        }
        return Result<std::array<double, 4>>::Success(result);
    }

    /** @copydoc WritePointNoise */
    Result<void> WritePointNoise(const PCGPointNoiseSettings &s, const PCGPointWriteView &writer, std::size_t index,
                                 const std::array<double, 4> &v) {
        switch (s.type) {
            using enum PCGAttributeType;
            case Scalar:
                return writer.SetColumnValue<PCGScalarColumn>(s.attribute.Value(), index, v[0]);
            case Vector2:
                return writer.SetColumnValue<PCGVector2Column>(s.attribute.Value(), index,
                                                               {static_cast<float>(v[0]), static_cast<float>(v[1])});
            case Vector3:
                return writer.SetColumnValue<PCGVector3Column>(s.attribute.Value(), index,
                                                               {static_cast<float>(v[0]), static_cast<float>(v[1]),
                                                                static_cast<float>(v[2])});
            case Vector4:
                return writer.SetColumnValue<PCGVector4Column>(s.attribute.Value(), index,
                                                               {static_cast<float>(v[0]), static_cast<float>(v[1]),
                                                                static_cast<float>(v[2]), static_cast<float>(v[3])});
            default:
                return Result<void>::Failure(MakeError(PCGErrors::CpuEvaluationUnsupported));
        }
    }
}  // namespace Horo::PCG::detail
