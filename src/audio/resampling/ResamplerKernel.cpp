#include "ResamplerKernel.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>
#include <utility>

namespace Horo::Audio::Detail {
    namespace {
        constexpr std::uint32_t PhaseIntervals = 1024;

        /** @brief Scalar reference with chronological accumulation independent of physical ring rotation. */
        double EvaluateScalar(const std::span<const float> history, std::uint32_t oldest, const float *lower, const float *upper,
                              const double blend) noexcept {
            double value = 0.0;
            std::ranges::for_each(std::views::iota(std::size_t{0}, history.size()), [&](const std::size_t tap) {
                const double weight = std::lerp(static_cast<double>(lower[tap]), static_cast<double>(upper[tap]), blend);
                value += history[oldest] * weight;
                ++oldest;
                if (oldest == history.size()) {
                    oldest = 0;
                }
            });
            return value;
        }

        /** @brief Sample a finite Hann-windowed low-pass sinc; all transcendental work is control-thread-only. */
        double WindowedSinc(const double distance, const double radius, const double cutoff) {
            if (std::abs(distance) >= radius) {
                return 0.0;
            }
            const double argument = std::numbers::pi * distance * cutoff;
            const double sinc = argument == 0.0 ? 1.0 : std::sin(argument) / argument;
            return cutoff * sinc * (0.5 + 0.5 * std::cos(std::numbers::pi * distance / radius));
        }

        /** @brief Prepare one phase with unity DC gain, including the endpoint row used for interpolation. */
        void PreparePhase(const std::span<float> row, const double fraction, const double cutoff) {
            const double radius = static_cast<double>(row.size()) / 2.0;
            double total = 0.0;
            std::ranges::for_each(std::views::iota(std::size_t{0}, row.size()), [&](const std::size_t tap) {
                const double distance = static_cast<double>(tap) - radius + 1.0 - fraction;
                row[tap] = static_cast<float>(WindowedSinc(distance, radius, cutoff));
                total += row[tap];
            });
            std::ranges::for_each(row, [total](auto &coefficient) {
                coefficient = static_cast<float>(coefficient / total);
            });
        }
    }  // namespace

    /** @copydoc ResamplerKernel::Prepare */
    Result<ResamplerKernel> ResamplerKernel::Prepare(const AudioResamplerPlan &plan, const std::uint64_t maximumBytes,
                                                     const AudioResamplerExecution execution) {
        ResamplerEvaluator evaluator;
        switch (execution) {
            case AudioResamplerExecution::Scalar:
                evaluator = EvaluateScalar;
                break;
            case AudioResamplerExecution::Simd:
                evaluator = NativeResamplerEvaluator();
                break;
        }
        if (!evaluator) {
            return Result<ResamplerKernel>::Failure(MakeError(AudioErrors::OperationUnsupported));
        }
        const auto count = static_cast<std::uint64_t>(PhaseIntervals + 1) * plan.Taps();
        if (count * sizeof(float) > maximumBytes) {
            return Result<ResamplerKernel>::Failure(MakeError(AudioErrors::ResamplerBudgetExceeded));
        }
        auto coefficients = std::make_unique<float[]>(static_cast<std::size_t>(count));
        const double cutoff = 0.9 / std::fmax(1.0, plan.InputStep());
        std::ranges::for_each(std::views::iota(std::uint32_t{0}, PhaseIntervals + 1), [&](const std::uint32_t phase) {
            const double fraction = static_cast<double>(phase) / PhaseIntervals;
            const auto row =
                std::span{coefficients.get(), static_cast<std::size_t>(count)}.subspan(static_cast<std::size_t>(phase) * plan.Taps(),
                                                                                       plan.Taps());
            if (plan.Descriptor().quality == AudioResamplerQuality::Linear) {
                row[0] = static_cast<float>(1.0 - fraction);
                row[1] = static_cast<float>(fraction);
            } else {
                PreparePhase(row, fraction, cutoff);
            }
        });
        return Result<ResamplerKernel>::Success(ResamplerKernel{std::move(coefficients), plan.Taps(), evaluator});
    }

    /** @copydoc ResamplerKernel::Evaluate */
    double ResamplerKernel::Evaluate(const std::span<const float> history, std::uint32_t oldest, const double fraction) const noexcept {
        const double position = fraction * PhaseIntervals;
        const auto phase = static_cast<std::uint32_t>(position);
        const double blend = position - phase;
        const auto offset = static_cast<std::size_t>(phase) * taps_;
        return evaluator_(history, oldest, coefficients_.get() + offset, coefficients_.get() + offset + taps_, blend);
    }

    /** @copydoc ResamplerKernel::StorageBytes */
    std::uint64_t ResamplerKernel::StorageBytes() const noexcept {
        return coefficients_ ? static_cast<std::uint64_t>(PhaseIntervals + 1) * taps_ * sizeof(float) : 0;
    }

    /** @copydoc ResamplerKernel::ResamplerKernel */
    ResamplerKernel::ResamplerKernel(std::unique_ptr<float[]> coefficients, const std::uint32_t taps, ResamplerEvaluator evaluator)
        : coefficients_(std::move(coefficients)), taps_(taps), evaluator_(std::move(evaluator)) {}
}  // namespace Horo::Audio::Detail
