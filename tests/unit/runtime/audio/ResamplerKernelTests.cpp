#include "Horo/Audio/AudioErrors.h"
#include "ResamplerKernel.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <vector>

namespace Horo::Audio::Detail {
    namespace {
        AudioResamplerPlan Plan(const AudioResamplerQuality quality, const std::uint32_t inputRate = 48'000,
                                const std::uint32_t outputRate = 48'000) {
            const auto plan = AudioResamplerPlan::Prepare({.quality = quality,
                                                           .inputRate = inputRate,
                                                           .outputRate = outputRate,
                                                           .channels = 1,
                                                           .maximumOutputFrames = 512},
                                                          {1ULL << 32, 1ULL << 24, 4096});
            REQUIRE(plan.HasValue());
            return plan.Value();
        }

        ResamplerKernel Kernel(const AudioResamplerPlan &plan) {
            auto prepared = ResamplerKernel::Prepare(plan, 32ULL << 20);
            REQUIRE(prepared.HasValue());
            return std::move(prepared).Value();
        }

        TEST_CASE("Coefficient preparation honors exact storage reservation", "[unit][audio][resampler]") {
            const auto plan = Plan(AudioResamplerQuality::Sinc64);
            constexpr std::uint64_t bytes = 1025 * 64 * sizeof(float);
            const auto exact = ResamplerKernel::Prepare(plan, bytes);
            REQUIRE(exact.HasValue());
            REQUIRE(exact.Value().StorageBytes() == bytes);
            const auto shortBudget = ResamplerKernel::Prepare(plan, bytes - 1);
            REQUIRE(shortBudget.HasError());
            REQUIRE(shortBudget.ErrorValue().code.Value() == AudioErrors::ResamplerBudgetExceeded.code.Value());
        }

