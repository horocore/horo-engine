#pragma once

#include "Horo/Audio/AudioResampler.h"
#include "ResamplerSimd.h"

#include <memory>
#include <span>

namespace Horo::Audio::Detail {
    /** @brief Immutable control-thread-prepared coefficient bank, privately owned by a render-core generation. */
    class ResamplerKernel final {
    public:
        /**
         * @brief Allocate and prepare all coefficients outside the callback.
         * @param plan Previously admitted conversion and kernel-width metadata.
         * @param maximumBytes Separately reserved coefficient storage, excluding history and I/O.
         * @param execution Explicit implementation selected before publication.
         * @return Prepared kernel or the stable resampler budget failure.
         */
        [[nodiscard]] static Result<ResamplerKernel> Prepare(const AudioResamplerPlan &plan, std::uint64_t maximumBytes,
                                                             AudioResamplerExecution execution = AudioResamplerExecution::Scalar);

        /**
         * @brief Evaluate one channel's circular history without allocation or transcendental functions.
         * @param history Exactly plan.Taps() finite samples, including explicit zero padding.
         * @param oldest Index of the oldest sample; must be less than history.size().
         * @param fraction Finite phase in [0, 1), shared by all channels of an output frame.
         * @return Double-precision accumulated output before the caller's finite/subnormal output policy.
         * @pre This object has not been moved from; all span/index/phase requirements are validated by the caller.
         */
        [[nodiscard]] double Evaluate(std::span<const float> history, std::uint32_t oldest, double fraction) const noexcept;

        /** @brief Get retained coefficient bytes. @return Allocated coefficient payload size, not allocator overhead. */
        [[nodiscard]] std::uint64_t StorageBytes() const noexcept;

    private:
        /** @brief Take exclusive ownership of a complete bank, never an incomplete preparation. */
        explicit ResamplerKernel(std::unique_ptr<float[]> coefficients, std::uint32_t taps, ResamplerEvaluator evaluator);
        std::unique_ptr<float[]> coefficients_;
        std::uint32_t taps_;
        ResamplerEvaluator evaluator_;
    };
}  // namespace Horo::Audio::Detail