        TEST_CASE("SIMD kernels match the scalar reference at ring and phase boundaries", "[unit][audio][resampler]") {
            const auto unsupported =
                ResamplerKernel::Prepare(Plan(AudioResamplerQuality::Linear), 32ULL << 20, static_cast<AudioResamplerExecution>(255));
            REQUIRE(unsupported.HasError());
            REQUIRE(unsupported.ErrorValue().code.Value() == AudioErrors::OperationUnsupported.code.Value());
            if (!AudioResampler::SupportsSimd()) {
                REQUIRE(
                    ResamplerKernel::Prepare(Plan(AudioResamplerQuality::Linear), 32ULL << 20, AudioResamplerExecution::Simd).HasError());
                return;
            }
            for (const auto quality : {AudioResamplerQuality::Linear, AudioResamplerQuality::Sinc32, AudioResamplerQuality::Sinc64}) {
                for (const auto inputRate : {44'100U, 96'000U, 384'000U}) {
                    const auto plan = Plan(quality, inputRate);
                    const auto scalar = Kernel(plan);
                    const auto native = ResamplerKernel::Prepare(plan, 32ULL << 20, AudioResamplerExecution::Simd);
                    REQUIRE(native.HasValue());
                    std::vector<float> samples(plan.Taps());
                    for (std::size_t index = 0; index < samples.size(); ++index) {
                        samples[index] = static_cast<float>(std::sin(static_cast<double>(index) * 0.271));
                    }
                    for (const auto oldest : {0U, 1U, plan.Taps() / 2, plan.Taps() - 1}) {
                        for (const double phase : {0.0, 0.123456789, 0.5, std::nextafter(1.0, 0.0)}) {
                            REQUIRE(native.Value().Evaluate(samples, oldest, phase) ==
                                    Catch::Approx(scalar.Evaluate(samples, oldest, phase)).margin(2e-12));
                        }
                    }
                }
            }
        }

        TEST_CASE("Linear kernel preserves endpoints ramps phase interpolation and circular order", "[unit][audio][resampler]") {
            const auto kernel = Kernel(Plan(AudioResamplerQuality::Linear));
            const std::array<float, 2> samples{2.0F, 6.0F};
            for (const double phase : {0.0, 0.125, 0.3456789, 0.75, std::nextafter(1.0, 0.0)}) {
                REQUIRE(kernel.Evaluate(samples, 0, phase) == Catch::Approx(2.0 + 4.0 * phase).margin(1e-12));
                REQUIRE(kernel.Evaluate(samples, 1, phase) == Catch::Approx(6.0 - 4.0 * phase).margin(1e-12));
            }
        }

        TEST_CASE("Every sinc quality preserves DC silence and circular sample identity", "[unit][audio][resampler]") {
            for (const auto quality : {AudioResamplerQuality::Sinc32, AudioResamplerQuality::Sinc64}) {
                for (const auto rate : {8'000U, 48'000U, 96'000U, 384'000U}) {
                    const auto plan = Plan(quality, rate);
                    const auto kernel = Kernel(plan);
                    std::vector<float> samples(plan.Taps(), 0.0F);
                    for (const double phase : {0.0, 0.1234567, 0.5, std::nextafter(1.0, 0.0)}) {
                        REQUIRE(kernel.Evaluate(samples, 0, phase) == 0.0);
                    }
                    std::ranges::fill(samples, 1.0F);
                    for (unsigned phase = 0; phase < 1024; ++phase) {
                        REQUIRE(kernel.Evaluate(samples, plan.Taps() - 1, static_cast<double>(phase) / 1024) ==
                                Catch::Approx(1.0).margin(2e-7));
                    }
                    samples[3] = -2.0F;
                    const auto expected = kernel.Evaluate(samples, 0, 0.345);
                    std::ranges::rotate(samples, samples.begin() + 3);
                    REQUIRE(kernel.Evaluate(samples, plan.Taps() - 3, 0.345) == expected);
                }
            }
        }

        TEST_CASE("Sinc bank agrees with independently evaluated double precision windowed impulse", "[unit][audio][resampler]") {
            const auto plan = Plan(AudioResamplerQuality::Sinc64, 96'000);
            const auto kernel = Kernel(plan);
            constexpr double phase = 0.321987;
            const double radius = plan.Taps() / 2.0;
            constexpr double cutoff = 0.45;
            std::vector<double> reference(plan.Taps());
            double normalizer = 0.0;
            for (std::size_t index = 0; index < reference.size(); ++index) {
                const double distance = static_cast<double>(index) - radius + 1.0 - phase;
                const double argument = std::numbers::pi * cutoff * distance;
                const double sinc = argument == 0.0 ? 1.0 : std::sin(argument) / argument;
                reference[index] = std::abs(distance) < radius
                                       ? cutoff * sinc * std::pow(std::cos(std::numbers::pi * distance / (2.0 * radius)), 2.0)
                                       : 0.0;
                normalizer += reference[index];
            }
            std::vector<float> impulse(plan.Taps(), 0.0F);
            for (std::size_t index = 0; index < impulse.size(); ++index) {
                impulse[index] = 1.0F;
                REQUIRE(kernel.Evaluate(impulse, 0, phase) == Catch::Approx(reference[index] / normalizer).margin(2e-7));
                impulse[index] = 0.0F;
            }
        }

        TEST_CASE("Downsampling sinc suppresses an above destination Nyquist tone", "[unit][audio][resampler]") {
            const auto plan = Plan(AudioResamplerQuality::Sinc64, 96'000, 48'000);
            const auto kernel = Kernel(plan);
            std::vector<float> tone(plan.Taps());
            for (const double frequency : {1'000.0, 30'000.0}) {
                double power = 0.0;
                for (unsigned frame = 0; frame < 96; ++frame) {
                    for (std::size_t tap = 0; tap < tone.size(); ++tap) {
                        const double sample = static_cast<double>(frame * 2 + tap) - plan.Taps() / 2.0 + 1.0;
                        tone[tap] = static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * sample / 96'000.0));
                    }
                    const auto value = kernel.Evaluate(tone, 0, 0.0);
                    power += value * value;
                }
                const auto rms = std::sqrt(power / 96.0);
                if (frequency == 1'000.0) {
                    REQUIRE(rms == Catch::Approx(std::sqrt(0.5)).margin(0.002));
                } else {
                    REQUIRE(rms < 0.001);
                }
            }
        }
    }  // namespace
}  // namespace Horo::Audio::Detail
